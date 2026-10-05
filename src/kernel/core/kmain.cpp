#include <boot/boot_info.h>
#include <drivers/acpi/acpi.h>
#include <drivers/bus/pci/pci.h>
#include <drivers/bus/usb/usb.h>
#include <drivers/class/hid/input.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <drivers/class/hid/usb_hid.h>
#include <drivers/rtc/rtc.h>
#include <drivers/sound/sound.h>
#include <drivers/storage/ahci.h>
#include <drivers/storage/ata.h>
#include <drivers/video/display.h>
#include <drivers/video/framebuffer.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/pat.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/boot_display_timing.h>
#include <kernel/boot_splash.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/event.h>
#include <kernel/fs/block_dev.h>
#include <kernel/fs/fat32.h>
#include <kernel/fs/partition.h>
#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/volume.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/net/arp.h>
#include <kernel/net/dns.h>
#include <kernel/net/icmp.h>
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>
#include <kernel/sync/futex.h>
#include <kernel/terminal.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <stddef.h>
#include <stdint.h>

const BootFramebuffer *g_framebuffer = nullptr;
static char g_bootloader_name_buf[64];
static char g_bootloader_version_buf[64];
const char *g_bootloader_name = nullptr;
const char *g_bootloader_version = nullptr;
static bool g_vol_mount_ready = false;
static BlockDevice *g_data_device = nullptr;
static constexpr const char *DATA_VOLUME_LABEL = "UNI_DATA";
static constexpr const char *BOOT_VOLUME_LABEL = "UNI_OS";

extern void call_global_constructors();
extern void apic_init();

#ifdef DEBUG
static uint64_t g_boot_timer_epoch = 0;

static void boot_timing_start()
{
    g_boot_timer_epoch = timer_get_ticks();
}

static uint64_t boot_timing_elapsed_ms()
{
    uint64_t now = timer_get_ticks();
    return now >= g_boot_timer_epoch ? now - g_boot_timer_epoch : 0;
}

static void boot_timing_log(const char *phase)
{
    BOOT_LOG("Boot timing: %s at %llu ms", phase ? phase : "phase", (unsigned long long)boot_timing_elapsed_ms());
}
#else
static void boot_timing_start()
{
}
static void boot_timing_log(const char *)
{
}
#endif

static void idle_task_entry()
{
    asm volatile("sti");
    while (true)
        asm volatile("hlt");
}

static void copy_boot_string(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    if (!src) {
        out[0] = '\0';
        return;
    }
    kstring::strncpy(out, src, out_size - 1);
    out[out_size - 1] = '\0';
}

static uint64_t framebuffer_span_bytes(const BootFramebuffer *fb)
{
    if (!fb || !fb->address || fb->pitch == 0 || fb->height == 0)
        return 0;
    return static_cast<uint64_t>(fb->pitch) * static_cast<uint64_t>(fb->height);
}

static bool ensure_directory(const char *path)
{
    if (!path || !*path)
        return false;

    VNodeStat st = {};
    if (vfs_stat(path, &st) == 0)
        return st.is_dir;

    if (vfs_mkdir(path) != 0)
        return false;

    return vfs_stat(path, &st) == 0 && st.is_dir;
}

static bool fat32_label_matches(const char *actual, const char *expected)
{
    return expected && actual && kstring::strcmp(actual, expected) == 0;
}

static bool fat32_label_is_boot_volume(const char *label)
{
    return fat32_label_matches(label, BOOT_VOLUME_LABEL);
}

static void sanitize_volume_name(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    size_t pos = 0;
    bool last_dash = false;
    if (!src)
        src = "";

    while (*src && pos + 1 < out_size) {
        char c = *src++;
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (valid) {
            out[pos++] = c;
            last_dash = false;
            continue;
        }
        if (!last_dash && pos > 0) {
            out[pos++] = '-';
            last_dash = true;
        }
    }
    while (pos > 0 && out[pos - 1] == '-')
        pos--;
    if (pos == 0) {
        kstring::strncpy(out, "volume", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    out[pos] = '\0';
}

static void make_unique_mount_path(const char *base_name, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    char safe_name[64];
    sanitize_volume_name(base_name, safe_name, sizeof(safe_name));

    for (uint32_t suffix = 0; suffix < 100; suffix++) {
        out[0] = '\0';
        kstring::strncpy(out, "/vol/", out_size - 1);
        kstring::strncat(out, safe_name, out_size - 1 - kstring::strlen(out));
        if (suffix != 0) {
            char suffix_buf[16];
            kstring::itoa(suffix, suffix_buf, 10);
            kstring::strncat(out, "-", out_size - 1 - kstring::strlen(out));
            kstring::strncat(out, suffix_buf, out_size - 1 - kstring::strlen(out));
        }
        VNodeStat st = {};
        if (vfs_stat(out, &st) != 0)
            return;
    }
    kstring::strncpy(out, "/vol/overflow", out_size - 1);
    out[out_size - 1] = '\0';
}

static bool mount_fat32_volume(BlockDevice *dev, const char *mount_path, bool system_data,
                               const char *required_label = nullptr, bool reject_boot_labels = false)
{
    auto *fs = static_cast<FAT32Filesystem *>(malloc(sizeof(FAT32Filesystem)));
    if (!fs)
        return false;
    *fs = {};
    if (!fat32_init(dev, fs)) {
        free(fs);
        return false;
    }
    if (required_label && !fat32_label_matches(fs->volume_label, required_label)) {
        free(fs);
        return false;
    }
    if (reject_boot_labels && fat32_label_is_boot_volume(fs->volume_label)) {
        free(fs);
        return false;
    }
    if (vfs_mount_ex(mount_path, fat32_get_root(fs), VFS_MOUNT_FLAG_STORAGE_GUARDED) != 0) {
        free(fs);
        return false;
    }

    const char *display = fs->volume_label[0]
                              ? fs->volume_label
                              : (dev->display_name[0] ? dev->display_name : (dev->name ? dev->name : "volume"));
    uint32_t flags = VOLUME_FLAG_MOUNTED | VOLUME_FLAG_WRITABLE | VOLUME_FLAG_STORAGE_DEVICE;
    if (system_data)
        flags |= VOLUME_FLAG_SYSTEM_DATA;
    volume_register(display, mount_path, dev->name ? dev->name : "", flags);
    return true;
}

static void settle_usb_storage(uint32_t polls, uint32_t delay_ms)
{
    for (uint32_t i = 0; i < polls; i++) {
        usb_poll();
        if (delay_ms != 0)
            sleep(delay_ms);
    }
    partition_scan_all();
}

static bool mount_persistent_data_volume()
{
    if (g_data_device)
        return true;

    for (int phase = 0; phase < 3; phase++) {
        const char *required_label = (phase == 0) ? DATA_VOLUME_LABEL : nullptr;
        const bool reject_boot_labels = (phase == 1);
        for (int pass = 0; pass < 2; pass++) {
            for (BlockDevice *dev = block_dev_first(); dev; dev = dev->next) {
                const bool eligible = (pass == 0) ? dev->is_partition : (!dev->is_partition && !dev->has_partitions);
                if (!eligible)
                    continue;

                if (mount_fat32_volume(dev, "/data", true, required_label, reject_boot_labels)) {
                    if (phase == 0) {
                        BOOT_SUCCESS("Persistent data volume mounted at /data from %s",
                                     dev->name ? dev->name : "(unnamed)");
                    } else if (phase == 1) {
                        BOOT_SUCCESS("FAT32 data volume mounted at /data from %s", dev->name ? dev->name : "(unnamed)");
                    } else {
                        BOOT_WARN("Storage: using boot FAT32 volume for /data from %s",
                                  dev->name ? dev->name : "(unnamed)");
                    }
                    g_data_device = dev;
                    return true;
                }
            }
        }
    }

    return false;
}

static void mount_removable_volumes()
{
    if (!g_vol_mount_ready)
        return;

    for (int pass = 0; pass < 2; pass++) {
        for (BlockDevice *dev = block_dev_first(); dev; dev = dev->next) {
            const bool eligible = (pass == 0) ? dev->is_partition : (!dev->is_partition && !dev->has_partitions);
            if (!eligible || dev == g_data_device)
                continue;

            char mount_path[96];
            const char *base_name = dev->display_name[0] ? dev->display_name : (dev->name ? dev->name : "volume");
            make_unique_mount_path(base_name, mount_path, sizeof(mount_path));
            if (!ensure_directory(mount_path))
                continue;
            if (mount_fat32_volume(dev, mount_path, false))
                BOOT_SUCCESS("FAT32 mounted at %s from %s", mount_path, dev->name ? dev->name : "(unnamed)");
        }
    }
}

#ifdef DEBUG
// Debug-only network self-test (runs only when a NIC was configured): proves
// the ARP blocking helper, the ICMP echo path, the UDP/DNS path and the TCP
// client path end-to-end against the emulated gateway/DNS/HTTP server
// (slirp always answers ARP and ICMP echo for the gateway; the DNS leg needs
// a host resolver with IPv4 answers, so it is informational; the HTTP leg
// fetches /hello.txt from 10.0.2.2:8931 where tools/smoke_net.py serves it).
// Results land in the serial log (warn-level: quiet mode keeps those) and
// one summary line is the smoke-net assertion surface.
#define NET_SMOKE_HTTP_IP 0x0202000Au // host-order 10.0.2.2 (slirp gateway)
#define NET_SMOKE_HTTP_PORT 8931

static void net_self_test_http(const char **status)
{
    *status = "SKIP";
    if (net_get_gateway() != NET_SMOKE_HTTP_IP)
        return; // not slirp user networking: no host server to talk to

    const int sock = tcp_socket();
    if (sock < 0) {
        *status = "FAIL";
        return;
    }

    if (!tcp_connect(sock, NET_SMOKE_HTTP_IP, NET_SMOKE_HTTP_PORT)) {
        DEBUG_WARN("net self-test: http connect to 10.0.2.2:%d failed", NET_SMOKE_HTTP_PORT);
        tcp_close(sock);
        *status = "FAIL";
        return;
    }

    static const char request[] = "GET /hello.txt HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    if (tcp_send(sock, request, (uint16_t)(sizeof(request) - 1)) < 0) {
        tcp_close(sock);
        *status = "FAIL";
        return;
    }

    static char response[4096];
    size_t total = 0;
    const uint64_t start = timer_get_ticks();
    const uint64_t deadline = start + (5 * timer_get_frequency());
    while (timer_get_ticks() < deadline) {
        const TcpState st = tcp_get_state(sock);
        const bool closed = (st == TCP_CLOSED) || (st >= TCP_FIN_WAIT_1 && st <= TCP_TIME_WAIT);
        int n = tcp_recv(sock, response + total, (uint16_t)(sizeof(response) - 1 - total));
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (closed || total >= sizeof(response) - 1)
            break;
        net_poll();
        scheduler_yield();
    }

    bool found = false;
    for (size_t i = 0; i + 15 <= total; i++) {
        if (kstring::memcmp(response + i, "uniOS-smoke-net", 15) == 0) {
            found = true;
            break;
        }
    }

    tcp_close(sock);
    DEBUG_WARN("net self-test: http fetch %u bytes: %s", (unsigned)total, found ? "PASS" : "FAIL");
    *status = found ? "PASS" : "FAIL";
}

static void net_self_test_task()
{
    if (!net_is_configured()) {
        DEBUG_INFO("net self-test: skipped (no address)");
        return;
    }

    uint8_t mac[6] = {};
    const uint32_t probe_ip = net_get_gateway() != 0 ? net_get_gateway() : net_get_dns();
    bool arp_ok = arp_resolve_blocking(probe_ip, mac, 3000);
    DEBUG_WARN("net self-test: arp %d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x: %s", probe_ip & 0xFF,
               (probe_ip >> 8) & 0xFF, (probe_ip >> 16) & 0xFF, (probe_ip >> 24) & 0xFF, mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5], arp_ok ? "PASS" : "FAIL");

    uint32_t ping_rtt = 0;
    const bool ping_ok = icmp_ping_probe(probe_ip, 2000, &ping_rtt) == 0;
    DEBUG_WARN("net self-test: ping %d.%d.%d.%d: %s (rtt %ums)", probe_ip & 0xFF, (probe_ip >> 8) & 0xFF,
               (probe_ip >> 16) & 0xFF, (probe_ip >> 24) & 0xFF, ping_ok ? "PASS" : "FAIL", ping_rtt);

    const uint32_t dns_ip = dns_resolve("unicode.org");
    DEBUG_WARN("net self-test: dns unicode.org -> %d.%d.%d.%d: %s", dns_ip & 0xFF, (dns_ip >> 8) & 0xFF,
               (dns_ip >> 16) & 0xFF, (dns_ip >> 24) & 0xFF, dns_ip != 0 ? "PASS" : "FAIL");

    const char *http_status = "SKIP";
    net_self_test_http(&http_status);

    // Single summary line: the smoke-net suite's assertion surface.
    DEBUG_WARN("net self-test summary: arp=%s ping=%s dns=%s http=%s", arp_ok ? "PASS" : "FAIL",
               ping_ok ? "PASS" : "FAIL", dns_ip != 0 ? "PASS" : "FAIL", http_status);
}
#endif

static void deferred_boot_services_task()
{
    BOOT_LOG("Boot stage: deferred services initialization");
    if (!g_data_device) {
        settle_usb_storage(3, 25);
        if (ensure_directory("/data"))
            mount_persistent_data_volume();
    }
    net_init();
#ifdef DEBUG
    if (Process *net_test = scheduler_create_task_deferred(net_self_test_task, "NetSelfTest")) {
        net_test->priority = 4;
        scheduler_enqueue_task(net_test);
    }
    // Debug-only, like the ktest suite: boot the userspace pthread self-test
    // so every debug run exercises the libc thread stack end to end. Its
    // serial summary line is the smoke suite's assertion surface.
    const int64_t threadtest_pid = kernel_exec("/bin/threadtest.elf");
    if (threadtest_pid < 0)
        BOOT_WARN("failed to launch /bin/threadtest.elf");
    else
        BOOT_LOG("/bin/threadtest.elf queued as pid %lu", static_cast<uint64_t>(threadtest_pid));
#endif
    mount_removable_volumes();
    boot_timing_log("deferred services ready");
}

static void launch_init_task()
{
    BOOT_LOG("Launching /bin/init.elf");
    const int64_t init_pid = kernel_exec("/bin/init.elf");
    if (init_pid < 0) {
        BOOT_ERROR("failed to launch /bin/init.elf");
        return;
    }

    BOOT_LOG("/bin/init.elf queued as pid %lu", static_cast<uint64_t>(init_pid));
    boot_timing_log("init queued");
}

#ifdef DEBUG
// Per-boot teardown audit (debug builds only): every kernel-mode task
// boot created (DeferredInit, InitLaunch, NetSelfTest) must exit AND be
// reaped, and the deferred-free list must hold no kernel-mode zombie —
// exactly the leak class the null guards in process_reap_classify_locked
// closed, where a reaped kernel task's null page table matched every
// other kernel task's null and its struct plus kernel stack parked
// forever. Each scheduler_yield() pumps a kernel-zombie reap pass, so
// waiting also drives the deferred retries. A survivor set that stops
// changing for kStuckMs (survivors drain by exiting; a slow-but-alive
// task keeps the same name until it exits) is a wedged teardown: print
// the survivors and FAIL rather than waiting out the smoke timeout.
static void task_teardown_audit_task()
{
    const uint64_t self_pid = process_get_current() ? process_get_current()->pid : 0;
    constexpr uint64_t kStuckMs = 60000;

    uint64_t deferred_total = 0;
    uint64_t deferred_kernel = 0;
    uint64_t remaining = 0;
    char survivors[160] = {0};
    char prev_survivors[160] = {0};

    scheduler_debug_teardown_state(self_pid, &deferred_total, &deferred_kernel, &remaining, survivors,
                                   sizeof(survivors));
    kstring::strncpy(prev_survivors, survivors, sizeof(prev_survivors) - 1);
    uint64_t stuck_deadline = timer_get_ticks() + timer_ms_to_ticks(kStuckMs);

    while (deferred_kernel != 0 || remaining != 0) {
        scheduler_yield();
        scheduler_debug_teardown_state(self_pid, &deferred_total, &deferred_kernel, &remaining, survivors,
                                       sizeof(survivors));
        if (kstring::strcmp(survivors, prev_survivors) != 0) {
            kstring::strncpy(prev_survivors, survivors, sizeof(prev_survivors) - 1);
            stuck_deadline = timer_get_ticks() + timer_ms_to_ticks(kStuckMs);
        } else if (timer_get_ticks() >= stuck_deadline) {
            DEBUG_WARN("task teardown audit: wedged (deferred_total=%llu deferred_kernel=%llu survivors: %s)",
                       (unsigned long long)deferred_total, (unsigned long long)deferred_kernel,
                       survivors[0] != '\0' ? survivors : "none");
            DEBUG_WARN("task teardown audit: FAIL");
            process_exit(0);
        }
    }

    DEBUG_WARN("task teardown audit: deferred_total=%llu deferred_kernel=0 remaining_kernel_tasks=0 pmm_free_kib=%llu",
               (unsigned long long)deferred_total, (unsigned long long)(pmm_get_free_memory() / 1024));
    DEBUG_WARN("task teardown audit: PASS");
    process_exit(0);
}
#endif

extern "C" void __stack_chk_guard_init();

extern "C" [[gnu::target("no-sse")]] void _start(BootInfo *boot_info)
{
    serial_init();
    BOOT_LOG("_start reached (Serial Init OK)");
    boot_set_info(boot_info);

    cpu_init();
    BOOT_SUCCESS("CPU features initialized");

    __stack_chk_guard_init();

#ifdef DEBUG
    BOOT_LOG("BUILD=debug");
#else
    BOOT_LOG("BUILD=release");
#endif

    if (!boot_info || boot_info->magic != BOOT_INFO_MAGIC || boot_info->revision != BOOT_INFO_REVISION ||
        !boot_info->framebuffer) {
        BOOT_ERROR("FATAL: boot info or framebuffer missing");
        hcf();
    }

    gdt_init();
    BOOT_SUCCESS("GDT/TSS initialized");
    idt_init();
    BOOT_SUCCESS("IDT initialized");

    BootFramebuffer *fb = boot_info->framebuffer;
    g_framebuffer = fb;

    gfx_init(fb);
    debug_init(fb);
    gfx_clear(COLOR_BLACK);
    boot_splash_init();
    boot_splash_set_progress(16);

    BOOT_LOG("uniOS Kernel starting (Commit %s)...", GIT_COMMIT);

    copy_boot_string(nullptr, g_bootloader_name_buf, sizeof(g_bootloader_name_buf));
    copy_boot_string(nullptr, g_bootloader_version_buf, sizeof(g_bootloader_version_buf));
    if (boot_info->bootloader_name || boot_info->bootloader_version) {
        copy_boot_string(boot_info->bootloader_name, g_bootloader_name_buf, sizeof(g_bootloader_name_buf));
        copy_boot_string(boot_info->bootloader_version, g_bootloader_version_buf, sizeof(g_bootloader_version_buf));
        g_bootloader_name = g_bootloader_name_buf;
        g_bootloader_version = g_bootloader_version_buf;
        BOOT_LOG("Bootloader: %s %s", g_bootloader_name_buf[0] ? g_bootloader_name_buf : "unknown",
                 g_bootloader_version_buf[0] ? g_bootloader_version_buf : "unknown");
    }

    uint64_t firmware_type = boot_info->firmware_type;
    uint64_t efi_system_table_addr = boot_info->efi_system_table_address;

    BOOT_LOG("Memory initialization");
    boot_splash_set_progress(18);
    vmm_early_init();
    pmm_init();
    vmm_init();

    BOOT_LOG("Heap initialization");
    heap_init(nullptr, 0);

    boot_display_timing_init(efi_system_table_addr, firmware_type);
    boot_splash_set_progress(34);

    BOOT_LOG("Global constructors");
    call_global_constructors();
    pat_init();
    vmm_protect_kernel();

    BOOT_LOG("ACPI initialization");
    acpi_init();

    BOOT_LOG("Interrupt controller initialization");
    pic_remap(32, 40);
    apic_init();
    for (int i = 0; i < 16; i++)
        pic_set_mask(i);
    boot_splash_set_progress(48);

    extern EventQueue g_event_queue;
    event_init(g_event_queue);

    // Futex buckets must exist before any userspace runs; initializing
    // lazily from the first syscall raced on SMP.
    futex_init();

    const uint64_t framebuffer_bytes = framebuffer_span_bytes(fb);
    if (framebuffer_bytes != 0)
        vmm_remap_framebuffer(reinterpret_cast<uint64_t>(fb->address), framebuffer_bytes);

    gfx_enable_double_buffering();
    boot_splash_set_progress(58);
    display_init(fb);
    scheduler_init();
    // Per-core idle context for the BSP. Never queued on the shared runqueue
    // and never made current — the boot task keeps running; schedule()'s
    // empty-queue fallback parks here only if the boot task ever blocks.
    // APs get their own private idles during smp_init().
    if (Process *idle = scheduler_create_idle_task(idle_task_entry, "Idle")) {
        cpu_get_local()->idle = idle;
    }

#ifdef DEBUG
    BOOT_LOG("Kernel tests enabled");
#endif

    BOOT_LOG("Driver initialization");
    ps2_keyboard_init();
    ps2_mouse_init();
    timer_init(1000);
    timer_tsc_calibrate();
    boot_timing_start();
    pci_init();
    display_late_init();
    rtc_init();
    input_set_screen_size((int32_t)fb->width, (int32_t)fb->height);
    BOOT_LOG("Boot stage: USB host initialization");
    usb_init();
    usb_hid_init();
    settle_usb_storage(3, 25);
    BOOT_LOG("Boot stage: USB host initialization complete");
    boot_splash_set_progress(72);

    ahci_init();
    ata_init();
    // Sound is a plain PCI driver like the storage ones and needs nothing
    // the deferred services provide; initializing it here means the debug
    // ktest suite below runs with the card present (the stream tests
    // otherwise always skip their card-dependent branch).
    sound_init();
    partition_scan_all();
    boot_splash_set_progress(84);

    BOOT_SUCCESS("Core boot complete");

    BOOT_LOG("Filesystem initialization");
    vfs_init();
    volume_reset();
    if (boot_info->module_count > 0 && boot_info->modules && boot_info->modules[0].address) {
        unifs_init(boot_info->modules[0].address, boot_info->modules[0].size);
        vfs_mount("/", unifs_get_root());
        volume_register("System", "/", "unifs", VOLUME_FLAG_MOUNTED | VOLUME_FLAG_WRITABLE);
    }

    const bool data_mount_ready = ensure_directory("/data");
    g_vol_mount_ready = ensure_directory("/vol");

    if (!data_mount_ready) {
        BOOT_WARN("Storage: failed to create /data mountpoint, persistent settings unavailable");
    }
    if (!g_vol_mount_ready) {
        BOOT_WARN("Storage: failed to create /vol mountpoint, removable volumes unavailable");
    }

    bool data_mounted = false;
    if (data_mount_ready)
        data_mounted = mount_persistent_data_volume();

    if (data_mount_ready && !data_mounted) {
        BOOT_LOG("Storage: waiting for late USB storage before giving up on /data");
        settle_usb_storage(6, 25);
        data_mounted = mount_persistent_data_volume();
    }

    if (!data_mounted)
        BOOT_WARN("Storage: no persistent /data volume mounted; settings will be session-only");

    boot_splash_set_progress(96);
    boot_timing_log("kernel critical path ready");

#ifdef DEBUG
    // ktests park real tasks on timed waits (futex timeouts, the timed-wait
    // registry); those wake only from tick-driven scheduler passes. The boot
    // critical path runs with interrupts off, and without the timer IRQ the
    // tick counter only advances inside the brief post-sti windows of spawned
    // tasks — a parked waiter with no covering sti window would hang forever.
    // Every IRQ consumer the tests can reach (APIC, PS/2, xHCI) is registered
    // by this point, so enable interrupts for the suite; smp_init() has not
    // run yet, so this stays BSP-local. Interrupts go back off afterwards:
    // smp_init's bring-up sequence expects to enter with IF=0.
    asm volatile("sti" ::: "memory");
    ktest_run_all();
    gfx_swap_buffers();
    asm volatile("cli" ::: "memory");
#else
    DEBUG_TRACE("kernel tests disabled in release build");
#endif
    boot_splash_set_progress(100);

    smp_init();

    // APs are online from here on, so tasks must be fully configured BEFORE
    // they become runnable — scheduler_create_task_deferred() + explicit
    // enqueue publishes atomically instead of mutating a queued Process.
    if (Process *deferred = scheduler_create_task_deferred(deferred_boot_services_task, "DeferredInit")) {
        deferred->priority = 0;
        scheduler_enqueue_task(deferred);
    } else {
        BOOT_ERROR("failed to create DeferredInit task");
    }
    if (Process *init_launcher = scheduler_create_task_deferred(launch_init_task, "InitLaunch")) {
        init_launcher->priority = 1;
        scheduler_enqueue_task(init_launcher);
    } else {
        BOOT_ERROR("failed to create InitLaunch task");
    }
#ifdef DEBUG
    // Debug-only, like the ktest suite: audits that every kernel task
    // boot created is fully torn down (issue #31). Created last so every
    // task it audits exists; the audit's PASS line is a smoke marker.
    if (Process *audit = scheduler_create_task_deferred(task_teardown_audit_task, "TeardownAudit")) {
        audit->priority = 2;
        scheduler_enqueue_task(audit);
    } else {
        BOOT_ERROR("failed to create TeardownAudit task");
    }
#endif

#ifdef DEBUG
    // IRQ-leak probe for the idle loop: which polled stage returns with
    // interrupts disabled. 1=input, 2=net, 3=sound, 4=yield. Printed once per
    // stage when the idle loop catches IF=0 right after it, then re-enabled
    // so the boot keeps going and the leak stays visible in the log.
    static uint32_t g_dbg_leak_reported = 0;
    auto dbg_poll_check = [](uint32_t stage) {
        if (!interrupts_enabled()) {
            if ((g_dbg_leak_reported & (1u << stage)) == 0) {
                g_dbg_leak_reported |= 1u << stage;
                DEBUG_WARN("idle: interrupts disabled by poll stage %u (leak)", stage);
            }
            asm volatile("sti" ::: "memory");
        }
    };
#define DBG_POLL(stage, call)                                                                                          \
    do {                                                                                                               \
        (call);                                                                                                        \
        dbg_poll_check(stage);                                                                                         \
    } while (0)
#else
#define DBG_POLL(stage, call) (call)
#endif

    asm volatile("sti" ::: "memory");
    scheduler_yield();

    input_poll();
    net_poll();

    uint32_t poll_counter = 0;
    while (true) {
        if (++poll_counter >= 10) {
            DBG_POLL(1, input_poll());
            DBG_POLL(2, net_poll());
            poll_counter = 0;
        }
        // Audio DMA refills and stream bookkeeping. Cheap no-op when no
        // playback is active; blocking sound writers rely on this pump.
        DBG_POLL(3, sound_poll());

        DBG_POLL(4, scheduler_yield());
        asm volatile("hlt");
    }
}

extern "C" {
void *memcpy(void *dst, const void *src, size_t n)
{
    return kstring::memcpy(dst, src, n);
}

void *memset(void *dst, int c, size_t n)
{
    return kstring::memset(dst, c, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return kstring::memmove(dst, src, n);
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    return kstring::memcmp(s1, s2, n);
}
}

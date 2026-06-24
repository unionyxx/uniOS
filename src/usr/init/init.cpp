#include <uapi/fs.h>
#include <uapi/gui.h>
#include <uapi/syscalls.h>

#include "../libc/config_utils.h"
#include "../libc/log.h"
#include "../libc/syscall.h"
#include "../libc/unistd.h"

static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";

static bool load_system_config(char *out, size_t out_size)
{
    const char *candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    return cfg_read_text_from_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]), out, out_size);
}

static bool config_flag_enabled(const char *config, const char *key, bool default_value)
{
    char value[8];
    if (!cfg_line_value(config, key, value, sizeof(value)))
        return default_value;
    return value[0] != '0';
}

static int spawn_process(const char *label, const char *path)
{
    LOG_INFO("init", "Spawning %s (%s)", label, path);
    int pid = fork();
    if (pid == 0) {
        exec(path);
        exit(1);
    }
    if (pid < 0)
        LOG_ERROR("init", "Failed to spawn %s", label);
    return pid;
}

static void terminate_child(int &pid, const char *label)
{
    if (pid <= 0)
        return;
    LOG_WARN("init", "Stopping %s (pid %d)", label, pid);
    syscall2(SYS_KILL, (uint64_t)pid, SIGTERM);
    pid = -1;
}

static Registry *wait_for_desktop_registry()
{
    syscall1(SYS_SHM_UNMAP, 0);

    Registry *registry = nullptr;
    for (int attempt = 0; attempt < 500; attempt++) {
        if (!registry) {
            uint64_t reg_ptr = syscall1(SYS_SHM_MAP, 0);
            if (reg_ptr != 0 && reg_ptr != (uint64_t)-1)
                registry = (Registry *)reg_ptr;
        }

        if (registry && registry->magic == REGISTRY_MAGIC &&
            gui_shm_id_is_valid(registry->mb_shm_id) &&
            gui_shm_id_is_valid(registry->dk_shm_id))
            return registry;

        sleep_ms(10);
    }

    return nullptr;
}

static bool start_desktop(int &wm_pid, int &menubar_pid, int &dock_pid)
{
    wm_pid = spawn_process("Window Manager", "/bin/wm.elf");
    if (wm_pid < 0)
        return false;

    Registry *registry = wait_for_desktop_registry();
    if (!registry) {
        LOG_WARN("init", "Window Manager did not publish a ready registry");
        return false;
    }

    menubar_pid = spawn_process("Menu Bar", "/bin/menubar.elf");
    sleep_ms(20);

    dock_pid = spawn_process("Dock", "/bin/dock.elf");
    sleep_ms(20);
    return true;
}

static void start_desktop_until_ready(int &wm_pid, int &menubar_pid, int &dock_pid)
{
    while (!start_desktop(wm_pid, menubar_pid, dock_pid)) {
        terminate_child(menubar_pid, "Menu Bar");
        terminate_child(dock_pid, "Dock");
        terminate_child(wm_pid, "Window Manager");
        sleep_ms(250);
    }
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    LOG_INFO("init", "Starting user space");

    int wm_pid = -1;
    int menubar_pid = -1;
    int dock_pid = -1;
    start_desktop_until_ready(wm_pid, menubar_pid, dock_pid);

#ifdef DEBUG
    LOG_INFO("init", "desktop shell ready at %llu ms", (unsigned long long)get_ticks());
#endif

    char config_buf[512];
    bool launch_terminal = false;
    if (load_system_config(config_buf, sizeof(config_buf))) {
        launch_terminal = config_flag_enabled(config_buf, "launch_terminal_on_boot", false);
    }

    if (launch_terminal) {
        spawn_process("Terminal", "/bin/terminal.elf");
    }

    int status = 0;
    while (true) {
        int pid = waitpid(-1, &status);
        if (pid < 0) {
            sleep_ms(250);
            continue;
        }

        if (pid == wm_pid) {
            LOG_WARN("init", "Window Manager exited with status %d; restarting desktop", status);
            wm_pid = -1;
            terminate_child(menubar_pid, "Menu Bar");
            terminate_child(dock_pid, "Dock");
            start_desktop_until_ready(wm_pid, menubar_pid, dock_pid);
        } else if (pid == menubar_pid) {
            LOG_WARN("init", "Menu Bar exited with status %d; restarting", status);
            menubar_pid = spawn_process("Menu Bar", "/bin/menubar.elf");
        } else if (pid == dock_pid) {
            LOG_WARN("init", "Dock exited with status %d; restarting", status);
            dock_pid = spawn_process("Dock", "/bin/dock.elf");
        } else {
            LOG_INFO("init", "Child pid %d exited with status %d", pid, status);
        }

        sleep_ms(50);
    }

    return 0;
}

#include <boot/boot_info.h>
#include <kernel/boot_display_timing.h>
#include <stddef.h>
#include <stdint.h>

#include "efi.h"

extern "C" [[noreturn]] EFIAPI void boot_jump_to_kernel(uint64_t entry, BootInfo *boot_info, uint64_t stack_top,
                                                        uint64_t cr3);

namespace {

constexpr uint64_t k_page_size = 4096;
constexpr uint64_t k_page_mask = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t k_large_page_mask = 0x000FFFFFFFE00000ULL;
constexpr uint64_t k_hhdm_base = 0xFFFF800000000000ULL;
constexpr uint64_t k_direct_map_limit = 0x0000800000000000ULL;
constexpr uint64_t k_large_page_size = 0x200000ULL;
constexpr uint64_t k_large_page_mask_inv = k_large_page_size - 1;
constexpr uint64_t k_gib = 0x40000000ULL;
constexpr UINTN k_boot_payload_pages = 16;
constexpr UINTN k_memory_map_buffer_pages = 16;
constexpr UINTN k_kernel_stack_pages = 16;
constexpr UINTN k_page_table_margin_pages = 32;
constexpr UINTN k_file_read_chunk_size = 1024 * 1024;
constexpr size_t k_max_boot_memory_map_entries = 1024;
constexpr size_t k_max_pml4_entries = 512;
constexpr size_t k_max_pd_entries = 32768;
constexpr size_t k_max_pt_entries = 8192;
constexpr uint64_t k_pte_present = 1ULL << 0;
constexpr uint64_t k_pte_writable = 1ULL << 1;
constexpr uint64_t k_pte_large = 1ULL << 7;
constexpr UINTN k_edid_block_size = 128;

static constexpr EFI_GUID EFI_BOOT_DISPLAY_TIMING_TABLE_GUID = {
    0x7f7d5f2a, 0x5ebd, 0x4a51, {0xa8, 0x6f, 0x72, 0x7a, 0x5a, 0x19, 0x31, 0x0c}};

[[nodiscard]] static uint64_t align_down(uint64_t value, uint64_t align);
[[nodiscard]] static uint64_t align_up(uint64_t value, uint64_t align);
static void zero_memory(void *dst, size_t size);

static const CHAR16 k_kernel_path[] = {'\\', 'K', 'E', 'R', 'N', 'E', 'L', '.', 'E', 'L', 'F', 0};
static const CHAR16 k_unifs_path[] = {'\\', 'U', 'N', 'I', 'F', 'S', '.', 'I', 'M', 'G', 0};

struct Elf64Ehdr
{
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64Phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

constexpr uint32_t k_elf_pt_load = 1;

struct BootPayload
{
    BootInfo info;
    BootFramebuffer framebuffer;
    BootVideoMode mode;
    BootVideoMode *mode_ptrs[1];
    BootModule modules[1];
    char bootloader_name[32];
    char bootloader_version[16];
    char module_path[32];
    BootMemoryMapEntry memory_map[k_max_boot_memory_map_entries];
};

static_assert(sizeof(BootPayload) <= k_boot_payload_pages * k_page_size, "Boot payload buffer is too small");

struct BootEdidModeHint
{
    bool valid;
    bool has_exact_timing;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
    uint32_t pixel_clock_khz;
    uint32_t h_total;
    uint32_t v_total;
    bool interlaced;
};

template <size_t Max>
struct UniqueSet
{
    uint64_t values[Max];
    size_t count;

    void reset()
    {
        count = 0;
    }

    bool add(uint64_t value)
    {
        for (size_t i = 0; i < count; i++) {
            if (values[i] == value)
                return true;
        }
        if (count >= Max)
            return false;
        values[count++] = value;
        return true;
    }
};

struct TableEstimator
{
    UniqueSet<k_max_pml4_entries> pml4_entries = {};
    UniqueSet<k_max_pd_entries> pd_entries = {};
    UniqueSet<k_max_pt_entries> pt_entries = {};

    void reset()
    {
        pml4_entries.reset();
        pd_entries.reset();
        pt_entries.reset();
    }

    bool add_2m_range(uint64_t virt_base, uint64_t size)
    {
        if (size == 0)
            return true;
        if (virt_base > UINT64_MAX - size)
            return false;
        const uint64_t start = align_down(virt_base, k_large_page_size);
        const uint64_t end = align_up(virt_base + size, k_large_page_size);
        if (end == UINT64_MAX)
            return false;
        // Align the tracking cursor down to k_gib to guarantee full tracking across boundaries
        for (uint64_t cursor = align_down(start, k_gib); cursor < end; cursor += k_gib) {
            if (!pml4_entries.add(cursor >> 39) || !pd_entries.add(cursor >> 30))
                return false;
        }
        return true;
    }

    bool add_4k_range(uint64_t virt_base, uint64_t size)
    {
        if (size == 0)
            return true;
        if (virt_base > UINT64_MAX - size)
            return false;
        const uint64_t start = align_down(virt_base, k_page_size);
        const uint64_t end = align_up(virt_base + size, k_page_size);
        if (end == UINT64_MAX)
            return false;
        // Align the tracking cursor down to k_gib to guarantee full tracking across boundaries
        for (uint64_t cursor = align_down(start, k_gib); cursor < end; cursor += k_gib) {
            if (!pml4_entries.add(cursor >> 39) || !pd_entries.add(cursor >> 30))
                return false;
        }
        for (uint64_t cursor = align_down(start, k_large_page_size); cursor < end; cursor += k_large_page_size) {
            if (!pt_entries.add(cursor >> 21))
                return false;
        }
        return true;
    }

    UINTN total_pages() const
    {
        return static_cast<UINTN>(1 + pml4_entries.count + pd_entries.count + pt_entries.count);
    }
};

struct TableAllocator
{
    uint64_t base_phys;
    UINTN page_count;
    UINTN used_pages;

    void reset()
    {
        used_pages = 0;
    }

    uint64_t allocate_page()
    {
        if (used_pages >= page_count)
            return 0;
        const uint64_t phys = base_phys + static_cast<uint64_t>(used_pages) * k_page_size;
        used_pages++;
        zero_memory(reinterpret_cast<void *>(phys), k_page_size);
        return phys;
    }
};

struct PageTables
{
    TableAllocator allocator = {};
    uint64_t pml4_phys = 0;

    bool init(uint64_t pool_phys, UINTN pool_pages)
    {
        allocator.base_phys = pool_phys;
        allocator.page_count = pool_pages;
        allocator.used_pages = 0;
        pml4_phys = allocator.allocate_page();
        return pml4_phys != 0;
    }

    static uint64_t *table_ptr(uint64_t phys)
    {
        return reinterpret_cast<uint64_t *>(phys);
    }

    uint64_t ensure_next(uint64_t table_phys, size_t index)
    {
        uint64_t *table = table_ptr(table_phys);
        const uint64_t entry = table[index];
        if ((entry & k_pte_present) != 0)
            return entry & k_page_mask;

        const uint64_t next = allocator.allocate_page();
        if (next == 0)
            return 0;
        table[index] = next | k_pte_present | k_pte_writable;
        return next;
    }

    bool map_2m(uint64_t virt, uint64_t phys, uint64_t flags)
    {
        if ((virt & k_large_page_mask_inv) != 0 || (phys & k_large_page_mask_inv) != 0)
            return false;

        const size_t pml4_index = static_cast<size_t>((virt >> 39) & 0x1FF);
        const size_t pdpt_index = static_cast<size_t>((virt >> 30) & 0x1FF);
        const size_t pd_index = static_cast<size_t>((virt >> 21) & 0x1FF);

        const uint64_t pdpt_phys = ensure_next(pml4_phys, pml4_index);
        if (pdpt_phys == 0)
            return false;
        const uint64_t pd_phys = ensure_next(pdpt_phys, pdpt_index);
        if (pd_phys == 0)
            return false;

        uint64_t *pd = table_ptr(pd_phys);
        pd[pd_index] = (phys & k_large_page_mask) | flags | k_pte_present | k_pte_large;
        return true;
    }

    bool map_4k(uint64_t virt, uint64_t phys, uint64_t flags)
    {
        const size_t pml4_index = static_cast<size_t>((virt >> 39) & 0x1FF);
        const size_t pdpt_index = static_cast<size_t>((virt >> 30) & 0x1FF);
        const size_t pd_index = static_cast<size_t>((virt >> 21) & 0x1FF);
        const size_t pt_index = static_cast<size_t>((virt >> 12) & 0x1FF);

        const uint64_t pdpt_phys = ensure_next(pml4_phys, pml4_index);
        if (pdpt_phys == 0)
            return false;
        const uint64_t pd_phys = ensure_next(pdpt_phys, pdpt_index);
        if (pd_phys == 0)
            return false;
        const uint64_t pt_phys = ensure_next(pd_phys, pd_index);
        if (pt_phys == 0)
            return false;

        uint64_t *pt = table_ptr(pt_phys);
        pt[pt_index] = (phys & k_page_mask) | flags | k_pte_present;
        return true;
    }
};

EFI_SYSTEM_TABLE *g_system_table = nullptr;
EFI_BOOT_SERVICES *g_boot_services = nullptr;
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con_out = nullptr;
TableEstimator g_table_estimator = {};

[[nodiscard]] static uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

[[nodiscard]] static uint64_t align_up(uint64_t value, uint64_t align)
{
    if (value > UINT64_MAX - (align - 1))
        return UINT64_MAX;
    return (value + align - 1) & ~(align - 1);
}

[[nodiscard]] static uint64_t page_count_for_bytes(uint64_t bytes)
{
    if (bytes > UINT64_MAX - (k_page_size - 1))
        return 0;
    return (bytes + k_page_size - 1) / k_page_size;
}

[[nodiscard]] static uint64_t range_end(uint64_t base, uint64_t length)
{
    if (length == 0)
        return base;
    if (base > UINT64_MAX - length)
        return UINT64_MAX;
    return base + length;
}

// Explicit type definition for unaligned 64-bit word operations with strict-aliasing overrides
typedef uint64_t __attribute__((aligned(1), may_alias)) unaligned_u64;

static void *copy_memory(void *dst, const void *src, size_t size)
{
    auto *d64 = reinterpret_cast<unaligned_u64 *>(dst);
    const auto *s64 = reinterpret_cast<const unaligned_u64 *>(src);
    
    // Perform bulk 8-byte transfers safely without alignment requirements
    size_t words = size / 8;
    for (size_t i = 0; i < words; ++i) {
        d64[i] = s64[i];
    }
    
    // Handle remaining bytes sequentially
    auto *d8 = reinterpret_cast<uint8_t *>(d64 + words);
    const auto *s8 = reinterpret_cast<const uint8_t *>(s64 + words);
    size_t rem = size % 8;
    for (size_t i = 0; i < rem; ++i) {
        d8[i] = s8[i];
    }
    return dst;
}

static void *set_memory(void *dst, int value, size_t size)
{
    uint64_t byte_val = static_cast<uint8_t>(value);
    uint64_t word_val = (byte_val << 56) | (byte_val << 48) | (byte_val << 40) | (byte_val << 32) |
                        (byte_val << 24) | (byte_val << 16) | (byte_val << 8)  | byte_val;
                        
    auto *d64 = reinterpret_cast<unaligned_u64 *>(dst);
    size_t words = size / 8;
    for (size_t i = 0; i < words; ++i) {
        d64[i] = word_val;
    }
    
    auto *d8 = reinterpret_cast<uint8_t *>(d64 + words);
    size_t rem = size % 8;
    for (size_t i = 0; i < rem; ++i) {
        d8[i] = static_cast<uint8_t>(value);
    }
    return dst;
}

static int compare_memory(const void *lhs, const void *rhs, size_t size)
{
    const auto *a = static_cast<const uint8_t *>(lhs);
    const auto *b = static_cast<const uint8_t *>(rhs);
    for (size_t i = 0; i < size; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static void zero_memory(void *dst, size_t size)
{
    set_memory(dst, 0, size);
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < dst_size && src[i] != '\0'; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

[[nodiscard]] static bool guid_equal(const EFI_GUID &lhs, const EFI_GUID &rhs)
{
    return lhs.Data1 == rhs.Data1 && lhs.Data2 == rhs.Data2 && lhs.Data3 == rhs.Data3 &&
           compare_memory(lhs.Data4, rhs.Data4, sizeof(lhs.Data4)) == 0;
}

static void console_write(const char *text)
{
    if (!g_con_out || !g_con_out->OutputString || !text)
        return;

    CHAR16 buffer[128];
    while (*text != '\0') {
        size_t pos = 0;
        while (*text != '\0' && pos + 1 < (sizeof(buffer) / sizeof(buffer[0]))) {
            const char ch = *text++;
            if (ch == '\n') {
                if (pos + 2 >= (sizeof(buffer) / sizeof(buffer[0])))
                    break;
                buffer[pos++] = '\r';
                buffer[pos++] = '\n';
            } else {
                buffer[pos++] = static_cast<CHAR16>(static_cast<uint8_t>(ch));
            }
        }
        buffer[pos] = 0;
        g_con_out->OutputString(g_con_out, buffer);
    }
}

static void console_write_line(const char *text)
{
    console_write(text);
    console_write("\n");
}

static void console_write_hex(uint64_t value)
{
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; i++) {
        const uint8_t nibble = static_cast<uint8_t>((value >> ((15 - i) * 4)) & 0x0F);
        buffer[2 + i] = nibble < 10 ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10));
    }
    buffer[18] = '\0';
    console_write(buffer);
}

static EFI_STATUS fail_status(const char *message, EFI_STATUS status)
{
    console_write("[UEFI] ");
    console_write(message);
    console_write(" (status=");
    console_write_hex(status);
    console_write(")\n");
    return status;
}

[[nodiscard]] static EFI_STATUS open_root_volume(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL **out_root)
{
    if (!g_boot_services || !out_root)
        return EFI_INVALID_PARAMETER;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = nullptr;
    EFI_STATUS status =
        g_boot_services->HandleProtocol(image_handle, const_cast<EFI_GUID *>(&EFI_LOADED_IMAGE_PROTOCOL_GUID),
                                        reinterpret_cast<void **>(&loaded_image));
    if (efi_error(status))
        return fail_status("failed to get loaded image protocol", status);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = nullptr;
    status = g_boot_services->HandleProtocol(loaded_image->DeviceHandle,
                                             const_cast<EFI_GUID *>(&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID),
                                             reinterpret_cast<void **>(&fs));
    if (efi_error(status))
        return fail_status("failed to get simple file system protocol", status);

    status = fs->OpenVolume(fs, out_root);
    if (efi_error(status))
        return fail_status("failed to open EFI volume", status);
    return EFI_SUCCESS;
}

[[nodiscard]] static EFI_STATUS open_file_for_read(EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                                                   EFI_FILE_PROTOCOL **out_file, UINTN *out_size)
{
    if (!root || !path || !out_file || !out_size)
        return EFI_INVALID_PARAMETER;

    EFI_FILE_PROTOCOL *file = nullptr;
    EFI_STATUS status = root->Open(root, &file, const_cast<CHAR16 *>(path), EFI_FILE_MODE_READ, 0);
    if (efi_error(status))
        return fail_status("failed to open boot file", status);

    // Enforce 8-byte structure alignment constraints directly on the stack buffer allocation
    alignas(EFI_FILE_INFO) uint8_t stack_info_buffer[sizeof(EFI_FILE_INFO) + 256];
    void *info_buffer = stack_info_buffer;
    UINTN info_size = sizeof(stack_info_buffer);
    status = file->GetInfo(file, const_cast<EFI_GUID *>(&EFI_FILE_INFO_GUID), &info_size, info_buffer);
    if (status == EFI_BUFFER_TOO_SMALL) {
        info_buffer = nullptr;
        status = g_boot_services->AllocatePool(EfiLoaderData, info_size, &info_buffer);
        if (efi_error(status)) {
            file->Close(file);
            return fail_status("failed to allocate boot file info buffer", status);
        }
        status = file->GetInfo(file, const_cast<EFI_GUID *>(&EFI_FILE_INFO_GUID), &info_size, info_buffer);
    }
    if (efi_error(status)) {
        if (info_buffer != stack_info_buffer)
            g_boot_services->FreePool(info_buffer);
        file->Close(file);
        return fail_status("failed to query boot file information", status);
    }

    const auto *info = reinterpret_cast<const EFI_FILE_INFO *>(info_buffer);
    if (info->FileSize == 0) {
        if (info_buffer != stack_info_buffer)
            g_boot_services->FreePool(info_buffer);
        file->Close(file);
        return fail_status("boot file is empty", EFI_LOAD_ERROR);
    }

    *out_file = file;
    *out_size = static_cast<UINTN>(info->FileSize);
    if (info_buffer != stack_info_buffer)
        g_boot_services->FreePool(info_buffer);
    return EFI_SUCCESS;
}

[[nodiscard]] static EFI_STATUS read_file_exact(EFI_FILE_PROTOCOL *file, void *buffer, UINTN file_size)
{
    if (!file || !buffer || file_size == 0)
        return EFI_INVALID_PARAMETER;

    auto *cursor = static_cast<uint8_t *>(buffer);
    UINTN remaining = file_size;
    while (remaining > 0) {
        UINTN chunk = remaining;
        if (chunk > k_file_read_chunk_size)
            chunk = k_file_read_chunk_size;
        UINTN read_size = chunk;
        EFI_STATUS status = file->Read(file, &read_size, cursor);
        if (efi_error(status))
            return fail_status("failed to read boot file", status);
        if (read_size == 0)
            return fail_status("unexpected end of boot file", EFI_DEVICE_ERROR);
        cursor += read_size;
        remaining -= read_size;
    }
    return EFI_SUCCESS;
}

[[nodiscard]] static EFI_STATUS read_entire_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path, void **out_buffer,
                                                 UINTN *out_size)
{
    if (!root || !path || !out_buffer || !out_size)
        return EFI_INVALID_PARAMETER;

    EFI_FILE_PROTOCOL *file = nullptr;
    UINTN file_size = 0;
    EFI_STATUS status = open_file_for_read(root, path, &file, &file_size);
    if (efi_error(status))
        return status;

    void *buffer = nullptr;
    status = g_boot_services->AllocatePool(EfiLoaderData, file_size, &buffer);
    if (efi_error(status)) {
        file->Close(file);
        return fail_status("failed to allocate file buffer", status);
    }

    status = read_file_exact(file, buffer, file_size);
    const EFI_STATUS close_status = file->Close(file);
    if (efi_error(status)) {
        g_boot_services->FreePool(buffer);
        return status;
    }
    if (efi_error(close_status)) {
        g_boot_services->FreePool(buffer);
        return fail_status("failed to close boot file handle", close_status);
    }

    *out_buffer = buffer;
    *out_size = file_size;
    return EFI_SUCCESS;
}

[[nodiscard]] static EFI_STATUS read_file_to_pages(EFI_FILE_PROTOCOL *root, const CHAR16 *path, uint64_t *out_phys,
                                                   uint64_t *out_size)
{
    if (!root || !path || !out_phys || !out_size)
        return EFI_INVALID_PARAMETER;

    EFI_FILE_PROTOCOL *file = nullptr;
    UINTN file_size = 0;
    EFI_STATUS status = open_file_for_read(root, path, &file, &file_size);
    if (efi_error(status))
        return status;

    EFI_PHYSICAL_ADDRESS phys = 0;
    const uint64_t file_pages = page_count_for_bytes(file_size);
    if (file_pages == 0)
        return fail_status("file is too large to allocate", EFI_OUT_OF_RESOURCES);

    status = g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, file_pages, &phys);
    if (efi_error(status)) {
        file->Close(file);
        return fail_status("failed to allocate module pages", status);
    }

    status = read_file_exact(file, reinterpret_cast<void *>(phys), file_size);
    const EFI_STATUS close_status = file->Close(file);
    if (efi_error(status)) {
        g_boot_services->FreePages(phys, file_pages);
        return status;
    }
    if (efi_error(close_status)) {
        g_boot_services->FreePages(phys, file_pages);
        return fail_status("failed to close boot file handle", close_status);
    }

    const uint64_t alloc_bytes = file_pages * k_page_size;
    if (alloc_bytes > file_size) {
        zero_memory(reinterpret_cast<uint8_t *>(phys) + file_size, (size_t)(alloc_bytes - file_size));
    }

    *out_phys = phys;
    *out_size = file_size;
    return EFI_SUCCESS;
}

[[nodiscard]] static EFI_STATUS load_kernel_image(const void *file_buffer, UINTN file_size, uint64_t *out_entry,
                                                  uint64_t *out_virt_base, uint64_t *out_phys_base,
                                                  uint64_t *out_image_size)
{
    if (!file_buffer || file_size < sizeof(Elf64Ehdr) || !out_entry || !out_virt_base || !out_phys_base ||
        !out_image_size) {
        return EFI_INVALID_PARAMETER;
    }

    const auto *ehdr = static_cast<const Elf64Ehdr *>(file_buffer);
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1 || ehdr->e_machine != 0x3E ||
        ehdr->e_phentsize != sizeof(Elf64Phdr)) {
        return fail_status("kernel ELF validation failed", EFI_LOAD_ERROR);
    }

    uint64_t ph_bytes = 0;
    uint64_t ph_end = 0;
    if (__builtin_mul_overflow(static_cast<uint64_t>(ehdr->e_phnum), static_cast<uint64_t>(sizeof(Elf64Phdr)),
                               &ph_bytes) ||
        __builtin_add_overflow(ehdr->e_phoff, ph_bytes, &ph_end) || ehdr->e_phoff >= file_size || ph_end > file_size) {
        return fail_status("kernel program header table is invalid", EFI_LOAD_ERROR);
    }

    const auto *phdrs = reinterpret_cast<const Elf64Phdr *>(static_cast<const uint8_t *>(file_buffer) + ehdr->e_phoff);

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    bool have_load_segment = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64Phdr &ph = phdrs[i];
        if (ph.p_type != k_elf_pt_load)
            continue;
        if (ph.p_memsz == 0)
            continue;
        if (ph.p_filesz > ph.p_memsz)
            return fail_status("kernel segment file size exceeds memory size", EFI_LOAD_ERROR);
        uint64_t file_end = 0;
        if (__builtin_add_overflow(ph.p_offset, ph.p_filesz, &file_end) || file_end > file_size)
            return fail_status("kernel segment extends beyond file", EFI_LOAD_ERROR);
        if (ph.p_vaddr > UINT64_MAX - ph.p_memsz)
            return fail_status("kernel segment virtual address overflows", EFI_LOAD_ERROR);
        if (ph.p_vaddr < min_vaddr)
            min_vaddr = ph.p_vaddr;
        const uint64_t segment_end = range_end(ph.p_vaddr, ph.p_memsz);
        if (segment_end > max_vaddr)
            max_vaddr = segment_end;
        have_load_segment = true;
    }

    if (!have_load_segment)
        return fail_status("kernel ELF contains no loadable segments", EFI_LOAD_ERROR);

    min_vaddr = align_down(min_vaddr, k_page_size);
    max_vaddr = align_up(max_vaddr, k_page_size);
    if (max_vaddr == UINT64_MAX || max_vaddr < min_vaddr)
        return fail_status("kernel image virtual range overflows", EFI_LOAD_ERROR);
    const uint64_t image_size = max_vaddr - min_vaddr;

    const uint64_t image_pages = page_count_for_bytes(image_size);
    if (image_pages == 0)
        return fail_status("kernel image is too large to allocate", EFI_OUT_OF_RESOURCES);

    EFI_PHYSICAL_ADDRESS kernel_phys = 0;
    EFI_STATUS status =
        g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, image_pages, &kernel_phys);
    if (efi_error(status))
        return fail_status("failed to allocate kernel image pages", status);

    zero_memory(reinterpret_cast<void *>(kernel_phys), image_size);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64Phdr &ph = phdrs[i];
        if (ph.p_type != k_elf_pt_load || ph.p_memsz == 0)
            continue;

        uint8_t *segment_target = reinterpret_cast<uint8_t *>(kernel_phys + (ph.p_vaddr - min_vaddr));
        const uint8_t *segment_source = static_cast<const uint8_t *>(file_buffer) + ph.p_offset;
        copy_memory(segment_target, segment_source, static_cast<size_t>(ph.p_filesz));
        if (ph.p_memsz > ph.p_filesz) {
            zero_memory(segment_target + ph.p_filesz, static_cast<size_t>(ph.p_memsz - ph.p_filesz));
        }
    }

    *out_entry = ehdr->e_entry;
    *out_virt_base = min_vaddr;
    *out_phys_base = kernel_phys;
    *out_image_size = image_size;
    return EFI_SUCCESS;
}

[[nodiscard]] static bool derive_mask(uint32_t mask, uint8_t *out_size, uint8_t *out_shift)
{
    if (!out_size || !out_shift)
        return false;
    if (mask == 0) {
        *out_size = 0;
        *out_shift = 0;
        return true;
    }

    uint8_t shift = 0;
    while (((mask >> shift) & 1U) == 0U)
        shift++;

    uint8_t size = 0;
    while ((shift + size) < 32 && ((mask >> (shift + size)) & 1U) != 0U)
        size++;

    *out_shift = shift;
    *out_size = size;
    return true;
}

static uint16_t read_le16(const uint8_t *data)
{
    if (!data)
        return 0;
    return static_cast<uint16_t>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8));
}

static bool edid_block_checksum_valid(const uint8_t *block)
{
    if (!block)
        return false;
    uint32_t sum = 0;
    for (UINTN i = 0; i < k_edid_block_size; i++)
        sum += block[i];
    return (sum & 0xFFu) == 0;
}

static bool edid_buffer_valid(const uint8_t *edid, UINTN edid_size, uint32_t *out_blocks)
{
    if (!edid || edid_size < k_edid_block_size)
        return false;

    static const uint8_t k_edid_header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (compare_memory(edid, k_edid_header, sizeof(k_edid_header)) != 0)
        return false;

    const uint32_t available_blocks = static_cast<uint32_t>(edid_size / k_edid_block_size);
    const uint32_t required_blocks = static_cast<uint32_t>(edid[126]) + 1u;
    if (required_blocks > available_blocks)
        return false;
    for (uint32_t block = 0; block < required_blocks; block++) {
        if (!edid_block_checksum_valid(edid + static_cast<UINTN>(block) * k_edid_block_size))
            return false;
    }

    if (out_blocks)
        *out_blocks = required_blocks;
    return true;
}

static bool edid_hint_better(const BootEdidModeHint &candidate, const BootEdidModeHint &best, bool have_best)
{
    if (!candidate.valid)
        return false;
    if (!have_best)
        return true;

    const uint64_t candidate_pixels = static_cast<uint64_t>(candidate.width) * candidate.height;
    const uint64_t best_pixels = static_cast<uint64_t>(best.width) * best.height;
    if (candidate_pixels != best_pixels)
        return candidate_pixels > best_pixels;

    if (candidate.refresh_millihz != best.refresh_millihz)
        return candidate.refresh_millihz > best.refresh_millihz;

    if (candidate.interlaced != best.interlaced)
        return !candidate.interlaced;

    if (candidate.has_exact_timing != best.has_exact_timing)
        return candidate.has_exact_timing;

    return false;
}

static void consider_edid_hint(BootEdidModeHint *best, bool *have_best, const BootEdidModeHint &candidate)
{
    if (!best || !have_best)
        return;
    if (edid_hint_better(candidate, *best, *have_best)) {
        *best = candidate;
        *have_best = true;
    }
}

static bool parse_edid_detailed_timing(const uint8_t *dtd, BootEdidModeHint *out_hint)
{
    if (!dtd || !out_hint)
        return false;

    uint32_t pixel_clock_10khz = static_cast<uint32_t>(dtd[0]) | (static_cast<uint32_t>(dtd[1]) << 8);
    if (pixel_clock_10khz == 0)
        return false;

    uint32_t h_active = static_cast<uint32_t>(dtd[2]) | ((static_cast<uint32_t>(dtd[4]) & 0xF0u) << 4);
    uint32_t h_blank = static_cast<uint32_t>(dtd[3]) | ((static_cast<uint32_t>(dtd[4]) & 0x0Fu) << 8);
    uint32_t v_active = static_cast<uint32_t>(dtd[5]) | ((static_cast<uint32_t>(dtd[7]) & 0xF0u) << 4);
    uint32_t v_blank = static_cast<uint32_t>(dtd[6]) | ((static_cast<uint32_t>(dtd[7]) & 0x0Fu) << 8);
    uint32_t h_total = h_active + h_blank;
    uint32_t v_total = v_active + v_blank;
    if (h_active == 0 || v_active == 0 || h_total <= h_active || v_total <= v_active)
        return false;

    const bool interlaced = (dtd[17] & 0x80u) != 0;
    uint64_t pixel_clock_hz = static_cast<uint64_t>(pixel_clock_10khz) * 10000ULL;
    uint64_t total_pixels = static_cast<uint64_t>(h_total) * static_cast<uint64_t>(v_total);
    uint64_t numerator = pixel_clock_hz * 1000ULL;
    if (interlaced)
        numerator *= 2ULL;
    uint32_t refresh_millihz = static_cast<uint32_t>((numerator + (total_pixels / 2ULL)) / total_pixels);
    if (refresh_millihz == 0)
        return false;

    *out_hint = {};
    out_hint->valid = true;
    out_hint->has_exact_timing = true;
    out_hint->width = h_active;
    out_hint->height = v_active;
    out_hint->refresh_millihz = refresh_millihz;
    out_hint->pixel_clock_khz = pixel_clock_10khz * 10u;
    out_hint->h_total = h_total;
    out_hint->v_total = v_total;
    out_hint->interlaced = interlaced;
    return true;
}

static bool parse_displayid_detailed_timing(const uint8_t *timing, bool type_7, BootEdidModeHint *out_hint)
{
    if (!timing || !out_hint)
        return false;

    uint32_t pixel_clock_raw = static_cast<uint32_t>(timing[0]) | (static_cast<uint32_t>(timing[1]) << 8) |
                               (static_cast<uint32_t>(timing[2]) << 16);
    if (pixel_clock_raw == 0)
        return false;

    uint32_t width = static_cast<uint32_t>(read_le16(&timing[4])) + 1u;
    uint32_t h_blank = static_cast<uint32_t>(read_le16(&timing[6])) + 1u;
    uint32_t height = static_cast<uint32_t>(read_le16(&timing[12])) + 1u;
    uint32_t v_blank = static_cast<uint32_t>(read_le16(&timing[14])) + 1u;
    uint32_t h_total = width + h_blank;
    uint32_t v_total = height + v_blank;
    if (h_total <= width || v_total <= height)
        return false;

    uint64_t pixel_clock_hz = static_cast<uint64_t>(pixel_clock_raw) * (type_7 ? 1000ULL : 10000ULL);
    uint64_t total_pixels = static_cast<uint64_t>(h_total) * static_cast<uint64_t>(v_total);
    uint32_t refresh_millihz = static_cast<uint32_t>(((pixel_clock_hz * 1000ULL) + (total_pixels / 2ULL)) /
                                                    total_pixels);
    if (refresh_millihz == 0)
        return false;

    *out_hint = {};
    out_hint->valid = true;
    out_hint->has_exact_timing = true;
    out_hint->width = width;
    out_hint->height = height;
    out_hint->refresh_millihz = refresh_millihz;
    out_hint->pixel_clock_khz = static_cast<uint32_t>(pixel_clock_hz / 1000ULL);
    out_hint->h_total = h_total;
    out_hint->v_total = v_total;
    out_hint->interlaced = false;
    return true;
}

static bool parse_displayid_formula_timing(const uint8_t *timing, BootEdidModeHint *out_hint)
{
    if (!timing || !out_hint)
        return false;

    const uint8_t timing_formula = timing[0] & 0x07u;
    if (timing_formula > 1u)
        return false;

    uint32_t width = static_cast<uint32_t>(read_le16(&timing[1])) + 1u;
    uint32_t height = static_cast<uint32_t>(read_le16(&timing[3])) + 1u;
    uint32_t refresh_hz = static_cast<uint32_t>(timing[5]) + 1u;
    if (false) // Always false due to +1u above, but keeping structure if needed for future logic changes
        return false;

    *out_hint = {};
    out_hint->valid = true;
    out_hint->width = width;
    out_hint->height = height;
    out_hint->refresh_millihz = refresh_hz * 1000u;
    out_hint->interlaced = false;
    return true;
}

static void parse_displayid_extension_for_hint(const uint8_t *ext, BootEdidModeHint *best, bool *have_best)
{
    if (!ext || !best || !have_best)
        return;

    uint32_t payload_len = ext[2];
    uint32_t offset = 5;
    uint32_t end = offset + payload_len;
    if (end > 126u)
        end = 126u;

    while (offset + 3u <= end) {
        uint8_t tag = ext[offset];
        uint32_t block_len = ext[offset + 2u];
        uint32_t payload = offset + 3u;
        uint32_t next = payload + block_len;
        if (next > end)
            break;

        if ((tag == 0x03u || tag == 0x22u) && (block_len % 20u) == 0) {
            const bool type_7 = tag == 0x22u;
            for (uint32_t i = 0; i < block_len; i += 20u) {
                BootEdidModeHint hint = {};
                if (parse_displayid_detailed_timing(&ext[payload + i], type_7, &hint))
                    consider_edid_hint(best, have_best, hint);
            }
        } else if ((tag == 0x24u || tag == 0x25u) && (block_len % 6u) == 0) {
            for (uint32_t i = 0; i < block_len; i += 6u) {
                BootEdidModeHint hint = {};
                if (parse_displayid_formula_timing(&ext[payload + i], &hint))
                    consider_edid_hint(best, have_best, hint);
            }
        }

        offset = next;
    }
}

static bool parse_cta_vic_hint(uint8_t vic, BootEdidModeHint *out_hint)
{
    if (!out_hint)
        return false;

    struct CtaVicTiming
    {
        uint16_t width;
        uint16_t height;
        uint16_t refresh_hz;
        uint8_t vic;
        bool interlaced;
    };

    static const CtaVicTiming k_cta_vics[] = {
        {640, 480, 60, 1, false},      {1280, 720, 60, 4, false},     {1920, 1080, 60, 5, true},
        {1920, 1080, 60, 16, false},   {1920, 1080, 50, 31, false},   {1920, 1080, 120, 63, false},
        {1920, 1080, 100, 64, false},  {3840, 2160, 30, 95, false},   {3840, 2160, 25, 96, false},
        {3840, 2160, 24, 97, false},   {4096, 2160, 24, 98, false},   {3840, 2160, 60, 102, false},
        {3840, 2160, 50, 103, false},  {4096, 2160, 60, 104, false},  {4096, 2160, 50, 105, false},
        {3840, 2160, 100, 117, false}, {3840, 2160, 120, 118, false}, {3840, 2160, 100, 119, false},
        {3840, 2160, 120, 120, false},
    };

    for (UINTN i = 0; i < sizeof(k_cta_vics) / sizeof(k_cta_vics[0]); i++) {
        if (k_cta_vics[i].vic != vic)
            continue;
        *out_hint = {};
        out_hint->valid = true;
        out_hint->width = k_cta_vics[i].width;
        out_hint->height = k_cta_vics[i].height;
        out_hint->refresh_millihz = static_cast<uint32_t>(k_cta_vics[i].refresh_hz) * 1000u;
        out_hint->interlaced = k_cta_vics[i].interlaced;
        return true;
    }
    return false;
}

static BootEdidModeHint parse_best_edid_mode_hint(const uint8_t *edid, UINTN edid_size)
{
    BootEdidModeHint best = {};
    bool have_best = false;
    uint32_t block_count = 0;
    if (!edid_buffer_valid(edid, edid_size, &block_count))
        return best;

    for (uint32_t offset = 54; offset + 18u <= 126u; offset += 18u) {
        BootEdidModeHint hint = {};
        if (parse_edid_detailed_timing(&edid[offset], &hint))
            consider_edid_hint(&best, &have_best, hint);
    }

    for (uint32_t block = 1; block < block_count; block++) {
        const uint8_t *ext = edid + static_cast<UINTN>(block) * k_edid_block_size;
        if (ext[0] == 0x02u) {
            uint32_t dtd_offset = ext[2];
            const bool valid_cta_data_range = dtd_offset >= 4u && dtd_offset <= 127u;
            if (!valid_cta_data_range)
                dtd_offset = 127u;
            if (valid_cta_data_range) {
                for (uint32_t offset = 4u; offset < dtd_offset;) {
                    uint8_t header = ext[offset];
                    uint32_t payload_len = header & 0x1Fu;
                    uint32_t next = offset + 1u + payload_len;
                    if (next > dtd_offset)
                        break;
                    if ((header >> 5) == 0x02u) {
                        for (uint32_t i = offset + 1u; i < next; i++) {
                            BootEdidModeHint hint = {};
                            if (parse_cta_vic_hint(ext[i] & 0x7Fu, &hint))
                                consider_edid_hint(&best, &have_best, hint);
                        }
                    }
                    offset = next;
                }
            }
            for (uint32_t offset = dtd_offset; offset + 18u <= 127u; offset += 18u) {
                BootEdidModeHint hint = {};
                if (parse_edid_detailed_timing(&ext[offset], &hint))
                    consider_edid_hint(&best, &have_best, hint);
            }
            continue;
        }

        if (ext[0] == 0x70u) {
            parse_displayid_extension_for_hint(ext, &best, &have_best);
            continue;
        }

        for (uint32_t offset = 0; offset + 18u <= 127u; offset += 18u) {
            BootEdidModeHint hint = {};
            if (parse_edid_detailed_timing(&ext[offset], &hint))
                consider_edid_hint(&best, &have_best, hint);
        }
    }

    return best;
}

static bool gop_pixel_format_supported(EFI_GRAPHICS_PIXEL_FORMAT format)
{
    return format == PixelRedGreenBlueReserved8BitPerColor || format == PixelBlueGreenRedReserved8BitPerColor ||
           format == PixelBitMask;
}

static bool copy_edid_from_protocols(EFI_HANDLE gop_handle, void **out_edid, UINTN *out_edid_size)
{
    if (!out_edid || !out_edid_size)
        return false;

    *out_edid = nullptr;
    *out_edid_size = 0;

    EFI_STATUS status = EFI_NOT_FOUND;
    EFI_EDID_ACTIVE_PROTOCOL *edid_active = nullptr;
    if (gop_handle) {
        status = g_boot_services->HandleProtocol(gop_handle, const_cast<EFI_GUID *>(&EFI_EDID_ACTIVE_PROTOCOL_GUID),
                                                 reinterpret_cast<void **>(&edid_active));
    }
    if (efi_error(status) || !edid_active) {
        status = g_boot_services->LocateProtocol(const_cast<EFI_GUID *>(&EFI_EDID_ACTIVE_PROTOCOL_GUID), nullptr,
                                                 reinterpret_cast<void **>(&edid_active));
    }
    if (!efi_error(status) && edid_active && edid_active->SizeOfEdid > 0 && edid_active->Edid) {
        void *edid_copy = nullptr;
        if (g_boot_services->AllocatePool(EfiLoaderData, edid_active->SizeOfEdid, &edid_copy) == EFI_SUCCESS) {
            copy_memory(edid_copy, edid_active->Edid, edid_active->SizeOfEdid);
            *out_edid = edid_copy;
            *out_edid_size = edid_active->SizeOfEdid;
            return true;
        }
    }

    EFI_EDID_DISCOVERED_PROTOCOL *edid_discovered = nullptr;
    if (gop_handle) {
        status = g_boot_services->HandleProtocol(gop_handle,
                                                 const_cast<EFI_GUID *>(&EFI_EDID_DISCOVERED_PROTOCOL_GUID),
                                                 reinterpret_cast<void **>(&edid_discovered));
    }
    if (efi_error(status) || !edid_discovered) {
        status = g_boot_services->LocateProtocol(const_cast<EFI_GUID *>(&EFI_EDID_DISCOVERED_PROTOCOL_GUID), nullptr,
                                                 reinterpret_cast<void **>(&edid_discovered));
    }
    if (!efi_error(status) && edid_discovered && edid_discovered->SizeOfEdid > 0 && edid_discovered->Edid) {
        void *edid_copy = nullptr;
        if (g_boot_services->AllocatePool(EfiLoaderData, edid_discovered->SizeOfEdid, &edid_copy) == EFI_SUCCESS) {
            copy_memory(edid_copy, edid_discovered->Edid, edid_discovered->SizeOfEdid);
            *out_edid = edid_copy;
            *out_edid_size = edid_discovered->SizeOfEdid;
            return true;
        }
    }

    return false;
}

struct GopModeCandidate
{
    UINT32 mode;
    uint32_t width;
    uint32_t height;
    uint64_t pixels;
    bool exact_hint;
    bool within_hint;
    bool square;
    bool current;
};

static bool gop_candidate_better(const GopModeCandidate &candidate, const GopModeCandidate &best, bool have_best,
                                 bool require_exact_hint, bool require_within_hint)
{
    if (!have_best)
        return true;
    if (require_exact_hint && candidate.exact_hint != best.exact_hint)
        return candidate.exact_hint;
    if (!require_exact_hint && require_within_hint && candidate.within_hint != best.within_hint)
        return candidate.within_hint;
    if (candidate.pixels != best.pixels)
        return candidate.pixels > best.pixels;
    if (candidate.square != best.square)
        return !candidate.square;
    if (candidate.width != best.width)
        return candidate.width > best.width;
    if (candidate.height != best.height)
        return candidate.height > best.height;
    if (candidate.current != best.current)
        return candidate.current;
    return candidate.mode < best.mode;
}

[[nodiscard]] static EFI_STATUS select_best_gop_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                                     const BootEdidModeHint *edid_hint)
{
    if (!gop || !gop->Mode || !gop->Mode->Info)
        return EFI_NOT_FOUND;

    UINT32 best_mode = gop->Mode->Mode;
    GopModeCandidate best = {};
    bool have_best = false;
    bool have_exact_hint_mode = false;
    bool have_within_hint_mode = false;
    bool have_within_cap_mode = false;

    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = nullptr;
        UINTN info_size = 0;
        EFI_STATUS status = gop->QueryMode(gop, mode, &info_size, &info);
        if (efi_error(status))
            continue;

        const bool usable = info->HorizontalResolution != 0 && info->VerticalResolution != 0 &&
                            gop_pixel_format_supported(info->PixelFormat);
        if (usable) {
            if (info->HorizontalResolution <= 1920 && info->VerticalResolution <= 1080) {
                have_within_cap_mode = true;
            }
            if (edid_hint && edid_hint->valid) {
                if (info->HorizontalResolution == edid_hint->width && info->VerticalResolution == edid_hint->height)
                    have_exact_hint_mode = true;
                if (info->HorizontalResolution <= edid_hint->width && info->VerticalResolution <= edid_hint->height)
                    have_within_hint_mode = true;
            }
        }
        g_boot_services->FreePool(info);
    }

    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = nullptr;
        UINTN info_size = 0;
        EFI_STATUS status = gop->QueryMode(gop, mode, &info_size, &info);
        if (efi_error(status))
            continue;

        if (info->HorizontalResolution == 0 || info->VerticalResolution == 0 ||
            !gop_pixel_format_supported(info->PixelFormat)) {
            g_boot_services->FreePool(info);
            continue;
        }

        // Capping workaround removed to allow bare-metal real hardware execution paths
        // to naturally scale up to native ultra-wide, 1440p, and 4K resolution envelopes.

        GopModeCandidate candidate = {};
        candidate.mode = mode;
        candidate.width = info->HorizontalResolution;
        candidate.height = info->VerticalResolution;
        candidate.pixels = static_cast<uint64_t>(candidate.width) * static_cast<uint64_t>(candidate.height);
        candidate.square = candidate.width == candidate.height;
        candidate.current = mode == gop->Mode->Mode;
        if (edid_hint && edid_hint->valid) {
            candidate.exact_hint = candidate.width == edid_hint->width && candidate.height == edid_hint->height;
            candidate.within_hint = candidate.width <= edid_hint->width && candidate.height <= edid_hint->height;
        }

        if (gop_candidate_better(candidate, best, have_best, have_exact_hint_mode, have_within_hint_mode)) {
            best = candidate;
            have_best = true;
            best_mode = mode;
        }
        g_boot_services->FreePool(info);
    }

    if (!have_best)
        return EFI_NOT_FOUND;

    if (best_mode == gop->Mode->Mode)
        return EFI_SUCCESS;

    EFI_STATUS status = gop->SetMode(gop, best_mode);
    if (efi_error(status))
        return fail_status("failed to switch GOP mode", status);
    return EFI_SUCCESS;
}

static void publish_boot_display_timing(const BootEdidModeHint &hint, uint32_t active_width, uint32_t active_height)
{
    if (!hint.valid || !hint.has_exact_timing || hint.width != active_width || hint.height != active_height)
        return;
    if (!g_boot_services || !g_boot_services->InstallConfigurationTable)
        return;

    using EFI_INSTALL_CONFIGURATION_TABLE = EFI_STATUS(EFIAPI *)(EFI_GUID *, void *);
    auto install_configuration_table =
        reinterpret_cast<EFI_INSTALL_CONFIGURATION_TABLE>(g_boot_services->InstallConfigurationTable);

    void *timing_buffer = nullptr;
    if (g_boot_services->AllocatePool(EfiLoaderData, sizeof(BootDisplayTiming), &timing_buffer) != EFI_SUCCESS)
        return;

    auto *timing = static_cast<BootDisplayTiming *>(timing_buffer);
    *timing = {};
    timing->revision = BOOT_DISPLAY_TIMING_REVISION;
    timing->flags = BOOT_DISPLAY_TIMING_FLAG_EXACT_ACTIVE;
    timing->width = hint.width;
    timing->height = hint.height;
    timing->refresh_millihz = hint.refresh_millihz;
    timing->pixel_clock_khz = hint.pixel_clock_khz;
    timing->h_total = hint.h_total;
    timing->v_total = hint.v_total;
    timing->interlaced = hint.interlaced ? 1u : 0u;

    EFI_STATUS status =
        install_configuration_table(const_cast<EFI_GUID *>(&EFI_BOOT_DISPLAY_TIMING_TABLE_GUID), timing);
    if (efi_error(status))
        g_boot_services->FreePool(timing_buffer);
}

[[nodiscard]] static EFI_STATUS locate_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop,
                                                   EFI_PHYSICAL_ADDRESS *out_base, uint64_t *out_size,
                                                   BootFramebuffer *out_framebuffer, BootVideoMode *out_mode)
{
    if (!out_gop || !out_base || !out_size || !out_framebuffer || !out_mode)
        return EFI_INVALID_PARAMETER;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = nullptr;
    EFI_HANDLE *handles = nullptr;
    UINTN handle_count = 0;

    EFI_STATUS status = g_boot_services->LocateHandleBuffer(
        ByProtocol, const_cast<EFI_GUID *>(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID), nullptr, &handle_count, &handles);
    if (efi_error(status) || handle_count == 0)
        return fail_status("failed to locate GOP handles", status);

    EFI_HANDLE gop_handle = nullptr;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *cand = nullptr;
        if (g_boot_services->HandleProtocol(handles[i], const_cast<EFI_GUID *>(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID),
                                            reinterpret_cast<void **>(&cand)) == EFI_SUCCESS) {
            if (cand && cand->Mode && cand->Mode->Info) {
                gop = cand;
                gop_handle = handles[i];
                break;
            }
        }
    }

    if (!gop || !gop_handle) {
        g_boot_services->FreePool(handles);
        return fail_status("failed to locate valid GOP on any handle", EFI_NOT_FOUND);
    }
    g_boot_services->FreePool(handles);

    void *edid_copy = nullptr;
    UINTN edid_size = 0;
    copy_edid_from_protocols(gop_handle, &edid_copy, &edid_size);
    BootEdidModeHint edid_hint = parse_best_edid_mode_hint(static_cast<const uint8_t *>(edid_copy), edid_size);

    status = select_best_gop_mode(gop, edid_hint.valid ? &edid_hint : nullptr);
    if (efi_error(status)) {
        if (edid_copy)
            g_boot_services->FreePool(edid_copy);
        return status;
    }

    if (!gop->Mode || !gop->Mode->Info || gop->Mode->FrameBufferBase == 0 || gop->Mode->FrameBufferSize == 0)
        return fail_status("GOP framebuffer is unavailable", EFI_NOT_FOUND);

    const auto *info = gop->Mode->Info;
    uint8_t red_size = 0;
    uint8_t red_shift = 0;
    uint8_t green_size = 0;
    uint8_t green_shift = 0;
    uint8_t blue_size = 0;
    uint8_t blue_shift = 0;
    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            red_size = 8;
            red_shift = 0;
            green_size = 8;
            green_shift = 8;
            blue_size = 8;
            blue_shift = 16;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            red_size = 8;
            red_shift = 16;
            green_size = 8;
            green_shift = 8;
            blue_size = 8;
            blue_shift = 0;
            break;
        case PixelBitMask:
            (void)derive_mask(info->PixelInformation.RedMask, &red_size, &red_shift);
            (void)derive_mask(info->PixelInformation.GreenMask, &green_size, &green_shift);
            (void)derive_mask(info->PixelInformation.BlueMask, &blue_size, &blue_shift);
            break;
        default:
            return fail_status("GOP pixel format is unsupported", EFI_UNSUPPORTED);
    }

    zero_memory(out_framebuffer, sizeof(*out_framebuffer));
    out_framebuffer->width = info->HorizontalResolution;
    out_framebuffer->height = info->VerticalResolution;
    out_framebuffer->pitch = static_cast<uint64_t>(info->PixelsPerScanLine) * 4ULL;
    out_framebuffer->bpp = 32;
    out_framebuffer->memory_model = 6;
    out_framebuffer->red_mask_size = red_size;
    out_framebuffer->red_mask_shift = red_shift;
    out_framebuffer->green_mask_size = green_size;
    out_framebuffer->green_mask_shift = green_shift;
    out_framebuffer->blue_mask_size = blue_size;
    out_framebuffer->blue_mask_shift = blue_shift;
    if (edid_copy && edid_size > 0) {
        out_framebuffer->edid_size = edid_size;
        out_framebuffer->edid = edid_copy;
    }

    zero_memory(out_mode, sizeof(*out_mode));
    out_mode->pitch = out_framebuffer->pitch;
    out_mode->width = out_framebuffer->width;
    out_mode->height = out_framebuffer->height;
    out_mode->bpp = out_framebuffer->bpp;
    out_mode->memory_model = out_framebuffer->memory_model;
    out_mode->red_mask_size = red_size;
    out_mode->red_mask_shift = red_shift;
    out_mode->green_mask_size = green_size;
    out_mode->green_mask_shift = green_shift;
    out_mode->blue_mask_size = blue_size;
    out_mode->blue_mask_shift = blue_shift;

    *out_gop = gop;
    *out_base = gop->Mode->FrameBufferBase;
    *out_size = gop->Mode->FrameBufferSize;

    publish_boot_display_timing(edid_hint, static_cast<uint32_t>(out_framebuffer->width),
                                static_cast<uint32_t>(out_framebuffer->height));

    return EFI_SUCCESS;
}

[[nodiscard]] static uint64_t boot_memory_type_from_efi(uint32_t efi_type)
{
    switch (efi_type) {
        case EfiConventionalMemory:
            return BOOT_MEM_USABLE;
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            return BOOT_MEM_BOOTLOADER_RECLAIMABLE;
        case EfiACPIReclaimMemory:
            return BOOT_MEM_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:
            return BOOT_MEM_ACPI_NVS;
        case EfiUnusableMemory:
            return BOOT_MEM_BAD;
        default:
            return BOOT_MEM_RESERVED;
    }
}

[[nodiscard]] static bool overlay_memory_range(BootMemoryMapEntry *entries, size_t *inout_count, size_t max_entries,
                                               uint64_t range_base, uint64_t range_length, uint64_t new_type)
{
    if (!entries || !inout_count || *inout_count > max_entries || range_length == 0)
        return true;

    static BootMemoryMapEntry temp[k_max_boot_memory_map_entries];
    const uint64_t range_limit = range_end(range_base, range_length);
    size_t out_count = 0;

    for (size_t i = 0; i < *inout_count; i++) {
        const BootMemoryMapEntry entry = entries[i];
        const uint64_t entry_limit = range_end(entry.base, entry.length);
        const bool disjoint = entry_limit <= range_base || entry.base >= range_limit;
        if (disjoint) {
            if (out_count >= max_entries)
                return false;
            temp[out_count++] = entry;
            continue;
        }

        if (entry.base < range_base) {
            if (out_count >= max_entries)
                return false;
            temp[out_count++] = {entry.base, range_base - entry.base, entry.type};
        }

        const uint64_t overlap_base = entry.base > range_base ? entry.base : range_base;
        const uint64_t overlap_limit = entry_limit < range_limit ? entry_limit : range_limit;
        if (overlap_limit > overlap_base) {
            if (out_count >= max_entries)
                return false;
            temp[out_count++] = {overlap_base, overlap_limit - overlap_base, new_type};
        }

        if (entry_limit > range_limit) {
            if (out_count >= max_entries)
                return false;
            temp[out_count++] = {range_limit, entry_limit - range_limit, entry.type};
        }
    }

    copy_memory(entries, temp, out_count * sizeof(BootMemoryMapEntry));
    *inout_count = out_count;
    return true;
}

[[nodiscard]] static size_t build_boot_memory_map(BootPayload *payload, const EFI_MEMORY_DESCRIPTOR *memory_map,
                                                  UINTN memory_map_size, UINTN descriptor_size,
                                                  uint64_t framebuffer_base, uint64_t framebuffer_size,
                                                  uint64_t kernel_phys_base, uint64_t kernel_size,
                                                  uint64_t module_phys_base, uint64_t module_size)
{
    if (!payload || !memory_map || descriptor_size == 0)
        return 0;

    size_t count = 0;
    for (UINTN offset = 0; offset < memory_map_size; offset += descriptor_size) {
        const auto *descriptor = reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(reinterpret_cast<const uint8_t *>(memory_map) + offset);
        if (descriptor->NumberOfPages == 0)
            continue;
        if (count >= k_max_boot_memory_map_entries)
            return 0;

        if (descriptor->NumberOfPages > UINT64_MAX / k_page_size)
            return 0;
        payload->memory_map[count].base = descriptor->PhysicalStart;
        payload->memory_map[count].length = descriptor->NumberOfPages * k_page_size;
        payload->memory_map[count].type = boot_memory_type_from_efi(descriptor->Type);
        count++;
    }

    if (!overlay_memory_range(payload->memory_map, &count, k_max_boot_memory_map_entries, kernel_phys_base, kernel_size,
                              BOOT_MEM_KERNEL_AND_MODULES)) {
        return 0;
    }
    if (!overlay_memory_range(payload->memory_map, &count, k_max_boot_memory_map_entries, module_phys_base, module_size,
                              BOOT_MEM_KERNEL_AND_MODULES)) {
        return 0;
    }
    if (!overlay_memory_range(payload->memory_map, &count, k_max_boot_memory_map_entries, framebuffer_base,
                              framebuffer_size, BOOT_MEM_FRAMEBUFFER)) {
        return 0;
    }

    return count;
}

[[nodiscard]] static bool estimate_page_tables(TableEstimator *estimator, const EFI_MEMORY_DESCRIPTOR *memory_map,
                                               UINTN memory_map_size, UINTN descriptor_size, uint64_t framebuffer_base,
                                               uint64_t framebuffer_size, uint64_t kernel_virt_base,
                                               uint64_t kernel_image_size)
{
    if (!estimator || !memory_map || descriptor_size == 0)
        return false;

    estimator->reset();

    // The estimation logic must perfectly mirror map_direct_range to accurately track level-1 page tables
    auto add_direct_range = [estimator](uint64_t phys_base, uint64_t phys_length) -> bool {
        if (phys_length == 0)
            return true;
        const uint64_t phys_limit = range_end(phys_base, phys_length);
        if (phys_limit > k_direct_map_limit)
            return false;

        uint64_t cursor = phys_base;
        while (cursor < phys_limit) {
            uint64_t remaining = phys_limit - cursor;
            
            if ((cursor & k_large_page_mask_inv) == 0 && remaining >= k_large_page_size) {
                if (!estimator->add_2m_range(cursor, k_large_page_size) ||
                    !estimator->add_2m_range(k_hhdm_base + cursor, k_large_page_size)) {
                    return false;
                }
                cursor += k_large_page_size;
            } else {
                if (!estimator->add_4k_range(cursor, k_page_size) ||
                    !estimator->add_4k_range(k_hhdm_base + cursor, k_page_size)) {
                    return false;
                }
                cursor += k_page_size;
            }
        }
        return true;
    };

    for (UINTN offset = 0; offset < memory_map_size; offset += descriptor_size) {
        const auto *descriptor = reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(reinterpret_cast<const uint8_t *>(memory_map) + offset);
        if (descriptor->NumberOfPages == 0 || descriptor->Type == EfiMemoryMappedIOPortSpace)
            continue;
        if (descriptor->NumberOfPages > UINT64_MAX / k_page_size)
            return false;
        if (!add_direct_range(descriptor->PhysicalStart, descriptor->NumberOfPages * k_page_size))
            return false;
    }

    if (!add_direct_range(framebuffer_base, framebuffer_size))
        return false;
    return estimator->add_4k_range(kernel_virt_base, kernel_image_size);
}

[[nodiscard]] static bool map_direct_range(PageTables *tables, uint64_t phys_base, uint64_t phys_length)
{
    if (!tables || phys_length == 0)
        return true;

    const uint64_t phys_limit = range_end(phys_base, phys_length);
    if (phys_limit > k_direct_map_limit)
        return false;

    uint64_t cursor = phys_base;
    while (cursor < phys_limit) {
        uint64_t remaining = phys_limit - cursor;
        
        // Use 2MB huge pages only if both the address and length align perfectly
        if ((cursor & k_large_page_mask_inv) == 0 && remaining >= k_large_page_size) {
            if (!tables->map_2m(cursor, cursor, k_pte_writable) ||
                !tables->map_2m(k_hhdm_base + cursor, cursor, k_pte_writable)) {
                return false;
            }
            cursor += k_large_page_size;
        } else {
            // Fall back to precise 4KB pages to protect adjacent unmapped regions
            if (!tables->map_4k(cursor, cursor, k_pte_writable) ||
                !tables->map_4k(k_hhdm_base + cursor, cursor, k_pte_writable)) {
                return false;
            }
            cursor += k_page_size;
        }
    }
    return true;
}

[[nodiscard]] static bool build_page_tables(PageTables *tables, uint64_t pool_phys, UINTN pool_pages,
                                            const EFI_MEMORY_DESCRIPTOR *memory_map, UINTN memory_map_size,
                                            UINTN descriptor_size, uint64_t framebuffer_base, uint64_t framebuffer_size,
                                            uint64_t kernel_virt_base, uint64_t kernel_phys_base,
                                            uint64_t kernel_image_size)
{
    if (!tables || !memory_map || descriptor_size == 0)
        return false;

    // Clear the entire memory allocation pool to wipe out stale sub-nodes from previous attempts
    zero_memory(reinterpret_cast<void *>(pool_phys), pool_pages * k_page_size);

    if (!tables->init(pool_phys, pool_pages))
        return false;

    for (UINTN offset = 0; offset < memory_map_size; offset += descriptor_size) {
        const auto *descriptor = reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(reinterpret_cast<const uint8_t *>(memory_map) + offset);
        if (descriptor->NumberOfPages == 0 || descriptor->Type == EfiMemoryMappedIOPortSpace)
            continue;
        if (descriptor->NumberOfPages > UINT64_MAX / k_page_size)
            return false;
        if (!map_direct_range(tables, descriptor->PhysicalStart, descriptor->NumberOfPages * k_page_size))
            return false;
    }

    if (!map_direct_range(tables, framebuffer_base, framebuffer_size))
        return false;

    for (uint64_t offset = 0; offset < kernel_image_size; offset += k_page_size) {
        if (!tables->map_4k(kernel_virt_base + offset, kernel_phys_base + offset, k_pte_writable))
            return false;
    }
    return true;
}

[[nodiscard]] static uint64_t find_acpi_rsdp(EFI_SYSTEM_TABLE *system_table)
{
    if (!system_table || !system_table->ConfigurationTable)
        return 0;

    for (UINTN i = 0; i < system_table->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE &entry = system_table->ConfigurationTable[i];
        if (guid_equal(entry.VendorGuid, EFI_ACPI_20_TABLE_GUID))
            return reinterpret_cast<uint64_t>(entry.VendorTable);
    }
    for (UINTN i = 0; i < system_table->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE &entry = system_table->ConfigurationTable[i];
        if (guid_equal(entry.VendorGuid, EFI_ACPI_TABLE_GUID))
            return reinterpret_cast<uint64_t>(entry.VendorTable);
    }
    return 0;
}

static void populate_boot_payload(BootPayload *payload, uint64_t payload_phys,
                                  const BootFramebuffer &framebuffer_template, const BootVideoMode &mode_template,
                                  uint64_t framebuffer_base, uint64_t rsdp_phys, uint64_t system_table_phys,
                                  uint64_t kernel_phys_base, uint64_t kernel_virt_base, uint64_t module_phys_base,
                                  uint64_t module_size, size_t memory_map_count)
{
    copy_string(payload->bootloader_name, sizeof(payload->bootloader_name), "Meridian");
    copy_string(payload->bootloader_version, sizeof(payload->bootloader_version), "0.1.0");
    copy_string(payload->module_path, sizeof(payload->module_path), "/unifs.img");

    payload->mode = mode_template;
    payload->mode_ptrs[0] = reinterpret_cast<BootVideoMode *>(k_hhdm_base + payload_phys + offsetof(BootPayload, mode));

    payload->framebuffer = framebuffer_template;
    payload->framebuffer.address = reinterpret_cast<void *>(k_hhdm_base + framebuffer_base);
    if (payload->framebuffer.edid) {
        payload->framebuffer.edid =
            reinterpret_cast<void *>(k_hhdm_base + reinterpret_cast<uintptr_t>(payload->framebuffer.edid));
    }
    payload->framebuffer.mode_count = 1;
    payload->framebuffer.modes =
        reinterpret_cast<BootVideoMode **>(k_hhdm_base + payload_phys + offsetof(BootPayload, mode_ptrs));

    payload->modules[0].address = reinterpret_cast<void *>(k_hhdm_base + module_phys_base);
    payload->modules[0].size = module_size;
    payload->modules[0].path =
        reinterpret_cast<const char *>(k_hhdm_base + payload_phys + offsetof(BootPayload, module_path));
    payload->modules[0].cmdline = nullptr;

    payload->info.magic = BOOT_INFO_MAGIC;
    payload->info.revision = BOOT_INFO_REVISION;
    payload->info.size = sizeof(BootInfo);
    payload->info.hhdm_offset = k_hhdm_base;
    payload->info.kernel_physical_base = kernel_phys_base;
    payload->info.kernel_virtual_base = kernel_virt_base;
    payload->info.framebuffer =
        reinterpret_cast<BootFramebuffer *>(k_hhdm_base + payload_phys + offsetof(BootPayload, framebuffer));
    payload->info.framebuffer_count = 1;
    payload->info.firmware_type = BOOT_FIRMWARE_UEFI64;
    payload->info.rsdp_address = rsdp_phys;
    payload->info.efi_system_table_address = system_table_phys;
    payload->info.bootloader_name =
        reinterpret_cast<const char *>(k_hhdm_base + payload_phys + offsetof(BootPayload, bootloader_name));
    payload->info.bootloader_version =
        reinterpret_cast<const char *>(k_hhdm_base + payload_phys + offsetof(BootPayload, bootloader_version));
    payload->info.module_count = 1;
    payload->info.modules = reinterpret_cast<BootModule *>(k_hhdm_base + payload_phys + offsetof(BootPayload, modules));
    payload->info.memory_map_count = memory_map_count;
    payload->info.memory_map =
        reinterpret_cast<BootMemoryMapEntry *>(k_hhdm_base + payload_phys + offsetof(BootPayload, memory_map));
}

} // namespace

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    g_system_table = system_table;
    g_boot_services = system_table ? system_table->BootServices : nullptr;
    g_con_out = system_table ? system_table->ConOut : nullptr;

    console_write_line("[UEFI] Meridian loader");

    if (!system_table || !g_boot_services)
        return EFI_INVALID_PARAMETER;

    if (g_boot_services->SetWatchdogTimer)
        g_boot_services->SetWatchdogTimer(0, 0, 0, nullptr);

    console_write_line("[UEFI] opening EFI volume");
    EFI_FILE_PROTOCOL *root = nullptr;
    EFI_STATUS status = open_root_volume(image_handle, &root);
    if (efi_error(status))
        return status;

    console_write_line("[UEFI] reading KERNEL.ELF");
    void *kernel_file = nullptr;
    UINTN kernel_file_size = 0;
    status = read_entire_file(root, k_kernel_path, &kernel_file, &kernel_file_size);
    if (efi_error(status)) {
        root->Close(root);
        return status;
    }

    console_write_line("[UEFI] reading UNIFS.IMG");
    uint64_t module_phys_base = 0;
    uint64_t module_size = 0;
    status = read_file_to_pages(root, k_unifs_path, &module_phys_base, &module_size);
    if (efi_error(status)) {
        g_boot_services->FreePool(kernel_file);
        root->Close(root);
        return status;
    }
    root->Close(root);

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = nullptr;
    EFI_PHYSICAL_ADDRESS framebuffer_base = 0;
    uint64_t framebuffer_size = 0;
    BootFramebuffer framebuffer = {};
    BootVideoMode mode = {};
    console_write_line("[UEFI] locating framebuffer");
    status = locate_framebuffer(&gop, &framebuffer_base, &framebuffer_size, &framebuffer, &mode);
    if (efi_error(status)) {
        g_boot_services->FreePool(kernel_file);
        g_boot_services->FreePages(module_phys_base, page_count_for_bytes(module_size));
        return status;
    }

    console_write_line("[UEFI] loading kernel image");
    uint64_t kernel_entry = 0;
    uint64_t kernel_virt_base = 0;
    uint64_t kernel_phys_base = 0;
    uint64_t kernel_image_size = 0;
    status = load_kernel_image(kernel_file, kernel_file_size, &kernel_entry, &kernel_virt_base, &kernel_phys_base,
                               &kernel_image_size);
    g_boot_services->FreePool(kernel_file);
    if (efi_error(status)) {
        g_boot_services->FreePages(module_phys_base, page_count_for_bytes(module_size));
        return status;
    }

    console_write_line("[UEFI] allocating boot payload");
    EFI_PHYSICAL_ADDRESS payload_phys = 0;
    status = g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, k_boot_payload_pages, &payload_phys);
    if (efi_error(status))
        return fail_status("failed to allocate boot payload buffer", status);

    EFI_PHYSICAL_ADDRESS memory_map_buffer_phys = 0;
    status = g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, k_memory_map_buffer_pages,
                                            &memory_map_buffer_phys);
    if (efi_error(status))
        return fail_status("failed to allocate EFI memory map buffer", status);

    EFI_PHYSICAL_ADDRESS kernel_stack_phys = 0;
    status = g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, k_kernel_stack_pages, &kernel_stack_phys);
    if (efi_error(status))
        return fail_status("failed to allocate kernel stack", status);

    auto *payload = reinterpret_cast<BootPayload *>(payload_phys);
    auto *memory_map = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(memory_map_buffer_phys);
    const uint64_t rsdp_phys = find_acpi_rsdp(system_table);

    UINTN memory_map_size = k_memory_map_buffer_pages * k_page_size;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    console_write_line("[UEFI] fetching EFI memory map");
    status =
        g_boot_services->GetMemoryMap(&memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version);
    if (efi_error(status))
        return fail_status("failed to fetch EFI memory map", status);

    console_write_line("[UEFI] sizing page tables");
    g_table_estimator.reset();
    if (!estimate_page_tables(&g_table_estimator, memory_map, memory_map_size, descriptor_size, framebuffer_base,
                              framebuffer_size, kernel_virt_base, kernel_image_size)) {
        return fail_status("failed to size initial page tables", EFI_OUT_OF_RESOURCES);
    }

    const UINTN page_table_pool_pages = g_table_estimator.total_pages() + k_page_table_margin_pages;
    EFI_PHYSICAL_ADDRESS page_table_pool_phys = 0;
    status =
        g_boot_services->AllocatePages(AllocateAnyPages, EfiLoaderData, page_table_pool_pages, &page_table_pool_phys);
    if (efi_error(status))
        return fail_status("failed to allocate page table pool", status);

    PageTables page_tables = {};
    bool exited_boot_services = false;
    console_write_line("[UEFI] exiting boot services");
    for (int attempt = 0; attempt < 4; attempt++) {
        memory_map_size = k_memory_map_buffer_pages * k_page_size;
        status = g_boot_services->GetMemoryMap(&memory_map_size, memory_map, &map_key, &descriptor_size,
                                               &descriptor_version);
        if (efi_error(status))
            return fail_status("failed to refresh EFI memory map", status);

        const size_t boot_memory_map_count =
            build_boot_memory_map(payload, memory_map, memory_map_size, descriptor_size, framebuffer_base,
                                  framebuffer_size, kernel_phys_base, kernel_image_size, module_phys_base, module_size);
        if (boot_memory_map_count == 0)
            return fail_status("failed to build boot memory map", EFI_OUT_OF_RESOURCES);

        populate_boot_payload(payload, payload_phys, framebuffer, mode, framebuffer_base, rsdp_phys,
                              reinterpret_cast<uint64_t>(system_table), kernel_phys_base, kernel_virt_base,
                              module_phys_base, module_size, boot_memory_map_count);

        if (!build_page_tables(&page_tables, page_table_pool_phys, page_table_pool_pages, memory_map, memory_map_size,
                               descriptor_size, framebuffer_base, framebuffer_size, kernel_virt_base, kernel_phys_base,
                               kernel_image_size)) {
            return fail_status("failed to build kernel page tables", EFI_OUT_OF_RESOURCES);
        }

        status = g_boot_services->ExitBootServices(image_handle, map_key);
        if (status == EFI_SUCCESS) {
            exited_boot_services = true;
            break;
        }
        if (status != EFI_INVALID_PARAMETER)
            return fail_status("ExitBootServices failed", status);
    }

    if (!exited_boot_services)
        return fail_status("ExitBootServices never stabilized", EFI_ABORTED);

    const uint64_t boot_info_virt = k_hhdm_base + payload_phys;
    const uint64_t stack_top = k_hhdm_base + kernel_stack_phys + k_kernel_stack_pages * k_page_size;
    boot_jump_to_kernel(kernel_entry, reinterpret_cast<BootInfo *>(boot_info_virt), stack_top, page_tables.pml4_phys);
}

extern "C" void *memcpy(void *dst, const void *src, size_t size)
{
    return copy_memory(dst, src, size);
}

extern "C" void *memset(void *dst, int value, size_t size)
{
    return set_memory(dst, value, size);
}

extern "C" int memcmp(const void *lhs, const void *rhs, size_t size)
{
    return compare_memory(lhs, rhs, size);
}

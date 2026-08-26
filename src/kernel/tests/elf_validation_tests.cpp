#include <kernel/elf.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <libk/kstring.h>

namespace {

struct [[gnu::packed]] TestElf
{
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
    uint8_t payload[64];
};

static void make_valid(TestElf &e)
{
    kstring::zero_memory(&e, sizeof(e));
    *reinterpret_cast<uint32_t *>(e.ehdr.e_ident) = ELF_MAGIC;
    e.ehdr.e_ident[4] = ELFCLASS64;
    e.ehdr.e_ident[5] = ELFDATA2LSB;
    e.ehdr.e_type = ET_EXEC;
    e.ehdr.e_machine = EM_X86_64;
    e.ehdr.e_entry = 0x400000;
    e.ehdr.e_phoff = sizeof(Elf64_Ehdr);
    e.ehdr.e_phentsize = sizeof(Elf64_Phdr);
    e.ehdr.e_phnum = 1;

    e.phdr.p_type = PT_LOAD;
    e.phdr.p_flags = PF_R | PF_X;
    e.phdr.p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    e.phdr.p_vaddr = 0x400000;
    e.phdr.p_filesz = sizeof(e.payload);
    e.phdr.p_memsz = sizeof(e.payload);
}

static bool validate(const TestElf &e, uint64_t size_override = 0)
{
    return elf_validate(reinterpret_cast<const uint8_t *>(&e), size_override ? size_override : sizeof(TestElf));
}

} // namespace

KTEST(elf_validate_accepts_minimal_image)
{
    TestElf e;
    make_valid(e);
    KTEST_EXPECT(validate(e));
}

KTEST(elf_validate_rejects_bad_identity)
{
    TestElf e;
    make_valid(e);

    TestElf bad = e;
    KTEST_EXPECT(!validate(bad, sizeof(Elf64_Ehdr) - 1)); // truncated header

    bad = e;
    bad.ehdr.e_ident[0] = 0x00; // broken magic
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_ident[4] = 1; // ELFCLASS32
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_ident[5] = 2; // big-endian
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_machine = 3; // EM_386
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_type = 1; // ET_REL
    KTEST_EXPECT(!validate(bad));
}

KTEST(elf_validate_rejects_bad_phdr_table)
{
    TestElf e;
    make_valid(e);

    TestElf bad = e;
    bad.ehdr.e_phnum = 0;
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_phentsize = sizeof(Elf64_Phdr) - 1;
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_phoff = sizeof(TestElf); // table starts past EOF
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_phoff = 0xFFFFFFFFFFFFFF00ULL; // phoff + bytes overflows u64
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_phnum = 0xFFFF; // table extends past EOF
    KTEST_EXPECT(!validate(bad));
}

KTEST(elf_validate_rejects_bad_segments)
{
    TestElf e;
    make_valid(e);

    TestElf bad = e;
    bad.phdr.p_filesz = bad.phdr.p_memsz + 1;
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.phdr.p_offset = sizeof(TestElf); // payload starts past EOF
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.phdr.p_offset = 0xFFFFFFFFFFFFFF00ULL; // offset + filesz overflows
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.phdr.p_memsz = 0xFFFFFFFFFFFFFFFFULL; // vaddr + memsz overflows
    KTEST_EXPECT(!validate(bad));
}

// Privilege boundary: no segment or entry point may reach the kernel half.
// A crafted image mapping the higher half could remap kernel pages through
// the loader's flag-merge path.
KTEST(elf_validate_rejects_kernel_addresses)
{
    TestElf e;
    make_valid(e);

    TestElf bad = e;
    bad.phdr.p_vaddr = 0xFFFF800000000000ULL; // higher-half segment
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.phdr.p_vaddr = 0x00007FFFFFFFFFFFULL; // user half...
    bad.phdr.p_memsz = 0x1000;                // ...but end crosses the limit
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_entry = 0x0000800000000000ULL;
    KTEST_EXPECT(!validate(bad));

    bad = e;
    bad.ehdr.e_entry = 0xFFFF800000000000ULL;
    KTEST_EXPECT(!validate(bad));

    // Top of the user half is still valid (entry must land in the segment).
    TestElf ok = e;
    ok.phdr.p_vaddr = 0x00007FFFFFFFFFFFULL;
    ok.phdr.p_memsz = 1;
    ok.phdr.p_filesz = 1;
    ok.ehdr.e_entry = 0x00007FFFFFFFFFFFULL;
    KTEST_EXPECT(validate(ok));
}

// Non-LOAD segments are not validated as loadable and must not fail the image
// as long as a valid LOAD segment still covers the entry point.
KTEST(elf_validate_ignores_non_load_segments)
{
    struct [[gnu::packed]] TwoPhdrElf
    {
        Elf64_Ehdr ehdr;
        Elf64_Phdr load;
        Elf64_Phdr other;
        uint8_t payload[64];
    };

    TwoPhdrElf e;
    kstring::zero_memory(&e, sizeof(e));
    *reinterpret_cast<uint32_t *>(e.ehdr.e_ident) = ELF_MAGIC;
    e.ehdr.e_ident[4] = ELFCLASS64;
    e.ehdr.e_ident[5] = ELFDATA2LSB;
    e.ehdr.e_type = ET_EXEC;
    e.ehdr.e_machine = EM_X86_64;
    e.ehdr.e_entry = 0x400000;
    e.ehdr.e_phoff = sizeof(Elf64_Ehdr);
    e.ehdr.e_phentsize = sizeof(Elf64_Phdr);
    e.ehdr.e_phnum = 2;

    e.load.p_type = PT_LOAD;
    e.load.p_flags = PF_R | PF_X;
    e.load.p_offset = sizeof(TwoPhdrElf) - sizeof(e.payload);
    e.load.p_vaddr = 0x400000;
    e.load.p_filesz = sizeof(e.payload);
    e.load.p_memsz = sizeof(e.payload);

    // Garbage non-LOAD segment: ignored by validation.
    e.other.p_type = PT_DYNAMIC;
    e.other.p_offset = 0xFFFFFFFFFFFFFFFFULL;
    e.other.p_filesz = 0xFFFFFFFFFFFFFFFFULL;

    KTEST_EXPECT(elf_validate(reinterpret_cast<const uint8_t *>(&e), sizeof(e)));
}

// The entry point must sit inside a PT_LOAD segment; a dangling entry is a
// guaranteed fault at exec time.
KTEST(elf_validate_rejects_dangling_entry)
{
    TestElf e;
    make_valid(e);
    e.ehdr.e_entry = 0x500000; // outside the only segment
    KTEST_EXPECT(!validate(e));
}

// Regression: the BSS zero-fill must be clamped to each page. Before the fix,
// pages deep in the BSS (vaddr + filesz below page_vaddr) computed the zero
// range from the unclamped file_hi, wrapping the write backwards into
// unrelated physical memory below the segment's frame and silently shredding
// whatever lived there — the "very large PT_LOAD segments corrupt the system"
// bug. Load a multi-page-BSS segment into a scratch address space and verify
// every BSS byte is zero while the file-backed bytes survive.
KTEST(elf_load_user_zeroes_multipage_bss_in_bounds)
{
    constexpr uint64_t k_vaddr = 0x4005B0;                    // unaligned, like real binaries
    constexpr uint64_t k_filesz = 0x108;                      // payload bytes
    constexpr uint64_t k_memsz = k_filesz + 4 * 4096 + 0x240; // ~4.6 pages of BSS
    constexpr uint64_t k_entry = k_vaddr;

    struct [[gnu::packed]] LoadElf
    {
        Elf64_Ehdr ehdr;
        Elf64_Phdr phdr;
        uint8_t payload[k_filesz];
    };

    static LoadElf e;
    kstring::zero_memory(&e, sizeof(e));
    *reinterpret_cast<uint32_t *>(e.ehdr.e_ident) = ELF_MAGIC;
    e.ehdr.e_ident[4] = ELFCLASS64;
    e.ehdr.e_ident[5] = ELFDATA2LSB;
    e.ehdr.e_type = ET_EXEC;
    e.ehdr.e_machine = EM_X86_64;
    e.ehdr.e_entry = k_entry;
    e.ehdr.e_phoff = sizeof(Elf64_Ehdr);
    e.ehdr.e_phentsize = sizeof(Elf64_Phdr);
    e.ehdr.e_phnum = 1;

    e.phdr.p_type = PT_LOAD;
    e.phdr.p_flags = PF_R | PF_W;
    e.phdr.p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    e.phdr.p_vaddr = k_vaddr;
    e.phdr.p_filesz = k_filesz;
    e.phdr.p_memsz = k_memsz;

    for (uint64_t i = 0; i < k_filesz; i++)
        e.payload[i] = static_cast<uint8_t>(i * 7 + 3);

    uint64_t *pml4 = vmm_create_address_space();
    KTEST_EXPECT(pml4 != nullptr);

    Process *loader = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    KTEST_EXPECT(loader != nullptr);
    kstring::zero_memory(loader, sizeof(Process));
    loader->page_table = pml4;
    loader->vma_list = nullptr;

    const uint64_t entry = elf_load_user(reinterpret_cast<const uint8_t *>(&e), sizeof(e), loader);
    KTEST_EXPECT(entry == k_entry);

    const uint64_t hhdm = vmm_get_hhdm_offset();

    // File-backed bytes survived the copy. vmm_virt_to_phys_in already folds
    // in the page offset, so the HHDM address lands on the exact byte.
    bool data_ok = true;
    for (uint64_t off = 0; off < k_filesz && data_ok; off++) {
        const uint64_t phys = vmm_virt_to_phys_in(pml4, k_vaddr + off);
        if (!phys) {
            data_ok = false;
            break;
        }
        const uint8_t got = *reinterpret_cast<const uint8_t *>(phys + hhdm);
        if (got != static_cast<uint8_t>(off * 7 + 3))
            data_ok = false;
    }
    KTEST_EXPECT(data_ok);

    // Every BSS byte across the whole multi-page tail reads back zero.
    bool bss_zero = true;
    uint64_t va = k_vaddr + k_filesz;
    const uint64_t seg_end = k_vaddr + k_memsz;
    while (va < seg_end && bss_zero) {
        const uint64_t page_va = va & ~0xFFFULL;
        const uint64_t phys = vmm_virt_to_phys_in(pml4, page_va);
        if (!phys) {
            bss_zero = false;
            break;
        }
        const uint8_t *page = reinterpret_cast<const uint8_t *>(phys + hhdm);
        const uint64_t start = va - page_va;
        uint64_t stop = page_va + 4096;
        if (stop > seg_end)
            stop = seg_end;
        for (uint64_t b = start; b < (stop - page_va); b++) {
            if (page[b] != 0) {
                bss_zero = false;
                break;
            }
        }
        va = page_va + 4096;
    }
    KTEST_EXPECT(bss_zero);

    if (loader->vma_list)
        vma_free_all(loader->vma_list);
    vmm_free_address_space(pml4);
    aligned_free(loader);
}

// Overlapping PT_LOAD segments would merge W and X permissions on the shared
// pages and double-map frames; they are rejected outright.
KTEST(elf_validate_rejects_overlapping_segments)
{
    struct [[gnu::packed]] TwoPhdrElf
    {
        Elf64_Ehdr ehdr;
        Elf64_Phdr first;
        Elf64_Phdr second;
        uint8_t payload[64];
    };

    TwoPhdrElf e;
    kstring::zero_memory(&e, sizeof(e));
    *reinterpret_cast<uint32_t *>(e.ehdr.e_ident) = ELF_MAGIC;
    e.ehdr.e_ident[4] = ELFCLASS64;
    e.ehdr.e_ident[5] = ELFDATA2LSB;
    e.ehdr.e_type = ET_EXEC;
    e.ehdr.e_machine = EM_X86_64;
    e.ehdr.e_entry = 0x400000;
    e.ehdr.e_phoff = sizeof(Elf64_Ehdr);
    e.ehdr.e_phentsize = sizeof(Elf64_Phdr);
    e.ehdr.e_phnum = 2;

    e.first.p_type = PT_LOAD;
    e.first.p_flags = PF_R | PF_X;
    e.first.p_offset = sizeof(TwoPhdrElf) - sizeof(e.payload);
    e.first.p_vaddr = 0x400000;
    e.first.p_filesz = sizeof(e.payload);
    e.first.p_memsz = sizeof(e.payload);

    e.second = e.first;
    e.second.p_flags = PF_R | PF_W;
    e.second.p_vaddr = 0x400020; // overlaps the first segment

    KTEST_EXPECT(!elf_validate(reinterpret_cast<const uint8_t *>(&e), sizeof(e)));
}

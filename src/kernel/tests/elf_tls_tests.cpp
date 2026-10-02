#include <kernel/elf.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>
#include <uapi/tcb.h>

namespace {

// Ehdr + [PT_LOAD, PT_TLS] phdr table + payload, laid out like the images
// the real linker emits: the TLS template's file bytes sit inside the LOAD
// segment's file range and its vaddr inside the LOAD segment's memory.
struct [[gnu::packed]] TlsElf
{
    Elf64_Ehdr ehdr;
    Elf64_Phdr load;
    Elf64_Phdr tls;
    uint8_t payload[64];
};

constexpr uint64_t k_load_vaddr = 0x400000;
constexpr uint64_t k_tls_offset = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
constexpr uint64_t k_tls_filesz = 8; // .tdata bytes carried in the file
constexpr uint64_t k_tls_memsz = 12; // + 4 bytes of .tbss, file-less
constexpr uint64_t k_tls_align = 4;
constexpr uint64_t k_tls_vaddr = k_load_vaddr; // inside the PT_LOAD

static void make_tls_image(TlsElf &e)
{
    kstring::zero_memory(&e, sizeof(e));
    *reinterpret_cast<uint32_t *>(e.ehdr.e_ident) = ELF_MAGIC;
    e.ehdr.e_ident[4] = ELFCLASS64;
    e.ehdr.e_ident[5] = ELFDATA2LSB;
    e.ehdr.e_type = ET_EXEC;
    e.ehdr.e_machine = EM_X86_64;
    e.ehdr.e_entry = k_load_vaddr;
    e.ehdr.e_phoff = sizeof(Elf64_Ehdr);
    e.ehdr.e_phentsize = sizeof(Elf64_Phdr);
    e.ehdr.e_phnum = 2;

    e.load.p_type = PT_LOAD;
    e.load.p_flags = PF_R | PF_W;
    e.load.p_offset = k_tls_offset;
    e.load.p_vaddr = k_load_vaddr;
    e.load.p_filesz = sizeof(e.payload);
    e.load.p_memsz = sizeof(e.payload);

    e.tls.p_type = PT_TLS;
    e.tls.p_flags = PF_R;
    e.tls.p_offset = k_tls_offset;
    e.tls.p_vaddr = k_tls_vaddr;
    e.tls.p_filesz = k_tls_filesz;
    e.tls.p_memsz = k_tls_memsz;
    e.tls.p_align = k_tls_align;

    for (uint32_t i = 0; i < sizeof(e.payload); i++)
        e.payload[i] = static_cast<uint8_t>(i * 11 + 5);
}

} // namespace

KTEST(elf_tls_info_found)
{
    TlsElf e;
    make_tls_image(e);

    uint64_t off = 0, memsz = 0, align = 0;
    KTEST_EXPECT(elf_tls_info(reinterpret_cast<const uint8_t *>(&e), sizeof(e), &off, &memsz, &align));
    KTEST_EXPECT_EQ(off, k_tls_offset);
    KTEST_EXPECT_EQ(memsz, k_tls_memsz);
    KTEST_EXPECT_EQ(align, k_tls_align);

    // Extended outs: the template's landing vaddr and its file-carried size.
    uint64_t vaddr = 0, filesz = 0;
    KTEST_EXPECT(elf_tls_info(reinterpret_cast<const uint8_t *>(&e), sizeof(e), &off, &memsz, &align, &vaddr, &filesz));
    KTEST_EXPECT_EQ(vaddr, k_tls_vaddr);
    KTEST_EXPECT_EQ(filesz, k_tls_filesz);

    // False must still zero the outputs (callers build TCB-only installs
    // from a false return without re-initializing).
    TlsElf absent = e;
    absent.tls.p_type = PT_NULL;
    off = memsz = align = vaddr = filesz = 0xAAAAAAAA;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&absent), sizeof(absent), &off, &memsz, &align, &vaddr,
                               &filesz));
    KTEST_EXPECT_EQ(off, 0ULL);
    KTEST_EXPECT_EQ(memsz, 0ULL);
    KTEST_EXPECT_EQ(align, 0ULL);
    KTEST_EXPECT_EQ(vaddr, 0ULL);
    KTEST_EXPECT_EQ(filesz, 0ULL);
}

KTEST(elf_tls_info_absent)
{
    TlsElf e;
    make_tls_image(e);

    // No PT_TLS anywhere: the LOAD sibling remains, only the type changes.
    e.tls.p_type = PT_DYNAMIC;
    uint64_t off = 0, memsz = 0, align = 0;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&e), sizeof(e), &off, &memsz, &align));

    // A second image whose only phdr is the LOAD: also absent.
    e.tls.p_type = PT_NULL;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&e), sizeof(e), &off, &memsz, &align));
}

KTEST(elf_tls_info_truncated)
{
    TlsElf e;
    make_tls_image(e);

    const uint8_t *image = reinterpret_cast<const uint8_t *>(&e);
    uint64_t off = 0, memsz = 0, align = 0;

    // Image cut in the middle of the phdr table.
    KTEST_EXPECT(!elf_tls_info(image, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) + 8, &off, &memsz, &align));

    // Header alone.
    KTEST_EXPECT(!elf_tls_info(image, sizeof(Elf64_Ehdr), &off, &memsz, &align));

    // Bad magic.
    TlsElf bad = e;
    bad.ehdr.e_ident[0] = 0x00;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&bad), sizeof(bad), &off, &memsz, &align));

    // Phdr table past the end of the image.
    bad = e;
    bad.ehdr.e_phoff = sizeof(TlsElf);
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&bad), sizeof(bad), &off, &memsz, &align));

    // Template file bytes running past the end of the image.
    bad = e;
    bad.tls.p_offset = sizeof(TlsElf) - k_tls_filesz + 1;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&bad), sizeof(bad), &off, &memsz, &align));

    // p_filesz larger than p_memsz is malformed, same rule as PT_LOAD.
    bad = e;
    bad.tls.p_filesz = bad.tls.p_memsz + 1;
    KTEST_EXPECT(!elf_tls_info(reinterpret_cast<const uint8_t *>(&bad), sizeof(bad), &off, &memsz, &align));
}

namespace {

// A throwaway loader like the one do_exec/kernel_exec build: fresh address
// space, fresh VMA list, own vma lock, arbitrary pid.
Process *scratch_loader()
{
    uint64_t *pml4 = vmm_create_address_space();
    if (!pml4)
        return nullptr;
    Process *loader = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!loader) {
        vmm_free_address_space(pml4);
        return nullptr;
    }
    kstring::zero_memory(loader, sizeof(Process));
    loader->page_table = pml4;
    loader->vmalist = vma_list_alloc();
    if (!loader->vmalist) {
        vmm_free_address_space(pml4);
        aligned_free(loader);
        return nullptr;
    }
    spinlock_init(&loader->vma_lock);
    loader->vma_lock_ptr = &loader->vma_lock;
    loader->pid = 0x1234;
    return loader;
}

void scratch_loader_free(Process *loader)
{
    if (!loader)
        return;
    if (loader->vmalist) {
        if (loader->vmalist->head)
            vma_free_all(loader->vmalist->head);
        vma_list_free(loader->vmalist);
    }
    vmm_free_address_space(loader->page_table);
    aligned_free(loader);
}

} // namespace

// The loader records the image's TLS facts after mapping the segments:
// template va/size/align for a TLS-carrying image, all zero without one.
// A loader copied from a live process would carry the old image's values,
// so the zeroing must be explicit.
KTEST(elf_load_user_records_tls_facts)
{
    TlsElf e;
    make_tls_image(e);

    Process *loader = scratch_loader();
    KTEST_EXPECT(loader != nullptr);
    if (!loader)
        return;

    // Stale facts like a memcpy'd loader would hold: must be overwritten.
    loader->tls_template_va = 0xDEAD0000;
    loader->tls_template_size = 0xEE;
    loader->tls_align = 0x77;

    const uint64_t entry = elf_load_user(reinterpret_cast<const uint8_t *>(&e), sizeof(e), loader);
    KTEST_EXPECT(entry == k_load_vaddr);
    KTEST_EXPECT_EQ(loader->tls_template_va, k_tls_vaddr);
    KTEST_EXPECT_EQ(loader->tls_template_size, k_tls_memsz);
    KTEST_EXPECT_EQ(loader->tls_align, k_tls_align);

    scratch_loader_free(loader);

    // Same image with the TLS phdr type cleared: facts reset to zero.
    TlsElf absent = e;
    absent.tls.p_type = PT_NULL;

    loader = scratch_loader();
    KTEST_EXPECT(loader != nullptr);
    if (!loader)
        return;
    loader->tls_template_va = 0xDEAD0000;
    loader->tls_template_size = 0xEE;
    loader->tls_align = 0x77;

    KTEST_EXPECT(elf_load_user(reinterpret_cast<const uint8_t *>(&absent), sizeof(absent), loader) == k_load_vaddr);
    KTEST_EXPECT_EQ(loader->tls_template_va, 0ULL);
    KTEST_EXPECT_EQ(loader->tls_template_size, 0ULL);
    KTEST_EXPECT_EQ(loader->tls_align, 0ULL);

    scratch_loader_free(loader);
}

namespace {

const uint8_t *direct_read(Process *p, uint64_t va)
{
    uint64_t *pml4 = p->page_table ? p->page_table : vmm_get_kernel_pml4();
    const uint64_t phys = vmm_virt_to_phys_in(pml4, va);
    if (phys == 0)
        return nullptr;
    // vmm_virt_to_phys_in folds the page offset into the returned address.
    return reinterpret_cast<const uint8_t *>(vmm_phys_to_virt(phys));
}

} // namespace

// The exec-side install: after elf_load_user mapped the segments, install
// the leader's TLS block + TCB from the image buffer. The .tbss tail
// ([filesz, memsz)) must read zero even though the file carries other
// bytes there — the file image never holds the tbss part.
KTEST(elf_install_tls_clones_template_and_tcb)
{
    TlsElf e;
    make_tls_image(e);
    // Garbage where the file does NOT own the block: the install must
    // zero-fill the tail, never copy it.
    e.payload[k_tls_filesz] = 0xFF;
    e.payload[k_tls_filesz + 1] = 0xFF;
    e.payload[k_tls_filesz + 2] = 0xFF;
    e.payload[k_tls_filesz + 3] = 0xFF;

    Process *loader = scratch_loader();
    KTEST_EXPECT(loader != nullptr);
    if (!loader)
        return;

    KTEST_EXPECT(elf_load_user(reinterpret_cast<const uint8_t *>(&e), sizeof(e), loader) == k_load_vaddr);
    KTEST_EXPECT(elf_install_tls(loader, reinterpret_cast<const uint8_t *>(&e), sizeof(e)));

    const uint64_t fs = loader->fs_base;
    KTEST_EXPECT(fs != 0);

    // Block: file-carried prefix byte for byte, zero tail.
    const uint8_t *block = direct_read(loader, fs - k_tls_memsz);
    KTEST_EXPECT(block != nullptr);
    if (block) {
        bool match = true;
        for (uint64_t i = 0; i < k_tls_memsz; i++) {
            const uint8_t want = (i < k_tls_filesz) ? static_cast<uint8_t>(i * 11 + 5) : 0;
            if (block[i] != want) {
                match = false;
                break;
            }
        }
        KTEST_EXPECT(match);
    }

    // TCB: self points at itself, tid is the loader's pid (exec preserves
    // the pid, so the do_exec loader already carries the leader's).
    const UniTcb *tcb = reinterpret_cast<const UniTcb *>(direct_read(loader, fs));
    KTEST_EXPECT(tcb != nullptr);
    if (tcb) {
        KTEST_EXPECT_EQ(tcb->self, fs);
        KTEST_EXPECT_EQ(tcb->tid, 0x1234ULL);
    }

    scratch_loader_free(loader);
}

// Always-TCB invariant: an image without PT_TLS still installs a TCB-only
// mapping, so every user thread gets a valid fs:0.
KTEST(elf_install_tls_absent_image_tcb_only)
{
    TlsElf e;
    make_tls_image(e);
    e.tls.p_type = PT_NULL;

    Process *loader = scratch_loader();
    KTEST_EXPECT(loader != nullptr);
    if (!loader)
        return;

    KTEST_EXPECT(elf_load_user(reinterpret_cast<const uint8_t *>(&e), sizeof(e), loader) == k_load_vaddr);
    KTEST_EXPECT(elf_install_tls(loader, reinterpret_cast<const uint8_t *>(&e), sizeof(e)));

    const uint64_t fs = loader->fs_base;
    KTEST_EXPECT(fs != 0);

    const UniTcb *tcb = reinterpret_cast<const UniTcb *>(direct_read(loader, fs));
    KTEST_EXPECT(tcb != nullptr);
    if (tcb) {
        KTEST_EXPECT_EQ(tcb->self, fs);
        KTEST_EXPECT_EQ(tcb->tid, 0x1234ULL);
    }

    scratch_loader_free(loader);
}

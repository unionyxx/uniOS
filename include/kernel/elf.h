#pragma once
#include <stdint.h>

constexpr uint32_t ELF_MAGIC = 0x464C457F;
constexpr uint8_t ELFCLASS64 = 2;
constexpr uint8_t ELFDATA2LSB = 1;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN = 3;
constexpr uint16_t EM_X86_64 = 62;

constexpr uint32_t PT_NULL = 0;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_INTERP = 3;
constexpr uint32_t PT_TLS = 7;

constexpr uint32_t PF_X = 0x1;
constexpr uint32_t PF_W = 0x2;
constexpr uint32_t PF_R = 0x4;

struct Elf64_Ehdr
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
} __attribute__((packed));

struct Elf64_Phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

struct Process;
[[nodiscard]] bool elf_validate(const uint8_t *data, uint64_t size);
[[nodiscard]] uint64_t elf_load(const uint8_t *data, uint64_t size, Process *proc);
[[nodiscard]] uint64_t elf_load_user(const uint8_t *data, uint64_t size, Process *proc);

/* Walk a 64-bit little-endian ELF image's program headers for PT_TLS. On a
 * hit whose file bytes ([p_offset, p_offset + p_filesz)) stay inside the
 * image, return true with the segment's p_offset/p_memsz/p_align; the two
 * optional outs additionally yield p_vaddr (the template's landing address
 * inside a mapped PT_LOAD) and p_filesz (the file-carried prefix of the
 * block; [filesz, memsz) is the zero-filled .tbss tail). Return false —
 * with every output zeroed — when the image has no PT_TLS or is truncated
 * or malformed (same identity and phdr-table rules as elf_validate). */
[[nodiscard]] bool elf_tls_info(const uint8_t *image, uint64_t size, uint64_t *template_offset, uint64_t *memsz,
                                uint64_t *align, uint64_t *template_vaddr = nullptr, uint64_t *filesz = nullptr);

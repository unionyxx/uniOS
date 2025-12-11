#pragma once
#include <stdint.h>

// ELF Magic
#define ELF_MAGIC 0x464C457F // "\x7FELF"

// ELF Class
#define ELFCLASS64 2

// ELF Data encoding
#define ELFDATA2LSB 1 // Little endian

// ELF Type
#define ET_EXEC 2 // Executable
#define ET_DYN  3 // Shared object (PIE)

// ELF Machine
#define EM_X86_64 62

// Program header types
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

// Program header flags
#define PF_X 0x1 // Execute
#define PF_W 0x2 // Write
#define PF_R 0x4 // Read

// ELF64 Header
struct Elf64_Ehdr {
    uint8_t  e_ident[16];  // Magic number and other info
    uint16_t e_type;       // Object file type
    uint16_t e_machine;    // Architecture
    uint32_t e_version;    // Object file version
    uint64_t e_entry;      // Entry point virtual address
    uint64_t e_phoff;      // Program header table file offset
    uint64_t e_shoff;      // Section header table file offset
    uint32_t e_flags;      // Processor-specific flags
    uint16_t e_ehsize;     // ELF header size
    uint16_t e_phentsize;  // Program header table entry size
    uint16_t e_phnum;      // Program header table entry count
    uint16_t e_shentsize;  // Section header table entry size
    uint16_t e_shnum;      // Section header table entry count
    uint16_t e_shstrndx;   // Section header string table index
} __attribute__((packed));

// ELF64 Program Header
struct Elf64_Phdr {
    uint32_t p_type;    // Segment type
    uint32_t p_flags;   // Segment flags
    uint64_t p_offset;  // Segment file offset
    uint64_t p_vaddr;   // Segment virtual address
    uint64_t p_paddr;   // Segment physical address (unused)
    uint64_t p_filesz;  // Segment size in file
    uint64_t p_memsz;   // Segment size in memory
    uint64_t p_align;   // Segment alignment
} __attribute__((packed));

// ELF Loader functions
bool elf_validate(const uint8_t* data, uint64_t size);
uint64_t elf_load(const uint8_t* data, uint64_t size);
uint64_t elf_load_user(const uint8_t* data, uint64_t size);

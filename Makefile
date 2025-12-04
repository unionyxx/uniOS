# Makefile for uniOS

# Toolchain
CXX = g++
LD = ld

# Flags
CXXFLAGS = -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -mno-red-zone -Wall -Wextra -I.
LDFLAGS = -nostdlib -T kernel/linker.ld -z max-page-size=0x1000

# Files
KERNEL_SRC = kernel/kernel.cpp kernel/gdt.cpp kernel/idt.cpp kernel/pic.cpp kernel/keyboard.cpp
KERNEL_ASM = kernel/gdt_asm.asm kernel/interrupts.asm
KERNEL_OBJ = $(KERNEL_SRC:.cpp=.o) $(KERNEL_ASM:.asm=.o)
KERNEL_BIN = kernel.elf
ISO_IMAGE = uniOS.iso

.PHONY: all clean run

all: $(ISO_IMAGE)

$(KERNEL_BIN): $(KERNEL_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.asm
	nasm -f elf64 $< -o $@

$(ISO_IMAGE): $(KERNEL_BIN) limine.conf
	rm -rf iso_root
	mkdir -p iso_root
	cp $(KERNEL_BIN) iso_root/
	cp limine.conf iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	cp limine/limine.sys iso_root/ || true
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	./limine/limine bios-install $(ISO_IMAGE)

clean:
	rm -f $(KERNEL_OBJ) $(KERNEL_BIN) $(ISO_IMAGE)
	rm -rf iso_root

run: $(ISO_IMAGE)
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 512M

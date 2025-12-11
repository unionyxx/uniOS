# Makefile for uniOS

# Toolchain
CXX = g++
LD = ld

# Flags
# Directories
KERNEL_DIRS = kernel/core kernel/arch kernel/mem kernel/drivers kernel/drivers/net kernel/drivers/usb kernel/net kernel/fs kernel/shell

# Flags
CXXFLAGS = -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -Wall -Wextra -Wno-volatile -I. -Ikernel $(foreach dir,$(KERNEL_DIRS),-I$(dir))
LDFLAGS = -nostdlib -T kernel/linker.ld -z max-page-size=0x1000

# Files
KERNEL_SRC = $(foreach dir,$(KERNEL_DIRS),$(wildcard $(dir)/*.cpp))
KERNEL_ASM = $(foreach dir,$(KERNEL_DIRS),$(wildcard $(dir)/*.asm))
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
	# Generate uniFS image
	python3 tools/mkunifs.py rootfs unifs.img
	
	rm -rf iso_root
	mkdir -p iso_root
	cp $(KERNEL_BIN) iso_root/
	cp unifs.img iso_root/
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

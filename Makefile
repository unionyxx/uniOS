# Makefile for uniOS
# ==================
# Build: make [release|debug]  (default: release)
# Run:   make run [USB=1] [NET=1] [SOUND=1] [SERIAL=1] [GDB=1]

# Toolchain
CXX = g++
LD = ld
NASM = nasm
PYTHON = python3

# Directory structure
INCLUDE_DIR = include
SRC_DIR = src
BUILD_DIR = build
TOOLS_DIR = tools

# Source directories (find all recursively)
SRC_SUBDIRS = $(shell find $(SRC_DIR) -type d 2>/dev/null)

# Build configuration (default: release)
BUILD ?= release
GIT_COMMIT := $(shell git rev-parse --short HEAD)

# Include paths - new structure
INCLUDES = -I$(INCLUDE_DIR) -I$(SRC_DIR)

# Base flags (always applied)
CXXFLAGS_BASE = -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
                -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
                -march=x86-64 -mtune=generic \
                -ffunction-sections -fdata-sections \
                -Wall -Wextra -Wno-volatile \
                $(INCLUDES)
CXXFLAGS_BASE += -DGIT_COMMIT=\"$(GIT_COMMIT)\"

# Debug-specific flags
CXXFLAGS_DEBUG = $(CXXFLAGS_BASE) -DDEBUG -g -O0

# Release-specific flags (optimized, smaller binary)
CXXFLAGS_RELEASE = $(CXXFLAGS_BASE) -DNDEBUG -O2

# Select flags based on build type
ifeq ($(BUILD),debug)
    CXXFLAGS = $(CXXFLAGS_DEBUG)
else
    CXXFLAGS = $(CXXFLAGS_RELEASE)
endif

LDFLAGS_BASE = -nostdlib -T linker.ld -z max-page-size=0x1000 --gc-sections
LDFLAGS_DEBUG = $(LDFLAGS_BASE)
LDFLAGS_RELEASE = $(LDFLAGS_BASE)

ifeq ($(BUILD),debug)
    LDFLAGS = $(LDFLAGS_DEBUG)
else
    LDFLAGS = $(LDFLAGS_RELEASE)
endif

# Files - find all sources recursively
SRC_CPP = $(shell find $(SRC_DIR) -name '*.cpp' 2>/dev/null)
SRC_ASM = $(shell find $(SRC_DIR) -name '*.asm' 2>/dev/null)

# Object files - preserve directory structure in build dir
OBJ_CPP = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRC_CPP))
OBJ_ASM = $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/%.o, $(SRC_ASM))
OBJ_ALL = $(OBJ_CPP) $(OBJ_ASM)

KERNEL_BIN = $(BUILD_DIR)/kernel.elf
UNIFS_IMG = $(BUILD_DIR)/unifs.img
ISO_IMAGE = $(BUILD_DIR)/uniOS.iso
DISK_IMG = $(BUILD_DIR)/disk.img

# QEMU options
QEMU = qemu-system-x86_64
QEMU_BASE = -boot d -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMG),format=raw,if=ide,index=0 -m 512M

# Command line flags for 'make run'
QEMU_FLAGS = 
ifeq ($(USB),1)
    QEMU_FLAGS += -device qemu-xhci -device usb-kbd -device usb-mouse
endif
ifeq ($(NET),1)
    QEMU_FLAGS += -nic user,model=e1000
endif
ifeq ($(SOUND),1)
    QEMU_FLAGS += -device ac97
endif
ifeq ($(SERIAL),1)
    QEMU_FLAGS += -serial stdio
endif
ifeq ($(GDB),1)
    QEMU_FLAGS += -s -S
endif

# ==============================================================================
# Build Targets
# ==============================================================================

.PHONY: all release debug clean run run-net run-usb run-sound run-serial run-gdb help directories iso

all: release

release:
	@$(MAKE) BUILD=release iso

debug:
	@$(MAKE) BUILD=debug iso

iso: directories $(ISO_IMAGE)

# Create build directory structure mirroring src
directories:
	@mkdir -p $(BUILD_DIR)
	@for dir in $(SRC_SUBDIRS); do \
		mkdir -p $(BUILD_DIR)/$${dir#$(SRC_DIR)/}; \
	done

$(KERNEL_BIN): $(OBJ_ALL)
	@echo "[Link] $@"
	@$(LD) $(LDFLAGS) -o $@ $^

# Compile C++ files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "[CXX] $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile assembly files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@echo "[ASM] $<"
	@mkdir -p $(dir $@)
	@$(NASM) -f elf64 $< -o $@

$(UNIFS_IMG): $(TOOLS_DIR)/mkunifs.py
	@echo "[FS] Generating uniFS image..."
	@$(PYTHON) $(TOOLS_DIR)/mkunifs.py rootfs $@

$(ISO_IMAGE): $(KERNEL_BIN) $(UNIFS_IMG) limine.conf $(DISK_IMG)
	@$(PYTHON) $(TOOLS_DIR)/create_iso.py $(KERNEL_BIN) $(UNIFS_IMG) limine $@ $(BUILD_DIR)

$(DISK_IMG):
	@echo "[DISK] Creating 64MB FAT32 disk image..."
	@if [ "$(shell uname)" = "Darwin" ]; then \
		hdiutil create -size 64m -fs "MS-DOS FAT32" -volname "UNI_OS" -layout NONE $@.dmg >/dev/null; \
		mv $@.dmg $@; \
	else \
		dd if=/dev/zero of=$@ bs=1M count=64 status=none; \
		mkfs.fat -F 32 $@ >/dev/null; \
	fi

# ==============================================================================
# Run Targets
# ==============================================================================

run: iso
	@if [ "$(GDB)" = "1" ]; then echo "Waiting for GDB on localhost:1234..."; fi
	$(QEMU) $(QEMU_BASE) $(QEMU_FLAGS)

# Shortcuts
run-net:
	@$(MAKE) run NET=1
run-usb:
	@$(MAKE) run USB=1
run-sound:
	@$(MAKE) run SOUND=1
run-serial:
	@$(MAKE) run SERIAL=1
run-gdb:
	@$(MAKE) run GDB=1

# ==============================================================================
# Utility Targets
# ==============================================================================

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)

help:
	@echo "uniOS Build System"
	@echo "=================="
	@echo ""
	@echo "Build targets:"
	@echo "  make           - Build release version (default)"
	@echo "  make release   - Build optimized release version"
	@echo "  make debug     - Build debug version with DEBUG macro"
	@echo ""
	@echo "Run targets:"
	@echo "  make run       - Run in QEMU (Flags: USB=1 NET=1 SOUND=1 SERIAL=1 GDB=1)"
	@echo "  make run-net   - Shortcut for NET=1"
	@echo "  make run-usb   - Shortcut for USB=1"
	@echo "  make run-sound - Shortcut for SOUND=1"
	@echo "  make run-serial- Shortcut for SERIAL=1"
	@echo "  make run-gdb   - Shortcut for GDB=1"
	@echo ""
	@echo "Utility:"
	@echo "  make clean     - Remove build directory"
	@echo "  make help      - Show this help"

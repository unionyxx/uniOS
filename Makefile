# Makefile for uniOS
# ==================
# Build: make [release|debug]  (default: release)
# Run:   make run[-debug|-serial|-gdb]

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

# Include paths - new structure
INCLUDES = -I$(INCLUDE_DIR) -I$(SRC_DIR)

# Base flags (always applied)
CXXFLAGS_BASE = -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
                -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
                -march=x86-64 -mtune=generic \
                -ffunction-sections -fdata-sections \
                -Wall -Wextra -Wno-volatile \
                $(INCLUDES)

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

# QEMU options
QEMU = qemu-system-x86_64
QEMU_BASE = -cdrom $(ISO_IMAGE) -m 512M
QEMU_SOUND = -device ac97
QEMU_HDA = -device intel-hda -device hda-duplex
QEMU_NET = -nic user,model=e1000
QEMU_SERIAL = -serial stdio
QEMU_DEBUG = -s -S
QEMU_USB = -device qemu-xhci -device usb-kbd -device usb-mouse

# ==============================================================================
# Build Targets
# ==============================================================================

.PHONY: all release debug clean run run-net run-usb run-sound run-hda run-serial run-gdb help directories version-sync version-check

all: release

release:
	@$(MAKE) BUILD=release iso

debug:
	@$(MAKE) BUILD=debug iso

iso: directories version-sync $(ISO_IMAGE)

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

$(ISO_IMAGE): $(KERNEL_BIN) $(UNIFS_IMG) limine.conf
	@$(PYTHON) $(TOOLS_DIR)/create_iso.py $(KERNEL_BIN) $(UNIFS_IMG) limine $@ $(BUILD_DIR)

# ==============================================================================
# Run Targets
# ==============================================================================

run: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE)

run-net: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE) $(QEMU_NET)

run-usb: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE) $(QEMU_USB)

run-sound: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE) $(QEMU_SOUND)

run-hda: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE) $(QEMU_HDA)

run-serial: $(ISO_IMAGE)
	$(QEMU) $(QEMU_BASE) $(QEMU_SERIAL)

run-gdb: $(ISO_IMAGE)
	@echo "Waiting for GDB on localhost:1234..."
	$(QEMU) $(QEMU_BASE) $(QEMU_DEBUG)

# ==============================================================================
# Utility Targets
# ==============================================================================

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)

version-sync:
	@$(PYTHON) $(TOOLS_DIR)/sync_version.py

version-check:
	@$(PYTHON) $(TOOLS_DIR)/sync_version.py --check

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
	@echo "  make run       - Run in QEMU"
	@echo "  make run-net   - Run with e1000 network"
	@echo "  make run-usb   - Run with xHCI USB controller"
	@echo "  make run-sound - Run with AC'97 sound card"
	@echo "  make run-serial- Run with serial output to stdio"
	@echo "  make run-gdb   - Run with GDB stub (localhost:1234)"
	@echo ""
	@echo "Utility:"
	@echo "  make clean     - Remove build directory"
	@echo "  make version-sync  - Sync version to README/docs"
	@echo "  make version-check - Verify versions match"
	@echo "  make help      - Show this help"

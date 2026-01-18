#!/usr/bin/env python3
"""
Include Path Migration Script for uniOS BSD/Linux Hybrid Restructure

This script updates all #include directives to use the new paths.
"""

import os
import re
from pathlib import Path

# Mapping from old include names to new paths
INCLUDE_MAP = {
    # Boot
    '"limine.h"': '<boot/limine.h>',
    
    # Architecture (x86_64)
    '"gdt.h"': '<kernel/arch/x86_64/gdt.h>',
    '"idt.h"': '<kernel/arch/x86_64/idt.h>',
    '"io.h"': '<kernel/arch/x86_64/io.h>',
    '"pic.h"': '<kernel/arch/x86_64/pic.h>',
    '"pat.h"': '<kernel/arch/x86_64/pat.h>',
    '"serial.h"': '<kernel/arch/x86_64/serial.h>',
    
    # Kernel Core
    '"panic.h"': '<kernel/panic.h>',
    '"syscall.h"': '<kernel/syscall.h>',
    '"version.h"': '<kernel/version.h>',
    '"debug.h"': '<kernel/debug.h>',
    '"terminal.h"': '<kernel/terminal.h>',
    '"elf.h"': '<kernel/elf.h>',
    '"scheduler.h"': '<kernel/scheduler.h>',
    '"process.h"': '<kernel/process.h>',
    '"shell.h"': '<kernel/shell.h>',
    
    # Sync
    '"mutex.h"': '<kernel/sync/mutex.h>',
    '"spinlock.h"': '<kernel/sync/spinlock.h>',
    
    # Memory
    '"pmm.h"': '<kernel/mm/pmm.h>',
    '"vmm.h"': '<kernel/mm/vmm.h>',
    '"heap.h"': '<kernel/mm/heap.h>',
    '"bitmap.h"': '<kernel/mm/bitmap.h>',
    
    # Filesystem
    '"unifs.h"': '<kernel/fs/unifs.h>',
    '"pipe.h"': '<kernel/fs/pipe.h>',
    
    # Network
    '"net.h"': '<kernel/net/net.h>',
    '"ethernet.h"': '<kernel/net/ethernet.h>',
    '"arp.h"': '<kernel/net/arp.h>',
    '"ipv4.h"': '<kernel/net/ipv4.h>',
    '"icmp.h"': '<kernel/net/icmp.h>',
    '"udp.h"': '<kernel/net/udp.h>',
    '"tcp.h"': '<kernel/net/tcp.h>',
    '"dhcp.h"': '<kernel/net/dhcp.h>',
    '"dns.h"': '<kernel/net/dns.h>',
    
    # Time
    '"timer.h"': '<kernel/time/timer.h>',
    
    # libk
    '"kstring.h"': '<libk/kstring.h>',
    
    # Drivers - ACPI
    '"acpi.h"': '<drivers/acpi/acpi.h>',
    
    # Drivers - Bus
    '"pci.h"': '<drivers/bus/pci/pci.h>',
    '"usb.h"': '<drivers/bus/usb/usb.h>',
    '"xhci.h"': '<drivers/bus/usb/xhci/xhci.h>',
    
    # Drivers - Class (HID)
    '"input.h"': '<drivers/class/hid/input.h>',
    '"ps2_keyboard.h"': '<drivers/class/hid/ps2_keyboard.h>',
    '"ps2_mouse.h"': '<drivers/class/hid/ps2_mouse.h>',
    '"usb_hid.h"': '<drivers/class/hid/usb_hid.h>',
    
    # Drivers - Network
    '"e1000.h"': '<drivers/net/e1000/e1000.h>',
    '"rtl8139.h"': '<drivers/net/rtl8139/rtl8139.h>',
    
    # Drivers - RTC
    '"rtc.h"': '<drivers/rtc/rtc.h>',
    
    # Drivers - Sound
    '"ac97.h"': '<drivers/sound/ac97/ac97.h>',
    '"wav.h"': '<drivers/sound/wav.h>',
    
    # Drivers - Video
    '"graphics.h"': '<drivers/video/framebuffer.h>',
    '"font.h"': '<drivers/video/font.h>',
}

def update_includes(filepath):
    """Update include directives in a single file."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return False
    
    original = content
    
    # Replace each old include with new path
    for old, new in INCLUDE_MAP.items():
        # Match #include followed by the old path
        pattern = r'#include\s+' + re.escape(old)
        replacement = f'#include {new}'
        content = re.sub(pattern, replacement, content)
    
    if content != original:
        try:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"Updated: {filepath}")
            return True
        except Exception as e:
            print(f"Error writing {filepath}: {e}")
            return False
    return False

def main():
    # Script is in tools/, so project root is parent
    project_root = Path(__file__).parent.parent
    
    # Find all .cpp and .h files in include/ and src/
    directories = [project_root / 'include', project_root / 'src']
    
    updated_count = 0
    for directory in directories:
        if not directory.exists():
            print(f"Directory not found: {directory}")
            continue
            
        for filepath in directory.rglob('*'):
            if filepath.suffix in ['.cpp', '.h']:
                if update_includes(filepath):
                    updated_count += 1
    
    print(f"\nUpdated {updated_count} files")

if __name__ == '__main__':
    main()

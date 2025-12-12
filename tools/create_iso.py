import os
import shutil
import subprocess
import sys
from pathlib import Path

def run_command(cmd):
    try:
        subprocess.run(cmd, check=True, shell=True)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {cmd}")
        sys.exit(1)

def main():
    if len(sys.argv) < 5:
        print("Usage: create_iso.py <kernel_bin> <unifs_img> <limine_dir> <output_iso> <build_dir>")
        sys.exit(1)

    kernel_bin = Path(sys.argv[1]).resolve()
    unifs_img = Path(sys.argv[2]).resolve()
    limine_dir = Path(sys.argv[3]).resolve()
    output_iso = Path(sys.argv[4]).resolve()
    build_dir = Path(sys.argv[5]).resolve()

    iso_root = build_dir / "iso_root"

    # Clean and create iso_root
    if iso_root.exists():
        shutil.rmtree(iso_root)
    iso_root.mkdir(parents=True, exist_ok=True)

    print(f"[Tool] Generating ISO: {output_iso.name}")

    # Copy Kernel and Filesystem
    shutil.copy(kernel_bin, iso_root / "kernel.elf")
    shutil.copy(unifs_img, iso_root / "unifs.img")
    
    # Copy Limine Config
    shutil.copy("limine.conf", iso_root / "limine.conf")

    # Copy Limine Bootloader Files
    limine_files = [
        "limine-bios.sys",
        "limine-bios-cd.bin",
        "limine-uefi-cd.bin"
    ]
    
    for f in limine_files:
        src = limine_dir / f
        if src.exists():
            shutil.copy(src, iso_root / f)
        else:
            print(f"Warning: Limine file not found: {src}")

    # Optional: limine.sys for some setups
    limine_sys = limine_dir / "limine.sys"
    if limine_sys.exists():
        shutil.copy(limine_sys, iso_root / "limine.sys")

    # Create ISO with xorriso
    xorriso_cmd = (
        f"xorriso -as mkisofs -b limine-bios-cd.bin "
        f"-no-emul-boot -boot-load-size 4 -boot-info-table "
        f"--efi-boot limine-uefi-cd.bin "
        f"-efi-boot-part --efi-boot-image --protective-msdos-label "
        f"{iso_root} -o {output_iso}"
    )
    
    # Suppress xorriso output unless error
    run_command(f"{xorriso_cmd} 2>nul" if os.name == 'nt' else f"{xorriso_cmd} 2>/dev/null")

    # Install Limine BIOS bootloader
    limine_exe = limine_dir / "limine"
    # On Windows it might be limine.exe
    if not limine_exe.exists() and (limine_dir / "limine.exe").exists():
        limine_exe = limine_dir / "limine.exe"

    run_command(f"{limine_exe} bios-install {output_iso}")

    print(f"[Tool] Success! ISO created at: {output_iso}")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
from datetime import datetime, timezone
from pathlib import Path


SECTOR_SIZE = 2048
SYSTEM_AREA_SECTORS = 16
PVD_SECTOR = 16
BOOT_RECORD_SECTOR = 17
TERMINATOR_SECTOR = 18
L_PATH_TABLE_SECTOR = 19
M_PATH_TABLE_SECTOR = 20
BOOT_CATALOG_SECTOR = 21
ROOT_DIR_SECTOR = 22
EFI_DIR_SECTOR = 23
BOOT_DIR_SECTOR = 24
FIRST_FILE_SECTOR = 25
UEFI_PLATFORM_ID = 0xEF


def ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def pad_to_sector(data: bytes) -> bytes:
    padding = (-len(data)) % SECTOR_SIZE
    if padding == 0:
        return data
    return data + b"\x00" * padding


def both_endian_16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def both_endian_32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def iso_recording_time(timestamp: datetime) -> bytes:
    utc = timestamp.astimezone(timezone.utc)
    return bytes((
        utc.year - 1900,
        utc.month,
        utc.day,
        utc.hour,
        utc.minute,
        utc.second,
        0,
    ))


def iso_volume_time(timestamp: datetime) -> bytes:
    utc = timestamp.astimezone(timezone.utc)
    return utc.strftime("%Y%m%d%H%M%S00").encode("ascii") + b"\x00"


def text_field(text: str, size: int) -> bytes:
    encoded = text.encode("ascii")
    if len(encoded) > size:
        encoded = encoded[:size]
    return encoded.ljust(size, b" ")


def path_table_record(identifier: bytes, extent: int, parent_dir_number: int, little_endian: bool) -> bytes:
    if len(identifier) > 0xFF:
        raise ValueError("path table identifier is too long")
    record = bytearray()
    record.append(len(identifier))
    record.append(0)
    record.extend(struct.pack("<I" if little_endian else ">I", extent))
    record.extend(struct.pack("<H" if little_endian else ">H", parent_dir_number))
    record.extend(identifier)
    if len(identifier) & 1:
        record.append(0)
    return bytes(record)


def directory_record(extent: int, size: int, is_directory: bool, identifier: bytes, timestamp: datetime) -> bytes:
    if len(identifier) > 0xFF:
        raise ValueError("directory identifier is too long")
    padding = 1 if len(identifier) % 2 == 0 else 0
    record_length = 33 + len(identifier) + padding
    record = bytearray(record_length)
    record[0] = record_length
    record[1] = 0
    record[2:10] = both_endian_32(extent)
    record[10:18] = both_endian_32(size)
    record[18:25] = iso_recording_time(timestamp)
    record[25] = 0x02 if is_directory else 0x00
    record[26] = 0
    record[27] = 0
    record[28:32] = both_endian_16(1)
    record[32] = len(identifier)
    record[33:33 + len(identifier)] = identifier
    return bytes(record)


def validation_entry(timestamp: datetime) -> bytes:
    del timestamp
    entry = bytearray(32)
    entry[0] = 0x01
    entry[1] = UEFI_PLATFORM_ID
    entry[4:28] = text_field("uniOS UEFI", 24)
    entry[30] = 0x55
    entry[31] = 0xAA
    checksum = sum(struct.unpack("<16H", entry)) & 0xFFFF
    struct.pack_into("<H", entry, 28, (-checksum) & 0xFFFF)
    return bytes(entry)


def initial_default_entry(boot_image_sector: int, boot_image_bytes: int) -> bytes:
    entry = bytearray(32)
    entry[0] = 0x88
    entry[1] = 0x00
    sector_count = ceil_div(boot_image_bytes, 512)
    if sector_count > 0xFFFF:
        sector_count = 0
    struct.pack_into("<H", entry, 6, sector_count)
    struct.pack_into("<I", entry, 8, boot_image_sector)
    return bytes(entry)


def build_boot_catalog(boot_image_sector: int, boot_image_bytes: int, timestamp: datetime) -> bytes:
    catalog = validation_entry(timestamp) + initial_default_entry(boot_image_sector, boot_image_bytes)
    return pad_to_sector(catalog)


def pvd_record(volume_space_size: int, path_table_size: int, root_dir_record: bytes, timestamp: datetime) -> bytes:
    pvd = bytearray(SECTOR_SIZE)
    pvd[0] = 0x01
    pvd[1:6] = b"CD001"
    pvd[6] = 0x01
    pvd[8:40] = text_field("EFI", 32)
    pvd[40:72] = text_field("UNIOS", 32)
    pvd[80:88] = both_endian_32(volume_space_size)
    pvd[120:124] = both_endian_16(1)
    pvd[124:128] = both_endian_16(1)
    pvd[128:132] = both_endian_16(SECTOR_SIZE)
    pvd[132:140] = both_endian_32(path_table_size)
    struct.pack_into("<I", pvd, 140, L_PATH_TABLE_SECTOR)
    struct.pack_into("<I", pvd, 144, 0)
    struct.pack_into(">I", pvd, 148, M_PATH_TABLE_SECTOR)
    struct.pack_into(">I", pvd, 152, 0)
    pvd[156:190] = root_dir_record
    pvd[190:318] = text_field("", 128)
    pvd[318:446] = text_field("uniOS", 128)
    pvd[446:574] = text_field("uniOS", 128)
    pvd[574:702] = text_field("uniOS Native UEFI ISO", 128)
    pvd[702:739] = text_field("", 37)
    pvd[739:776] = text_field("", 37)
    pvd[776:813] = text_field("", 37)
    timestamp_field = iso_volume_time(timestamp)
    pvd[813:830] = timestamp_field
    pvd[830:847] = timestamp_field
    pvd[847:864] = b"0" * 16 + b"\x00"
    pvd[864:881] = timestamp_field
    pvd[881] = 0x01
    return bytes(pvd)


def boot_record(boot_catalog_sector: int) -> bytes:
    record = bytearray(SECTOR_SIZE)
    record[0] = 0x00
    record[1:6] = b"CD001"
    record[6] = 0x01
    record[7:39] = text_field("EL TORITO SPECIFICATION", 32)
    struct.pack_into("<I", record, 71, boot_catalog_sector)
    return bytes(record)


def volume_terminator() -> bytes:
    record = bytearray(SECTOR_SIZE)
    record[0] = 0xFF
    record[1:6] = b"CD001"
    record[6] = 0x01
    return bytes(record)


def build_iso(efi_image: Path, bootloader: Path, kernel: Path, unifs: Path, output: Path) -> None:
    timestamp = datetime.now(timezone.utc)
    visible_bootloader = bootloader.read_bytes()
    kernel_bytes = kernel.read_bytes()
    unifs_bytes = unifs.read_bytes()
    boot_image = efi_image.read_bytes()

    bootloader_extent = FIRST_FILE_SECTOR
    kernel_extent = bootloader_extent + ceil_div(len(visible_bootloader), SECTOR_SIZE)
    unifs_extent = kernel_extent + ceil_div(len(kernel_bytes), SECTOR_SIZE)
    boot_image_extent = unifs_extent + ceil_div(len(unifs_bytes), SECTOR_SIZE)
    volume_space_size = boot_image_extent + ceil_div(len(boot_image), SECTOR_SIZE)

    root_dir_record = directory_record(ROOT_DIR_SECTOR, SECTOR_SIZE, True, b"\x00", timestamp)
    efi_dir_record = directory_record(EFI_DIR_SECTOR, SECTOR_SIZE, True, b"EFI", timestamp)
    boot_dir_record = directory_record(BOOT_DIR_SECTOR, SECTOR_SIZE, True, b"BOOT", timestamp)
    kernel_record = directory_record(kernel_extent, len(kernel_bytes), False, b"KERNEL.ELF;1", timestamp)
    unifs_record = directory_record(unifs_extent, len(unifs_bytes), False, b"UNIFS.IMG;1", timestamp)
    bootloader_record = directory_record(bootloader_extent, len(visible_bootloader), False, b"BOOTX64.EFI;1",
                                         timestamp)

    root_directory = pad_to_sector(
        b"".join((
            root_dir_record,
            directory_record(ROOT_DIR_SECTOR, SECTOR_SIZE, True, b"\x01", timestamp),
            efi_dir_record,
            kernel_record,
            unifs_record,
        )))

    efi_directory = pad_to_sector(
        b"".join((
            directory_record(EFI_DIR_SECTOR, SECTOR_SIZE, True, b"\x00", timestamp),
            directory_record(ROOT_DIR_SECTOR, SECTOR_SIZE, True, b"\x01", timestamp),
            boot_dir_record,
        )))

    boot_directory = pad_to_sector(
        b"".join((
            directory_record(BOOT_DIR_SECTOR, SECTOR_SIZE, True, b"\x00", timestamp),
            directory_record(EFI_DIR_SECTOR, SECTOR_SIZE, True, b"\x01", timestamp),
            bootloader_record,
        )))

    little_path_table = pad_to_sector(
        b"".join((
            path_table_record(b"\x00", ROOT_DIR_SECTOR, 1, True),
            path_table_record(b"EFI", EFI_DIR_SECTOR, 1, True),
            path_table_record(b"BOOT", BOOT_DIR_SECTOR, 2, True),
        )))
    big_path_table = pad_to_sector(
        b"".join((
            path_table_record(b"\x00", ROOT_DIR_SECTOR, 1, False),
            path_table_record(b"EFI", EFI_DIR_SECTOR, 1, False),
            path_table_record(b"BOOT", BOOT_DIR_SECTOR, 2, False),
        )))

    image = b"".join((
        b"\x00" * (SYSTEM_AREA_SECTORS * SECTOR_SIZE),
        pvd_record(volume_space_size, 34, root_dir_record, timestamp),
        boot_record(BOOT_CATALOG_SECTOR),
        volume_terminator(),
        little_path_table,
        big_path_table,
        build_boot_catalog(boot_image_extent, len(boot_image), timestamp),
        root_directory,
        efi_directory,
        boot_directory,
        pad_to_sector(visible_bootloader),
        pad_to_sector(kernel_bytes),
        pad_to_sector(unifs_bytes),
        pad_to_sector(boot_image),
    ))

    if len(image) != volume_space_size * SECTOR_SIZE:
        raise ValueError("internal ISO layout error: computed size does not match generated image")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"[Tool] UEFI ISO created at {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a UEFI bootable ISO9660 image for uniOS.")
    parser.add_argument("--efi-image", required=True, help="Path to the El Torito EFI system partition image.")
    parser.add_argument("--bootloader", required=True, help="Path to BOOTX64.EFI to expose in the ISO filesystem.")
    parser.add_argument("--kernel", required=True, help="Path to kernel.elf to expose in the ISO filesystem.")
    parser.add_argument("--unifs", required=True, help="Path to unifs.img to expose in the ISO filesystem.")
    parser.add_argument("output", help="Output ISO path.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_iso(Path(args.efi_image).resolve(), Path(args.bootloader).resolve(), Path(args.kernel).resolve(),
              Path(args.unifs).resolve(), Path(args.output).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

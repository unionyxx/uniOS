#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path


BYTES_PER_SECTOR = 512
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_DIR_ENTRIES = 512
ROOT_DIR_SECTORS = (ROOT_DIR_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
MEDIA_DESCRIPTOR = 0xF8
DEFAULT_HEADROOM = 4 * 1024 * 1024
MIN_IMAGE_SIZE = 64 * 1024 * 1024
DEFAULT_DATA_IMAGE_SIZE = 128 * 1024 * 1024
MIN_FAT16_CLUSTERS = 4085
MAX_FAT16_CLUSTERS = 65524
MIN_FAT32_CLUSTERS = 65525
DISK_PARTITION_LBA = 2048
DISK_ALIGNMENT_SECTORS = 2048
MBR_PARTITION_TYPE_FAT32_LBA = 0x0C
FAT32_RESERVED_SECTORS = 32
FAT32_FSINFO_SECTOR = 1
FAT32_BACKUP_BOOT_SECTOR = 6
FAT32_ROOT_CLUSTER = 2
DATA_VOLUME_LABEL = "UNI_DATA"


def _le16(value: int) -> bytes:
    return struct.pack("<H", value)


def _le32(value: int) -> bytes:
    return struct.pack("<I", value)


def align_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def fat_name(name: str) -> bytes:
    upper = name.upper()
    parts = upper.split(".")
    if len(parts) > 2:
        raise ValueError(f"unsupported FAT 8.3 name: {name}")
    base = parts[0]
    ext = parts[1] if len(parts) == 2 else ""
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$%'-@~`!(){}^#&")
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError(f"unsupported FAT 8.3 name: {name}")
    for ch in base + ext:
        if ch not in allowed:
            raise ValueError(f"unsupported FAT 8.3 name character in {name!r}")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def required_image_size(files: list[bytes]) -> int:
    total_file_bytes = sum(len(data) for data in files)
    return max(MIN_IMAGE_SIZE, align_up(total_file_bytes + DEFAULT_HEADROOM, 1024 * 1024))


def compute_sectors_per_fat(total_sectors: int, sectors_per_cluster: int) -> tuple[int, int]:
    sectors_per_fat = 1
    while True:
        data_sectors = total_sectors - RESERVED_SECTORS - FAT_COUNT * sectors_per_fat - ROOT_DIR_SECTORS
        cluster_count = data_sectors // sectors_per_cluster
        required = math.ceil(((cluster_count + 2) * 2) / BYTES_PER_SECTOR)
        if required <= sectors_per_fat:
            return sectors_per_fat, cluster_count
        sectors_per_fat = required


def choose_layout(total_sectors: int) -> tuple[int, int, int]:
    for sectors_per_cluster in (1, 2, 4, 8, 16, 32, 64):
        sectors_per_fat, cluster_count = compute_sectors_per_fat(total_sectors, sectors_per_cluster)
        if MIN_FAT16_CLUSTERS <= cluster_count <= MAX_FAT16_CLUSTERS:
            return sectors_per_cluster, sectors_per_fat, cluster_count
    raise ValueError("unable to find a valid FAT16 layout for the requested image size")


def make_boot_sector(total_sectors: int, sectors_per_cluster: int, sectors_per_fat: int, label: str,
                     hidden_sectors: int) -> bytes:
    boot = bytearray(BYTES_PER_SECTOR)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"uniOSEFI"
    boot[11:13] = _le16(BYTES_PER_SECTOR)
    boot[13] = sectors_per_cluster
    boot[14:16] = _le16(RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    boot[17:19] = _le16(ROOT_DIR_ENTRIES)
    if total_sectors < 0x10000:
        boot[19:21] = _le16(total_sectors)
    else:
        boot[19:21] = _le16(0)
    boot[21] = MEDIA_DESCRIPTOR
    boot[22:24] = _le16(sectors_per_fat)
    boot[24:26] = _le16(63)
    boot[26:28] = _le16(255)
    boot[28:32] = _le32(hidden_sectors)
    if total_sectors >= 0x10000:
        boot[32:36] = _le32(total_sectors)
    boot[36] = 0x80
    boot[38] = 0x29
    boot[39:43] = _le32(0x554E694F)
    boot[43:54] = label.encode("ascii")[:11].ljust(11, b" ")
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    return bytes(boot)


def set_fat_entry(raw_fat: bytearray, cluster: int, value: int) -> None:
    raw_fat[cluster * 2:(cluster + 1) * 2] = _le16(value & 0xFFFF)


def make_dir_entry(name11: bytes, attr: int, first_cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name11
    entry[11] = attr
    entry[20:22] = _le16((first_cluster >> 16) & 0xFFFF)
    entry[26:28] = _le16(first_cluster & 0xFFFF)
    entry[28:32] = _le32(size)
    return bytes(entry)


@dataclass
class FileNode:
    name: str
    data: bytes
    cluster: int = 0


@dataclass
class DirNode:
    name: str
    parent: "DirNode | None" = None
    children: dict[str, "DirNode"] = field(default_factory=dict)
    files: dict[str, FileNode] = field(default_factory=dict)
    cluster: int = 0


class Fat16Builder:
    def __init__(self, total_bytes: int, label: str):
        total_sectors = total_bytes // BYTES_PER_SECTOR
        self.total_bytes = total_bytes
        self.total_sectors = total_sectors
        self.sectors_per_cluster, self.sectors_per_fat, self.cluster_count = choose_layout(total_sectors)
        self.cluster_size = self.sectors_per_cluster * BYTES_PER_SECTOR
        self.first_root_dir_sector = RESERVED_SECTORS + FAT_COUNT * self.sectors_per_fat
        self.first_data_sector = self.first_root_dir_sector + ROOT_DIR_SECTORS
        self.image = bytearray(total_bytes)
        self.fat = bytearray(self.sectors_per_fat * BYTES_PER_SECTOR)
        self.root = DirNode(name="")
        self.next_free_cluster = 2
        self.label = label

        set_fat_entry(self.fat, 0, 0xFFF8 | MEDIA_DESCRIPTOR)
        set_fat_entry(self.fat, 1, 0xFFFF)

    def root_dir_offset(self) -> int:
        return self.first_root_dir_sector * BYTES_PER_SECTOR

    def cluster_offset(self, cluster: int) -> int:
        sector = self.first_data_sector + (cluster - 2) * self.sectors_per_cluster
        return sector * BYTES_PER_SECTOR

    def allocate_chain(self, size_bytes: int) -> int:
        clusters = max(1, align_up(size_bytes, self.cluster_size) // self.cluster_size)
        first = self.next_free_cluster
        last = first + clusters - 1
        if last >= self.cluster_count + 2:
            raise ValueError("FAT16 image ran out of clusters")
        for cluster in range(first, last):
            set_fat_entry(self.fat, cluster, cluster + 1)
        set_fat_entry(self.fat, last, 0xFFFF)
        self.next_free_cluster = last + 1
        return first

    def write_chain(self, first_cluster: int, data: bytes) -> None:
        cluster = first_cluster
        data_offset = 0
        data_size = len(data)
        while True:
            chunk_end = min(data_offset + self.cluster_size, data_size)
            chunk_size = chunk_end - data_offset
            offset = self.cluster_offset(cluster)
            self.image[offset:offset + chunk_size] = data[data_offset:chunk_end]
            if chunk_size < self.cluster_size:
                self.image[offset + chunk_size:offset + self.cluster_size] = b"\x00" * (self.cluster_size - chunk_size)
            data_offset = chunk_end
            next_value = struct.unpack_from("<H", self.fat, cluster * 2)[0]
            if next_value >= 0xFFF8:
                break
            cluster = next_value

    def ensure_dir(self, path_components: list[str]) -> DirNode:
        node = self.root
        for component in path_components:
            child = node.children.get(component)
            if child is None:
                child = DirNode(name=component, parent=node)
                node.children[component] = child
            node = child
        return node

    def add_file(self, path_components: list[str], data: bytes) -> None:
        directory = self.ensure_dir(path_components[:-1])
        directory.files[path_components[-1]] = FileNode(name=path_components[-1], data=data)

    def allocate_directory_clusters(self, node: DirNode) -> None:
        if node is not self.root:
            node.cluster = self.allocate_chain(self.cluster_size)
        for child in node.children.values():
            self.allocate_directory_clusters(child)

    def populate_file_data(self, node: DirNode) -> None:
        for file_node in node.files.values():
            file_node.cluster = self.allocate_chain(len(file_node.data))
            self.write_chain(file_node.cluster, file_node.data)
        for child in node.children.values():
            self.populate_file_data(child)

    def write_root_directory(self) -> None:
        entries = bytearray()
        for name, child in sorted(self.root.children.items()):
            entries.extend(make_dir_entry(fat_name(name), 0x10, child.cluster, 0))
        for name, file_node in sorted(self.root.files.items()):
            entries.extend(make_dir_entry(fat_name(name), 0x20, file_node.cluster, len(file_node.data)))
        entries.extend(b"\x00" * 32)
        if len(entries) > ROOT_DIR_ENTRIES * 32:
            raise ValueError("root directory exceeds fixed FAT16 root directory size")
        offset = self.root_dir_offset()
        self.image[offset:offset + len(entries)] = entries

    def write_directory(self, node: DirNode) -> None:
        if node is self.root:
            self.write_root_directory()
        else:
            entries = bytearray()
            parent_cluster = node.parent.cluster if node.parent and node.parent is not self.root else 0
            entries.extend(make_dir_entry(b".          ", 0x10, node.cluster, 0))
            entries.extend(make_dir_entry(b"..         ", 0x10, parent_cluster, 0))
            for name, child in sorted(node.children.items()):
                entries.extend(make_dir_entry(fat_name(name), 0x10, child.cluster, 0))
            for name, file_node in sorted(node.files.items()):
                entries.extend(make_dir_entry(fat_name(name), 0x20, file_node.cluster, len(file_node.data)))
            entries.extend(b"\x00" * 32)
            if len(entries) > self.cluster_size:
                raise ValueError(f"directory {node.name or '/'} exceeds one cluster; multi-cluster dirs not implemented")
            self.write_chain(node.cluster, bytes(entries))

        for child in node.children.values():
            self.write_directory(child)

    def finalise(self, hidden_sectors: int) -> bytes:
        self.allocate_directory_clusters(self.root)
        self.populate_file_data(self.root)
        self.write_directory(self.root)

        boot_sector = make_boot_sector(self.total_sectors, self.sectors_per_cluster, self.sectors_per_fat, self.label,
                                       hidden_sectors)
        self.image[0:BYTES_PER_SECTOR] = boot_sector

        first_fat_offset = RESERVED_SECTORS * BYTES_PER_SECTOR
        self.image[first_fat_offset:first_fat_offset + len(self.fat)] = self.fat
        second_fat_offset = first_fat_offset + len(self.fat)
        self.image[second_fat_offset:second_fat_offset + len(self.fat)] = self.fat
        return bytes(self.image)


def compute_fat32_sectors_per_fat(total_sectors: int, sectors_per_cluster: int) -> tuple[int, int]:
    sectors_per_fat = 1
    while True:
        data_sectors = total_sectors - FAT32_RESERVED_SECTORS - FAT_COUNT * sectors_per_fat
        if data_sectors <= 0:
            raise ValueError("image is too small for FAT32")
        cluster_count = data_sectors // sectors_per_cluster
        required = math.ceil(((cluster_count + 2) * 4) / BYTES_PER_SECTOR)
        if required <= sectors_per_fat:
            return sectors_per_fat, cluster_count
        sectors_per_fat = required


def choose_fat32_layout(total_sectors: int) -> tuple[int, int, int]:
    for sectors_per_cluster in (1, 2, 4, 8, 16, 32, 64):
        sectors_per_fat, cluster_count = compute_fat32_sectors_per_fat(total_sectors, sectors_per_cluster)
        if cluster_count >= MIN_FAT32_CLUSTERS:
            return sectors_per_cluster, sectors_per_fat, cluster_count
    raise ValueError("unable to find a valid FAT32 layout for the requested image size")


def make_fat32_boot_sector(total_sectors: int, sectors_per_cluster: int, sectors_per_fat: int, label: str,
                           hidden_sectors: int) -> bytes:
    boot = bytearray(BYTES_PER_SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"uniOSEFI"
    boot[11:13] = _le16(BYTES_PER_SECTOR)
    boot[13] = sectors_per_cluster
    boot[14:16] = _le16(FAT32_RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    boot[17:19] = _le16(0)
    boot[19:21] = _le16(0)
    boot[21] = MEDIA_DESCRIPTOR
    boot[22:24] = _le16(0)
    boot[24:26] = _le16(63)
    boot[26:28] = _le16(255)
    boot[28:32] = _le32(hidden_sectors)
    boot[32:36] = _le32(total_sectors)
    boot[36:40] = _le32(sectors_per_fat)
    boot[40:42] = _le16(0)
    boot[42:44] = _le16(0)
    boot[44:48] = _le32(FAT32_ROOT_CLUSTER)
    boot[48:50] = _le16(FAT32_FSINFO_SECTOR)
    boot[50:52] = _le16(FAT32_BACKUP_BOOT_SECTOR)
    boot[64] = 0x80
    boot[66] = 0x29
    boot[67:71] = _le32(0x554E694F)
    boot[71:82] = label.encode("ascii")[:11].ljust(11, b" ")
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"
    return bytes(boot)


def make_fat32_fsinfo(next_free_cluster: int, free_clusters: int) -> bytes:
    fsinfo = bytearray(BYTES_PER_SECTOR)
    fsinfo[0:4] = _le32(0x41615252)
    fsinfo[484:488] = _le32(0x61417272)
    fsinfo[488:492] = _le32(free_clusters)
    fsinfo[492:496] = _le32(next_free_cluster)
    fsinfo[508:512] = _le32(0xAA550000)
    return bytes(fsinfo)


def set_fat32_entry(raw_fat: bytearray, cluster: int, value: int) -> None:
    raw_fat[cluster * 4:(cluster + 1) * 4] = _le32(value & 0x0FFFFFFF)


class Fat32Builder:
    def __init__(self, total_bytes: int, label: str):
        total_sectors = total_bytes // BYTES_PER_SECTOR
        self.total_bytes = total_bytes
        self.total_sectors = total_sectors
        self.sectors_per_cluster, self.sectors_per_fat, self.cluster_count = choose_fat32_layout(total_sectors)
        self.cluster_size = self.sectors_per_cluster * BYTES_PER_SECTOR
        self.first_data_sector = FAT32_RESERVED_SECTORS + FAT_COUNT * self.sectors_per_fat
        self.image = bytearray(total_bytes)
        self.fat = bytearray(self.sectors_per_fat * BYTES_PER_SECTOR)
        self.root = DirNode(name="")
        self.root.cluster = FAT32_ROOT_CLUSTER
        self.next_free_cluster = FAT32_ROOT_CLUSTER + 1
        self.label = label

        set_fat32_entry(self.fat, 0, 0x0FFFFFF8)
        set_fat32_entry(self.fat, 1, 0x0FFFFFFF)
        set_fat32_entry(self.fat, FAT32_ROOT_CLUSTER, 0x0FFFFFFF)

    def cluster_offset(self, cluster: int) -> int:
        sector = self.first_data_sector + (cluster - 2) * self.sectors_per_cluster
        return sector * BYTES_PER_SECTOR

    def allocate_chain(self, size_bytes: int) -> int:
        clusters = max(1, align_up(size_bytes, self.cluster_size) // self.cluster_size)
        first = self.next_free_cluster
        last = first + clusters - 1
        if last >= self.cluster_count + 2:
            raise ValueError("FAT32 image ran out of clusters")
        for cluster in range(first, last):
            set_fat32_entry(self.fat, cluster, cluster + 1)
        set_fat32_entry(self.fat, last, 0x0FFFFFFF)
        self.next_free_cluster = last + 1
        return first

    def write_chain(self, first_cluster: int, data: bytes) -> None:
        cluster = first_cluster
        data_offset = 0
        data_size = len(data)
        while True:
            chunk_end = min(data_offset + self.cluster_size, data_size)
            chunk_size = chunk_end - data_offset
            offset = self.cluster_offset(cluster)
            self.image[offset:offset + chunk_size] = data[data_offset:chunk_end]
            if chunk_size < self.cluster_size:
                self.image[offset + chunk_size:offset + self.cluster_size] = b"\x00" * (self.cluster_size - chunk_size)
            data_offset = chunk_end
            next_value = struct.unpack_from("<I", self.fat, cluster * 4)[0] & 0x0FFFFFFF
            if next_value >= 0x0FFFFFF8:
                break
            cluster = next_value

    def ensure_dir(self, path_components: list[str]) -> DirNode:
        node = self.root
        for component in path_components:
            child = node.children.get(component)
            if child is None:
                child = DirNode(name=component, parent=node)
                node.children[component] = child
            node = child
        return node

    def add_file(self, path_components: list[str], data: bytes) -> None:
        directory = self.ensure_dir(path_components[:-1])
        directory.files[path_components[-1]] = FileNode(name=path_components[-1], data=data)

    def allocate_directory_clusters(self, node: DirNode) -> None:
        if node is not self.root:
            node.cluster = self.allocate_chain(self.cluster_size)
        for child in node.children.values():
            self.allocate_directory_clusters(child)

    def populate_file_data(self, node: DirNode) -> None:
        for file_node in node.files.values():
            file_node.cluster = self.allocate_chain(len(file_node.data))
            self.write_chain(file_node.cluster, file_node.data)
        for child in node.children.values():
            self.populate_file_data(child)

    def write_directory(self, node: DirNode) -> None:
        entries = bytearray()
        if node is not self.root:
            parent_cluster = node.parent.cluster if node.parent else FAT32_ROOT_CLUSTER
            entries.extend(make_dir_entry(b".          ", 0x10, node.cluster, 0))
            entries.extend(make_dir_entry(b"..         ", 0x10, parent_cluster, 0))
        for name, child in sorted(node.children.items()):
            entries.extend(make_dir_entry(fat_name(name), 0x10, child.cluster, 0))
        for name, file_node in sorted(node.files.items()):
            entries.extend(make_dir_entry(fat_name(name), 0x20, file_node.cluster, len(file_node.data)))
        entries.extend(b"\x00" * 32)
        if len(entries) > self.cluster_size:
            raise ValueError(f"directory {node.name or '/'} exceeds one cluster; multi-cluster dirs not implemented")
        self.write_chain(node.cluster, bytes(entries))

        for child in node.children.values():
            self.write_directory(child)

    def finalise(self, hidden_sectors: int) -> bytes:
        self.allocate_directory_clusters(self.root)
        self.populate_file_data(self.root)
        self.write_directory(self.root)

        boot_sector = make_fat32_boot_sector(self.total_sectors, self.sectors_per_cluster, self.sectors_per_fat,
                                             self.label, hidden_sectors)
        free_clusters = self.cluster_count - (self.next_free_cluster - 2)
        fsinfo_sector = make_fat32_fsinfo(self.next_free_cluster, free_clusters)
        self.image[0:BYTES_PER_SECTOR] = boot_sector
        self.image[FAT32_FSINFO_SECTOR * BYTES_PER_SECTOR:(FAT32_FSINFO_SECTOR + 1) * BYTES_PER_SECTOR] = fsinfo_sector
        backup = FAT32_BACKUP_BOOT_SECTOR * BYTES_PER_SECTOR
        self.image[backup:backup + BYTES_PER_SECTOR] = boot_sector
        self.image[backup + BYTES_PER_SECTOR:backup + 2 * BYTES_PER_SECTOR] = fsinfo_sector

        first_fat_offset = FAT32_RESERVED_SECTORS * BYTES_PER_SECTOR
        self.image[first_fat_offset:first_fat_offset + len(self.fat)] = self.fat
        second_fat_offset = first_fat_offset + len(self.fat)
        self.image[second_fat_offset:second_fat_offset + len(self.fat)] = self.fat
        return bytes(self.image)


def patch_hidden_sectors(volume: bytearray, hidden_sectors: int) -> None:
    volume[28:32] = _le32(hidden_sectors)


def mbr_partition_entry(start_lba: int, sector_count: int, partition_type: int, bootable: bool = False) -> bytes:
    entry = bytearray(16)
    entry[0] = 0x80 if bootable else 0x00
    entry[1:4] = b"\xFF\xFF\xFF"
    entry[4] = partition_type
    entry[5:8] = b"\xFF\xFF\xFF"
    entry[8:12] = _le32(start_lba)
    entry[12:16] = _le32(sector_count)
    return bytes(entry)


def fat32_volume_label(volume: bytes) -> str:
    if len(volume) < BYTES_PER_SECTOR or volume[510:512] != b"\x55\xAA":
        return ""
    try:
        return volume[71:82].decode("ascii", errors="ignore").strip()
    except UnicodeDecodeError:
        return ""


def read_existing_partition_by_label(image_path: Path, label: str, sector_count: int) -> bytes | None:
    if not image_path.exists():
        return None

    try:
        with image_path.open("rb") as existing:
            mbr = existing.read(BYTES_PER_SECTOR)
            if len(mbr) != BYTES_PER_SECTOR or mbr[510:512] != b"\x55\xAA":
                return None

            for index in range(4):
                offset = 446 + index * 16
                part_type = mbr[offset + 4]
                start_lba = struct.unpack_from("<I", mbr, offset + 8)[0]
                part_sectors = struct.unpack_from("<I", mbr, offset + 12)[0]
                if part_type == 0 or start_lba == 0 or part_sectors == 0:
                    continue

                existing.seek(start_lba * BYTES_PER_SECTOR)
                boot_sector = existing.read(BYTES_PER_SECTOR)
                if fat32_volume_label(boot_sector) != label:
                    continue
                if part_sectors != sector_count:
                    print(
                        f"[Tool] Existing {label} partition has {part_sectors} sectors; "
                        f"expected {sector_count}, reinitializing"
                    )
                    return None
                existing.seek(start_lba * BYTES_PER_SECTOR)
                data = existing.read(part_sectors * BYTES_PER_SECTOR)
                if len(data) == part_sectors * BYTES_PER_SECTOR:
                    return data
    except OSError:
        return None

    return None


def create_fat_image(output: Path, bootloader: Path, kernel: Path, unifs: Path, label: str) -> None:
    files = {
        ("EFI", "BOOT", "BOOTX64.EFI"): bootloader.read_bytes(),
        ("KERNEL.ELF",): kernel.read_bytes(),
        ("UNIFS.IMG",): unifs.read_bytes(),
    }
    builder = Fat32Builder(required_image_size(list(files.values())), label)
    for components, data in files.items():
        builder.add_file(list(components), data)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(builder.finalise(hidden_sectors=0))
    print(f"[Tool] EFI FAT image created at {output}")


def create_blank_data_volume(size_bytes: int, label: str, hidden_sectors: int) -> bytes:
    size_bytes = align_up(max(size_bytes, MIN_IMAGE_SIZE), BYTES_PER_SECTOR)
    builder = Fat32Builder(size_bytes, label)
    return builder.finalise(hidden_sectors=hidden_sectors)


def create_disk_image(output: Path, fat_image_path: Path, data_size_bytes: int, include_data_partition: bool) -> None:
    volume = bytearray(fat_image_path.read_bytes())
    patch_hidden_sectors(volume, DISK_PARTITION_LBA)

    esp_partition_sectors = len(volume) // BYTES_PER_SECTOR
    total_sectors = DISK_PARTITION_LBA + esp_partition_sectors
    data_volume = b""
    data_start_lba = 0
    data_partition_sectors = 0

    if include_data_partition:
        data_size_bytes = align_up(max(data_size_bytes, MIN_IMAGE_SIZE), BYTES_PER_SECTOR)
        data_start_lba = align_up(total_sectors, DISK_ALIGNMENT_SECTORS)
        data_partition_sectors = data_size_bytes // BYTES_PER_SECTOR
        existing_data = read_existing_partition_by_label(output, DATA_VOLUME_LABEL, data_partition_sectors)
        if existing_data is not None:
            data_volume = existing_data
            print(f"[Tool] Preserved existing {DATA_VOLUME_LABEL} partition from {output}")
        else:
            data_volume = create_blank_data_volume(data_size_bytes, DATA_VOLUME_LABEL, data_start_lba)
        total_sectors = data_start_lba + data_partition_sectors

    disk = bytearray(total_sectors * BYTES_PER_SECTOR)

    disk[446:462] = mbr_partition_entry(DISK_PARTITION_LBA, esp_partition_sectors, MBR_PARTITION_TYPE_FAT32_LBA, True)
    if include_data_partition:
        disk[462:478] = mbr_partition_entry(data_start_lba, data_partition_sectors, MBR_PARTITION_TYPE_FAT32_LBA)
    disk[510:512] = b"\x55\xAA"
    esp_start = DISK_PARTITION_LBA * BYTES_PER_SECTOR
    disk[esp_start:esp_start + len(volume)] = volume
    if include_data_partition:
        data_start = data_start_lba * BYTES_PER_SECTOR
        disk[data_start:data_start + len(data_volume)] = data_volume

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(disk)
    if include_data_partition:
        print(
            f"[Tool] EFI disk image created at {output} "
            f"(ESP + {data_partition_sectors * BYTES_PER_SECTOR // (1024 * 1024)} MiB {DATA_VOLUME_LABEL})"
        )
    else:
        print(f"[Tool] EFI disk image created at {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build uniOS EFI FAT or raw-disk images.")
    parser.add_argument("--mode", choices=("fat", "disk"), required=True)
    parser.add_argument("--bootloader")
    parser.add_argument("--kernel")
    parser.add_argument("--unifs")
    parser.add_argument("--efi-image")
    parser.add_argument("--label", default="UNI_OS")
    parser.add_argument("--data-size-mib", type=int, default=DEFAULT_DATA_IMAGE_SIZE // (1024 * 1024))
    parser.add_argument("--no-data-partition", action="store_true")
    parser.add_argument("output")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = Path(args.output).resolve()
    if args.mode == "fat":
        if not args.bootloader or not args.kernel or not args.unifs:
            raise ValueError("fat mode requires --bootloader, --kernel, and --unifs")
        label = args.label.upper()
        if not label or any(ord(ch) > 0x7F for ch in label):
            raise ValueError("label must be non-empty ASCII")
        create_fat_image(output, Path(args.bootloader).resolve(), Path(args.kernel).resolve(),
                         Path(args.unifs).resolve(), label)
        return 0

    if not args.efi_image:
        raise ValueError("disk mode requires --efi-image")
    if args.data_size_mib < 0:
        raise ValueError("--data-size-mib must not be negative")
    create_disk_image(output, Path(args.efi_image).resolve(), args.data_size_mib * 1024 * 1024,
                      not args.no_data_partition)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

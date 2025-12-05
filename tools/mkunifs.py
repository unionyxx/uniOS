import os
import struct
import sys

def create_unifs(source_dir, output_file):
    files = []
    for root, _, filenames in os.walk(source_dir):
        for filename in filenames:
            filepath = os.path.join(root, filename)
            files.append((filename, filepath))

    # Header: Magic (8 bytes), File Count (8 bytes)
    magic = b"UNIFS v1"
    file_count = len(files)
    
    header = struct.pack("<8sQ", magic, file_count)
    
    # Calculate offsets
    # Header size: 16 bytes
    # Entry size: 64 (name) + 8 (offset) + 8 (size) = 80 bytes
    current_offset = 16 + (file_count * 80)
    
    entries = []
    data_blob = bytearray()
    
    for name, filepath in files:
        with open(filepath, "rb") as f:
            content = f.read()
            
        size = len(content)
        
        # Entry: Name (64s), Offset (Q), Size (Q)
        # Pad name to 64 bytes
        name_bytes = name.encode('utf-8')
        if len(name_bytes) > 63:
            print(f"Warning: Filename {name} truncated")
            name_bytes = name_bytes[:63]
            
        entry = struct.pack("<64sQQ", name_bytes, current_offset, size)
        entries.append(entry)
        
        data_blob.extend(content)
        current_offset += size

    with open(output_file, "wb") as f:
        f.write(header)
        for entry in entries:
            f.write(entry)
        f.write(data_blob)
        
    print(f"Created {output_file} with {file_count} files.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: mkunifs.py <source_dir> <output_file>")
        sys.exit(1)
        
    create_unifs(sys.argv[1], sys.argv[2])

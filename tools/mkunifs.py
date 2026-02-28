import os
import struct
import sys

def create_unifs(source_dir, output_file):
    entries_to_add = [] # (name, is_dir, source_path)
    
    for root, dirs, filenames in os.walk(source_dir):
        # Add directories
        for d in dirs:
            dirpath = os.path.join(root, d)
            rel_path = os.path.relpath(dirpath, source_dir)
            entries_to_add.append((rel_path + "/", True, None))
            
        # Add files
        for filename in filenames:
            filepath = os.path.join(root, filename)
            rel_path = os.path.relpath(filepath, source_dir)
            entries_to_add.append((rel_path, False, filepath))

    # Header: Magic (8 bytes), File Count (8 bytes)
    magic = b"UNIFS v1"
    file_count = len(entries_to_add)
    
    header = struct.pack("<8sQ", magic, file_count)
    
    # Header size: 16 bytes
    # Entry size: 80 bytes
    current_offset = 16 + (file_count * 80)
    
    entries = []
    data_blob = bytearray()
    
    for name, is_dir, filepath in entries_to_add:
        content = b""
        if not is_dir and filepath:
            with open(filepath, "rb") as f:
                content = f.read()
            
        size = len(content)
        
        # Entry: Name (64s), Offset (Q), Size (Q)
        name_bytes = name.encode('utf-8')
        if len(name_bytes) > 63:
            print(f"Warning: Name {name} truncated")
            name_bytes = name_bytes[:63]
            
        entry = struct.pack("<64sQQ", name_bytes, current_offset, size)
        entries.append(entry)
        
        if not is_dir:
            data_blob.extend(content)
            current_offset += size
        else:
            # Directory uses 0 offset or just points to nothing
            pass

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

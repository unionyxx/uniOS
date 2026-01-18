#!/usr/bin/env python3
"""
sync_version.py - Synchronize version across all uniOS files

Reads version from kernel/core/version.h (the single source of truth)
and updates README.md and docs/index.html to match.

Usage: python3 tools/sync_version.py [--check]
  --check: Only verify versions match, don't modify files (exit 1 if mismatch)
"""

import re
import sys
from pathlib import Path

# Paths relative to project root
VERSION_H = Path("include/kernel/version.h")
README_MD = Path("README.md")
DOCS_HTML = Path("docs/index.html")

def get_version_from_header():
    """Extract version string from version.h"""
    content = VERSION_H.read_text()
    
    major = re.search(r'#define UNIOS_VERSION_MAJOR\s+(\d+)', content)
    minor = re.search(r'#define UNIOS_VERSION_MINOR\s+(\d+)', content)
    patch = re.search(r'#define UNIOS_VERSION_PATCH\s+(\d+)', content)
    
    if not all([major, minor, patch]):
        raise RuntimeError("Could not parse version from version.h")
    
    return f"{major.group(1)}.{minor.group(1)}.{patch.group(1)}"

def update_readme(version):
    """Update version in README.md"""
    content = README_MD.read_text()
    # Match "Current Version: **vX.Y.Z**"
    new_content = re.sub(
        r'Current Version: \*\*v[\d.]+\*\*',
        f'Current Version: **v{version}**',
        content
    )
    if content != new_content:
        README_MD.write_text(new_content)
        return True
    return False

def update_docs_html(version):
    """Update version in docs/index.html"""
    content = DOCS_HTML.read_text()
    
    # JSON-LD softwareVersion
    new_content = re.sub(
        r'"softwareVersion":\s*"[\d.]+"',
        f'"softwareVersion": "{version}"',
        content
    )
    
    # Badge: <span class="badge">vX.Y.Z</span>
    new_content = re.sub(
        r'<span class="badge">v[\d.]+</span>',
        f'<span class="badge">v{version}</span>',
        new_content
    )
    
    # Typewriter: "Booting uniOS vX.Y.Z..."
    new_content = re.sub(
        r'Booting uniOS v[\d.]+\.\.\.',
        f'Booting uniOS v{version}...',
        new_content
    )
    
    if content != new_content:
        DOCS_HTML.write_text(new_content)
        return True
    return False

def check_versions(version):
    """Check if all files have the correct version"""
    errors = []
    
    # Check README.md
    readme = README_MD.read_text()
    if f'Current Version: **v{version}**' not in readme:
        match = re.search(r'Current Version: \*\*v([\d.]+)\*\*', readme)
        found = match.group(1) if match else "???"
        errors.append(f"README.md: expected {version}, found {found}")
    
    # Check docs/index.html
    html = DOCS_HTML.read_text()
    if f'"softwareVersion": "{version}"' not in html:
        match = re.search(r'"softwareVersion":\s*"([\d.]+)"', html)
        found = match.group(1) if match else "???"
        errors.append(f"docs/index.html (JSON-LD): expected {version}, found {found}")
    
    if f'<span class="badge">v{version}</span>' not in html:
        match = re.search(r'<span class="badge">v([\d.]+)</span>', html)
        found = match.group(1) if match else "???"
        errors.append(f"docs/index.html (badge): expected {version}, found {found}")
    
    if f'Booting uniOS v{version}...' not in html:
        match = re.search(r'Booting uniOS v([\d.]+)\.\.\.', html)
        found = match.group(1) if match else "???"
        errors.append(f"docs/index.html (typewriter): expected {version}, found {found}")
    
    return errors

def main():
    check_only = "--check" in sys.argv
    
    try:
        version = get_version_from_header()
    except Exception as e:
        print(f"Error: {e}")
        return 1
    
    print(f"Version from version.h: {version}")
    
    if check_only:
        errors = check_versions(version)
        if errors:
            print("\nVersion mismatches found:")
            for e in errors:
                print(f"  - {e}")
            return 1
        print("All versions match!")
        return 0
    
    # Update files
    readme_updated = update_readme(version)
    html_updated = update_docs_html(version)
    
    if readme_updated:
        print(f"Updated: README.md -> v{version}")
    else:
        print(f"Already up to date: README.md")
    
    if html_updated:
        print(f"Updated: docs/index.html -> v{version}")
    else:
        print(f"Already up to date: docs/index.html")
    
    print("\nDone! Version synchronized across all files.")
    return 0

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Extract an Xbox 360 XDVDFS (GDFX) game ISO into the assets directory,
equivalent to `extract-xiso -d assets <iso>`.

Scans the repo root for the first *.iso file it finds and extracts its
XDVDFS filesystem tree into assets/, matching the layout the SDK's codegen
and other scripts (extract_dlc.py, extract_tu.py) expect (default.xex,
Content/, System/, etc. at the top level).

The XDVDFS parsing (magic-offset probing + binary-tree directory table
layout) mirrors xenia-canary's DiscImageDevice - see
xenia/vfs/devices/disc_image_device.cc.

Usage:
    python scripts/extract_game.py [ISO_PATH] [--out DIR]
"""
import argparse
import glob
import os
import struct
import sys

SECTOR_SIZE = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Sector-32 offsets to try for the game partition, in order (see Xenia).
LIKELY_OFFSETS = (0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000)

FILE_ATTRIBUTE_DIRECTORY = 0x10


def find_first_iso(root):
    for path in sorted(glob.glob(os.path.join(root, "*.iso"))):
        if os.path.isfile(path):
            return path
    return None


def verify(data):
    """Locate the game partition and its root directory-table sector/size."""
    for game_offset in LIKELY_OFFSETS:
        magic_off = game_offset + 32 * SECTOR_SIZE
        if data[magic_off:magic_off + len(MAGIC)] == MAGIC:
            break
    else:
        raise ValueError("not an XDVDFS image: magic not found at any known offset")

    fs_off = game_offset + 32 * SECTOR_SIZE
    (root_sector,) = struct.unpack_from("<I", data, fs_off + 20)
    (root_size,) = struct.unpack_from("<I", data, fs_off + 24)
    if root_size < 13 or root_size > 32 * 1024 * 1024:
        raise ValueError(f"damaged root directory table (size={root_size})")

    root_offset = game_offset + root_sector * SECTOR_SIZE
    return game_offset, root_offset, root_size


def read_entry(data, game_offset, buffer_offset, ordinal, out_dir, count):
    """Recursively walk one binary-search-tree directory table, writing files
    under out_dir. Returns the number of files written."""
    p = buffer_offset + ordinal * 4
    node_l, node_r = struct.unpack_from("<HH", data, p)
    (sector,) = struct.unpack_from("<I", data, p + 4)
    (length,) = struct.unpack_from("<I", data, p + 8)
    attributes = data[p + 12]
    name_length = data[p + 13]
    name = data[p + 14:p + 14 + name_length].decode("cp1252")

    if node_l:
        count = read_entry(data, game_offset, buffer_offset, node_l, out_dir, count)

    entry_path = os.path.join(out_dir, name)
    if attributes & FILE_ATTRIBUTE_DIRECTORY:
        os.makedirs(entry_path, exist_ok=True)
        if length:
            child_buffer_offset = game_offset + sector * SECTOR_SIZE
            count = read_entry(data, game_offset, child_buffer_offset, 0, entry_path, count)
    else:
        file_offset = game_offset + sector * SECTOR_SIZE
        with open(entry_path, "wb") as f:
            f.write(data[file_offset:file_offset + length])
        count += 1

    if node_r:
        count = read_entry(data, game_offset, buffer_offset, node_r, out_dir, count)

    return count


def extract(iso_path, out_dir):
    with open(iso_path, "rb") as f:
        data = f.read()

    game_offset, root_offset, root_size = verify(data)
    os.makedirs(out_dir, exist_ok=True)
    return read_entry(data, game_offset, root_offset, 0, out_dir, 0)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))

    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("iso", nargs="?", help="Path to the game ISO (default: first *.iso found in the repo root)")
    p.add_argument("--out", default=os.path.join(root, "assets"), help="Output directory (default: assets/)")
    args = p.parse_args()

    iso_path = args.iso or find_first_iso(root)
    if not iso_path:
        print(f"error: no *.iso found in {root}", file=sys.stderr)
        sys.exit(1)

    print(f"extracting {iso_path} -> {args.out}")
    count = extract(iso_path, args.out)
    print(f"extracted {count} file(s)")


if __name__ == "__main__":
    main()

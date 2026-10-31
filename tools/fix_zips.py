#!/usr/bin/env python3
"""Repack ROM zips so retro-go can read them.

retro-go's rg_storage_unzip_file() reads the ZIP *local* header and takes
both sizes from it:

    output_buffer_size = header.uncompressed_size;
    stream_remaining   = header.compressed_size;

Archives written with a data descriptor -- the streaming style most tools
produce, and what MiSTer's ROM packs use -- carry `flags & 0x08` and leave
those two fields ZERO in the local header, putting the real values in the
central directory at the end of the file. retro-go therefore allocates a
zero-byte buffer and stops with "failed to open the zip" on every ROM,
regardless of path or filesystem.

This script rewrites each single-entry archive with the sizes in the local
header, which is what `zipfile.writestr(bytes)` produces. It also verifies
the result.

    python3 fix_zips.py IN_DIR OUT_DIR
    python3 fix_zips.py IN_DIR OUT_DIR --layout roms

The input tree is walked recursively; each zip keeps its relative path
under OUT_DIR. With --layout roms, files land in OUT_DIR/roms/<system>/,
which is the shape retro-go's launcher scans
(RG_BASE_PATH_ROMS = "/fs/roms/<short_name>").
"""
import argparse
import os
import shutil
import struct
import sys
import zipfile

DATA_DESCRIPTOR = 0x08


def local_header_flags_and_size(path):
    """Return (gp_flags, uncompressed_size) from the local file header."""
    with open(path, "rb") as f:
        hdr = f.read(30)
    if len(hdr) < 30 or hdr[:4] != b"PK\x03\x04":
        return None, None
    flags = struct.unpack("<H", hdr[6:8])[0]
    usize = struct.unpack("<I", hdr[22:26])[0]
    return flags, usize


def repack(src, dst):
    """Rewrite one archive with sizes in the local header."""
    with zipfile.ZipFile(src) as z:
        entries = [i for i in z.infolist() if not i.is_dir()]
        if len(entries) != 1:
            return f"skipped: {len(entries)} entries (retro-go takes the first match only)"
        entry = entries[0]
        data = z.read(entry.filename)
        name = os.path.basename(entry.filename)
        crc = entry.CRC

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        # writestr with bytes writes the sizes into the local header.
        z.writestr(info, data)

    flags, usize = local_header_flags_and_size(dst)
    if flags is None:
        return "FAILED: output is not a zip"
    if flags & DATA_DESCRIPTOR:
        return "FAILED: output still has a data descriptor"
    if usize == 0:
        return "FAILED: output has a zero local size"
    with zipfile.ZipFile(dst) as z:
        if z.infolist()[0].CRC != crc:
            return "FAILED: CRC changed"
    return "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("in_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--layout", choices=["flat", "roms"], default="flat",
                    help="roms: place each system under OUT/roms/<system>/")
    ap.add_argument("--system-map", default="NES=nes,Gameboy=gb,GBC=gbc",
                    help="comma-separated IN_DIR_NAME=short_name pairs for --layout roms")
    ap.add_argument("--force", action="store_true", help="overwrite OUT_DIR")
    args = ap.parse_args()

    if args.force and os.path.isdir(args.out_dir):
        shutil.rmtree(args.out_dir)

    smap = dict(p.split("=", 1) for p in args.system_map.split(",") if p)

    counts = {"ok": 0, "skipped": 0, "FAILED": 0}
    total_in = total_out = 0
    for root, _dirs, files in os.walk(args.in_dir):
        for f in sorted(files):
            if not f.lower().endswith(".zip"):
                continue
            src = os.path.join(root, f)
            rel = os.path.relpath(src, args.in_dir)
            if args.layout == "roms":
                top = rel.split(os.sep)[0]
                short = smap.get(top)
                if short is None:
                    print(f"  ?? {rel}: no system mapping for '{top}'")
                    counts["skipped"] += 1
                    continue
                dst = os.path.join(args.out_dir, "roms", short, f)
            else:
                dst = os.path.join(args.out_dir, rel)
            result = repack(src, dst)
            key = result if result in counts else ("ok" if result.startswith("ok") else "FAILED")
            counts[key if key in counts else "FAILED"] += 1
            if not result.startswith("ok"):
                print(f"  {result}: {rel}")
            total_in += os.path.getsize(src)
            if os.path.exists(dst):
                total_out += os.path.getsize(dst)

    print(f"\nok={counts['ok']} skipped={counts['skipped']} failed={counts['FAILED']}")
    print(f"in {total_in / 1048576:.2f} MB -> out {total_out / 1048576:.2f} MB")
    return 1 if counts["FAILED"] else 0


if __name__ == "__main__":
    sys.exit(main())

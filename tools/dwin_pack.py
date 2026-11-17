#!/usr/bin/env python3
"""Pack DGUS files into a single .dwpk bundle for the DWIN SDK display OTA.

Bundle layout, every field little-endian:

    Header, 32 bytes
        0   4   magic "DWPK"
        4   1   format version (1)
        5   1   flags (0)
        6   2   number of files
        8   4   total bundle size
        12  16  version label, ASCII, zero padded
        28  4   CRC32 of the header (without this field) followed by every entry

    One entry per file, 16 bytes
        0   2   DGUS file id (23.icl -> 23)
        2   2   reserved (0)
        4   4   offset of the file data in the bundle
        8   4   size of the file data
        12  4   CRC32 of the file data

    File data, in the order of the entries.

Usage:
    dwin_pack.py pack HMI_OTA_NITRO --version 1.4.0 --last 22 -o hmi.dwpk
    dwin_pack.py list hmi.dwpk
"""

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"DWPK"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sBBHI16sI")
ENTRY = struct.Struct("<HHIII")
VERSION_SIZE = 16

SLOT_SIZE = 256 * 1024  # one DGUS file slot
BLOCK_SIZE = 32 * 1024  # one NOR flash write operation
BLOCKS_PER_SLOT = SLOT_SIZE // BLOCK_SIZE
MAX_FILES_DEFAULT = 16  # CONFIG_DWIN_OTA_BUNDLE_MAX_FILES default on the device


def fail(message):
    sys.exit(f"error: {message}")


def blocks_of(file_id, size):
    """32KB flash blocks a file occupies, as a half-open range."""
    first = file_id * BLOCKS_PER_SLOT
    return first, first + -(-size // BLOCK_SIZE)


def collect(sources):
    """Map DGUS file id -> path, taking the id from the leading digits of each name."""
    paths = []
    for source in sources:
        source = Path(source)
        if source.is_dir():
            paths += sorted(p for p in source.iterdir() if p.is_file() and not p.name.startswith("."))
        elif source.is_file():
            paths.append(source)
        else:
            fail(f"{source} does not exist")

    files = {}
    for path in paths:
        match = re.match(r"\d+", path.name)
        if match is None:
            print(f"warning: skipping {path.name}, the name does not start with a DGUS file number", file=sys.stderr)
            continue
        file_id = int(match.group())
        if file_id in files:
            fail(f"{files[file_id].name} and {path.name} both target file slot {file_id}")
        files[file_id] = path
    return files


def pack(args):
    if re.fullmatch(r"\d+(\.\d+)*", args.version) is None:
        fail(f'version "{args.version}" is not dotted numeric, e.g. 1.4.0')
    if len(args.version) >= VERSION_SIZE:
        fail(f"version is longer than {VERSION_SIZE - 1} characters")

    files = collect(args.sources)
    if not files:
        fail("no DGUS files found")
    if len(files) > args.max_files:
        fail(f"{len(files)} files, the device accepts {args.max_files} (CONFIG_DWIN_OTA_BUNDLE_MAX_FILES)")
    if args.last is not None and args.last not in files:
        fail(f"--last {args.last}: no file with that number")

    data = {file_id: path.read_bytes() for file_id, path in files.items()}

    flash_blocks = args.flash_size * 1024 * 1024 // BLOCK_SIZE
    used = []
    for file_id in sorted(files):
        if not data[file_id]:
            fail(f"{files[file_id].name} is empty")
        first, end = blocks_of(file_id, len(data[file_id]))
        if end > flash_blocks:
            fail(f"{files[file_id].name} does not fit in a {args.flash_size}MB NOR flash")
        if used and first < used[-1][1]:
            fail(f"{used[-1][0].name} is larger than its slots and runs into {files[file_id].name}")
        used.append((files[file_id], end))

    # Largest first, the file named by --last at the very end: it is the one that carries the
    # version, so an update that stops half way leaves the old version on the panel.
    order = sorted(files, key=lambda file_id: (file_id == args.last, -len(data[file_id]), file_id))

    offset = HEADER.size + ENTRY.size * len(order)
    entries = b""
    for file_id in order:
        entries += ENTRY.pack(file_id, 0, offset, len(data[file_id]), zlib.crc32(data[file_id]))
        offset += len(data[file_id])

    header = HEADER.pack(MAGIC, FORMAT_VERSION, 0, len(order), offset, args.version.encode("ascii"), 0)
    table_crc = zlib.crc32(header[:-4] + entries)
    bundle = header[:-4] + struct.pack("<I", table_crc) + entries + b"".join(data[file_id] for file_id in order)

    output = Path(args.output) if args.output else Path(Path(args.sources[0]).resolve().name + ".dwpk")
    output.write_bytes(bundle)

    print(f"{output}: version {args.version}, {len(order)} files, {len(bundle)} bytes")
    describe(bundle, {file_id: files[file_id].name for file_id in order})


def parse(bundle):
    """Return (version, total size, entries) and raise ValueError on a malformed table."""
    if len(bundle) < HEADER.size:
        raise ValueError("shorter than a bundle header")
    magic, fmt, _flags, count, total, version, crc = HEADER.unpack_from(bundle)
    if magic != MAGIC:
        raise ValueError("not a .dwpk bundle")
    if fmt != FORMAT_VERSION:
        raise ValueError(f"unsupported format version {fmt}")
    table_end = HEADER.size + ENTRY.size * count
    if len(bundle) < table_end:
        raise ValueError("truncated file table")
    if zlib.crc32(bundle[: HEADER.size - 4] + bundle[HEADER.size : table_end]) != crc:
        raise ValueError("file table CRC mismatch")
    entries = [ENTRY.unpack_from(bundle, HEADER.size + ENTRY.size * i) for i in range(count)]
    return version.rstrip(b"\0").decode("ascii", "replace"), total, entries


def describe(bundle, names=None):
    """Print the file table and return True when every file CRC matches."""
    _version, _total, entries = parse(bundle)
    ok = True
    blocks = 0
    print(f"{'id':>5} {'offset':>9} {'size':>9} {'blocks':>6}  {'crc32':8}  status")
    for file_id, _reserved, offset, size, crc in entries:
        payload = bundle[offset : offset + size]
        good = len(payload) == size and zlib.crc32(payload) == crc
        ok &= good
        first, end = blocks_of(file_id, size)
        blocks += end - first
        name = f"  {names[file_id]}" if names else ""
        print(f"{file_id:>5} {offset:>9} {size:>9} {end - first:>6}  {crc:08x}  {'ok' if good else 'CORRUPT'}{name}")
    print(f"{blocks} flash blocks, about {blocks * 6} s on the panel")
    return ok


def list_bundle(args):
    bundle = Path(args.bundle).read_bytes()
    try:
        version, total, entries = parse(bundle)
    except ValueError as error:
        fail(f"{args.bundle}: {error}")
    print(f"{args.bundle}: version {version}, {len(entries)} files, {len(bundle)} bytes")
    ok = describe(bundle)
    if total != len(bundle):
        fail(f"the header announces {total} bytes, the file has {len(bundle)}")
    if not ok:
        fail("at least one file is corrupt")


def main():
    parser = argparse.ArgumentParser(description="Pack DGUS files into a .dwpk bundle for the DWIN SDK display OTA.")
    commands = parser.add_subparsers(dest="command", required=True)

    pack_parser = commands.add_parser("pack", help="create a bundle from a directory or a list of DGUS files")
    pack_parser.add_argument("sources", nargs="+", help="directory with the DGUS files, or the files themselves")
    pack_parser.add_argument("--version", required=True, help="version label of the bundle, dotted numeric (1.4.0)")
    pack_parser.add_argument("-o", "--output", help="bundle to write, defaults to <first source>.dwpk")
    pack_parser.add_argument("--last", type=int, metavar="ID", help="DGUS file number to write last (the one carrying the version, usually 22)")
    pack_parser.add_argument("--flash-size", type=int, choices=(8, 16), default=16, help="display NOR flash size in MB (default 16)")
    pack_parser.add_argument("--max-files", type=int, default=MAX_FILES_DEFAULT, help=f"files the device accepts in one bundle (default {MAX_FILES_DEFAULT})")
    pack_parser.set_defaults(run=pack)

    list_parser = commands.add_parser("list", help="show and verify the content of a bundle")
    list_parser.add_argument("bundle")
    list_parser.set_defaults(run=list_bundle)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()

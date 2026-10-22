"""Extract an Xbox 360 XDVDFS (XGD2/XGD3) disc image into a directory."""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048
# Game partition offsets for plain XDVDFS, XGD2, XGD1-style and XGD3 images.
PARTITIONS = (0x0, 0xFD90000, 0x18300000, 0x2080000)
ATTR_DIRECTORY = 0x10


def find_partition(stream) -> int:
    for base in PARTITIONS:
        stream.seek(base + 32 * SECTOR)
        if stream.read(len(MAGIC)) == MAGIC:
            return base
    raise RuntimeError("No XDVDFS volume descriptor found")


def walk(stream, base: int, sector: int, size: int, prefix: Path):
    if size == 0:
        return
    stream.seek(base + sector * SECTOR)
    table = stream.read(size)
    pending = [0]
    seen = set()
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > len(table):
            continue
        seen.add(offset)
        left, right, start, length, attr, name_len = struct.unpack_from("<HHIIBB", table, offset)
        if left == 0xFFFF:
            continue
        name = table[offset + 14: offset + 14 + name_len].decode("ascii", "replace")
        if left:
            pending.append(left * 4)
        if right:
            pending.append(right * 4)
        path = prefix / name
        if attr & ATTR_DIRECTORY:
            yield path, None, None
            yield from walk(stream, base, start, length, path)
        else:
            yield path, start, length


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--list", action="store_true", help="List files without extracting")
    args = parser.parse_args()
    with args.iso.open("rb") as stream:
        base = find_partition(stream)
        stream.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", stream.read(8))
        total = 0
        for path, start, length in walk(stream, base, root_sector, root_size, Path()):
            if start is None:
                if not args.list:
                    (args.output / path).mkdir(parents=True, exist_ok=True)
                continue
            total += length
            if args.list:
                print(f"{length:12d}  {path.as_posix()}")
                continue
            target = args.output / path
            target.parent.mkdir(parents=True, exist_ok=True)
            stream.seek(base + start * SECTOR)
            with target.open("wb") as out:
                remaining = length
                while remaining:
                    chunk = stream.read(min(remaining, 1 << 24))
                    if not chunk:
                        raise RuntimeError(f"Truncated image while reading {path}")
                    out.write(chunk)
                    remaining -= len(chunk)
        print(f"{'Listed' if args.list else 'Extracted'} {total / (1 << 20):.1f} MiB "
              f"(partition offset {base:#x})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

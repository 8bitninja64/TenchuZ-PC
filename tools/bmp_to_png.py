"""Convert 32-bit BMP screenshots (TENCHU_SCREENSHOTS) to PNG, without dependencies."""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import zlib


def convert(source: Path, target: Path, scale: int) -> None:
    data = source.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    if bits != 32:
        raise ValueError(f"{source}: only 32-bit BMP files are supported")
    rows = []
    for y in range(abs(height)):
        row_index = abs(height) - 1 - y if height > 0 else y
        start = offset + row_index * width * 4
        pixels = data[start:start + width * 4]
        row = bytearray()
        for x in range(0, width, scale):
            b, g, r = pixels[x * 4], pixels[x * 4 + 1], pixels[x * 4 + 2]
            row += bytes((r, g, b))
        if y % scale == 0:
            rows.append(b"\0" + bytes(row))
    out_width, out_height = len(rows[0]) // 3, len(rows)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", out_width, out_height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    target.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--scale", type=int, default=2, help="keep every Nth pixel (default 2)")
    args = parser.parse_args()
    for source in args.files:
        convert(source, source.with_suffix(".png"), args.scale)
    return 0


if __name__ == "__main__":
    sys.exit(main())

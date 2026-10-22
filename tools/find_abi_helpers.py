"""Locate the compiler's register save/restore helpers in the decoded XEX image.

XenonRecomp needs their addresses (config/tenchuz.toml) to translate the
prologue/epilogue calls. Each helper is found by its first two instructions.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys

from xex_image import read_image

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x82000000
SIGNATURES = {
    "savegprlr_14": (0xF9C1FF68, 0xF9E1FF70),  # std r14,-0x98(r1); std r15,-0x90(r1)
    "restgprlr_14": (0xE9C1FF68, 0xE9E1FF70),  # ld r14,-0x98(r1); ld r15,-0x90(r1)
    "savefpr_14": (0xD9CCFF70, 0xD9ECFF78),    # stfd f14,-0x90(r12); stfd f15,-0x88(r12)
    "restfpr_14": (0xC9CCFF70, 0xC9ECFF78),    # lfd f14,-0x90(r12); lfd f15,-0x88(r12)
    "savevmx_14": (0x3960FEE0, 0x7DCB61CE),    # li r11,-0x120; stvx v14,r11,r12
    "restvmx_14": (0x3960FEE0, 0x7DCB60CE),    # li r11,-0x120; lvx v14,r11,r12
    "savevmx_64": (0x3960FC00, 0x100B61CB),    # li r11,-0x400; stvx128 v64,r11,r12
    "restvmx_64": (0x3960FC00, 0x100B60CB),    # li r11,-0x400; lvx128 v64,r11,r12
}


def code_sections(image: bytes):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    count, optional = struct.unpack_from("<H", image, pe + 6)[0], struct.unpack_from("<H", image, pe + 20)[0]
    for i in range(count):
        offset = pe + 24 + optional + i * 40
        size, address = struct.unpack_from("<II", image, offset + 8)
        if struct.unpack_from("<I", image, offset + 36)[0] & 0x20:
            yield address, size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path, nargs="?", default=ROOT / "game/default.xex")
    args = parser.parse_args()
    image = read_image(args.xex)
    status = 0
    for name, words in SIGNATURES.items():
        pattern = b"".join(struct.pack(">I", word) for word in words)
        hits = []
        for address, size in code_sections(image):
            text = image[address:address + size]
            index = text.find(pattern)
            while index != -1:
                if index % 4 == 0:
                    hits.append(BASE + address + index)
                index = text.find(pattern, index + 4)
        if len(hits) == 1:
            print(f"{name}_address = 0x{hits[0]:08X}")
        else:
            print(f"# {name}: {len(hits)} candidates {[hex(h) for h in hits]}", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())

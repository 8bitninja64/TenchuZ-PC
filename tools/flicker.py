"""Find flicker in a burst of consecutive screenshots (control file 'burst').

For each frame, compares 32x18 block luminance with the previous and next
frame. A block that changes sharply and changes back on the next frame (A-B-A)
is flicker; steady motion changes it in one direction. Prints the worst frames
and writes a contact sheet (PNG) of the worst triple.
Usage: python tools/flicker.py <burst dir> [--top N]
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import zlib

GRID_X, GRID_Y = 32, 18


def load(path: Path):
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    return data, offset, width, abs(height), height > 0


def blocks(path: Path):
    data, offset, width, height, bottom_up = load(path)
    bw, bh = width // GRID_X, height // GRID_Y
    sums = [[0.0] * GRID_X for _ in range(GRID_Y)]
    step = 4  # sample every 4th pixel in each direction
    for gy in range(GRID_Y):
        for gx in range(GRID_X):
            total = count = 0
            for y in range(gy * bh, (gy + 1) * bh, step):
                row = (height - 1 - y) if bottom_up else y
                base = offset + row * width * 4
                for x in range(gx * bw, (gx + 1) * bw, step):
                    i = base + x * 4
                    total += 0.114 * data[i] + 0.587 * data[i + 1] + 0.299 * data[i + 2]
                    count += 1
            sums[gy][gx] = total / count
    return sums


def write_sheet(paths, target: Path, scale=4):
    images = [load(p) for p in paths]
    width = images[0][2] // scale
    height = images[0][3] // scale
    rows = []
    for y in range(height):
        row = bytearray(b"\0")
        for data, offset, w, h, bottom_up in images:
            sy = y * scale
            r = (h - 1 - sy) if bottom_up else sy
            base = offset + r * w * 4
            for x in range(width):
                i = base + x * scale * 4
                row += bytes((data[i + 2], data[i + 1], data[i]))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width * len(images), height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    target.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--top", type=int, default=10)
    args = parser.parse_args()
    paths = sorted(args.directory.glob("burst_*.bmp"))
    if len(paths) < 3:
        print("need at least 3 frames", file=sys.stderr)
        return 1
    grids = [blocks(p) for p in paths]
    scores = []
    for i in range(1, len(grids) - 1):
        worst = 0.0
        where = (0, 0)
        flagged = 0
        for gy in range(GRID_Y):
            for gx in range(GRID_X):
                a, b, c = grids[i - 1][gy][gx], grids[i][gy][gx], grids[i + 1][gy][gx]
                # Out and back: b differs from both neighbours in the same direction,
                # while the neighbours agree with each other (motion moves all three).
                spike = min(abs(b - a), abs(b - c)) if (b - a) * (b - c) > 0 else 0.0
                if abs(a - c) > 0.5 * spike:
                    spike = 0.0
                if spike > 12:
                    flagged += 1
                if spike > worst:
                    worst, where = spike, (gx, gy)
        scores.append((worst, flagged, i, where))
    scores.sort(reverse=True)
    print(f"{len(paths)} frames; worst A-B-A spikes (luma 0-255, blocks over 12):")
    for worst, flagged, i, where in scores[: args.top]:
        print(f"  {paths[i].name}: spike {worst:5.1f} at block {where}, {flagged} blocks")
    flickering = sum(1 for s in scores if s[1] > 0)
    print(f"frames with flicker blocks: {flickering} of {len(scores)}")

    # Region test, robust to motion: in each of 4x3 regions, a frame shown between
    # its neighbours differs from each by about half their mutual difference. A
    # one-frame glitch differs from both by more than they differ from each other.
    glitches = []
    for i in range(1, len(grids) - 1):
        worst_ratio, where = 0.0, None
        for ry in range(3):
            for rx in range(4):
                cells = [(gx, gy) for gy in range(ry * 6, ry * 6 + 6) for gx in range(rx * 8, rx * 8 + 8)]

                def distance(p, q):
                    return sum(abs(grids[p][gy][gx] - grids[q][gy][gx]) for gx, gy in cells) / len(cells)

                ab, bc, ac = distance(i - 1, i), distance(i, i + 1), distance(i - 1, i + 1)
                low = min(ab, bc)
                if low < 15:
                    continue
                ratio = low / max(ac, 1.0)
                if ratio > worst_ratio:
                    worst_ratio, where = ratio, (rx, ry, round(low, 1), round(ac, 1))
        if worst_ratio > 1.0:
            glitches.append((worst_ratio, i, where))
    glitches.sort(reverse=True)
    print(f"glitch frames (region ratio > 1.0): {len(glitches)}")
    for ratio, i, where in glitches[:args.top]:
        print(f"  glitch {paths[i].name}: ratio {ratio:.2f} region {where}")
    if scores and scores[0][0] > 12:
        i = scores[0][2]
        sheet = args.directory / "worst_triple.png"
        write_sheet(paths[i - 1:i + 2], sheet)
        print(f"worst triple: {sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

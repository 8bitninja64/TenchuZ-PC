"""Marks Tenchu Z missions as cleared in a tenchu.bin save so Mission Select offers them.

Save layout (5216 bytes, big-endian; unpacked into the game state by sub_82283408):
  +0x1E4  44 x {u16 1, u16 item, u32 count, u32 ?}  item stock (sub_822881C0)
  +0x500  8 chapters of 488 bytes (sub_822B7D48 copies them to game state +7528):
          u32 chapter, u32 count, then count x 48-byte mission records:
            u32 mission id, u32 type flags, u32 ?   (defaults: table at 0x829C837C)
            3 x {u32 status, u32 best score, u32 last result}, one per difficulty
          status bit 0 and bit 1 are both set when a mission is cleared (0x8227E8B8).
The game reads the records without a checksum, so editing them in place is enough.
Mission Select (sub_822B8010 builds the list) then shows every cleared mission on the map;
left/right on the map moves between them, A opens the briefing.

Usage: python tools/save_unlock.py [save] [--difficulty 0-2|all] [--through MISSION_ID]
The default save is %APPDATA%\\TenchuZRecomp\\content\\00000001\\TENCHU_Z\\tenchu.bin;
a copy is kept next to it as tenchu.bin.bak before the first edit.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct

SAVE_SIZE = 0x1460
CHAPTERS = 0x500
CHAPTER_SIZE = 488
RECORD_SIZE = 48


def missions(data: bytes):
    for chapter in range(8):
        base = CHAPTERS + chapter * CHAPTER_SIZE
        number, count = struct.unpack_from('>2I', data, base)
        for index in range(min(count, (CHAPTER_SIZE - 8) // RECORD_SIZE)):
            offset = base + 8 + index * RECORD_SIZE
            mission, flags = struct.unpack_from('>2I', data, offset)
            yield number, mission, flags, offset


def main():
    default = Path(os.environ.get('APPDATA', '.')) / 'TenchuZRecomp/content/00000001/TENCHU_Z/tenchu.bin'
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('save', nargs='?', type=Path, default=default)
    parser.add_argument('--difficulty', default='0', help='difficulty slot to mark (0-2) or "all"')
    parser.add_argument('--through', type=lambda v: int(v, 0), default=None,
                        help='last mission id to mark (default: every mission)')
    parser.add_argument('--list', action='store_true', help='only print the mission records')
    args = parser.parse_args()

    data = bytearray(args.save.read_bytes())
    if len(data) != SAVE_SIZE:
        raise SystemExit(f'{args.save}: expected {SAVE_SIZE} bytes, got {len(data)}')
    slots = range(3) if args.difficulty == 'all' else [int(args.difficulty)]

    if args.list:
        for chapter, mission, flags, offset in missions(data):
            status = [struct.unpack_from('>3I', data, offset + 12 + 12 * s) for s in range(3)]
            print(f'chapter {chapter} mission {mission:#04x} type {flags:#06x}',
                  ' '.join(f'[{s:x} {b} {r}]' for s, b, r in status))
        return

    marked = 0
    for _, mission, _, offset in missions(data):
        if args.through is not None and mission > args.through:
            continue
        for slot in slots:
            status = offset + 12 + 12 * slot
            value = struct.unpack_from('>I', data, status)[0]
            struct.pack_into('>I', data, status, value | 3)
        marked += 1

    backup = args.save.with_name(args.save.name + '.bak')
    if not backup.exists():
        shutil.copyfile(args.save, backup)
    args.save.write_bytes(data)
    print(f'marked {marked} missions cleared (difficulty {args.difficulty}); backup: {backup}')


if __name__ == '__main__':
    main()

"""Drive a game started with TENCHU_CONTROL=<file> and fetch screenshots.

Example: python tools/drive.py build/live/ctl.txt "press a" "wait 2000" --shot build/live/menu
Commands are appended to the control file (see runtime/kernel/control.h); with --shot
a screenshot is requested afterwards, waited for and converted to PNG.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).parent))
from bmp_to_png import convert  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control", type=Path)
    parser.add_argument("commands", nargs="*")
    parser.add_argument("--shot", type=Path, help="screenshot path without extension")
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    lines = list(args.commands)
    bmp = None
    if args.shot:
        bmp = args.shot.with_suffix(".bmp").resolve()
        bmp.unlink(missing_ok=True)
        lines.append(f"shot {bmp}")
    with args.control.open("a", newline="\n") as file:
        for line in lines:
            file.write(line + "\n")
    if bmp:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if bmp.exists() and bmp.stat().st_size > 54:
                time.sleep(0.3)  # let the writer finish
                convert(bmp, bmp.with_suffix(".png"), args.scale)
                print(bmp.with_suffix(".png"))
                return 0
            time.sleep(0.2)
        print("screenshot timed out", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

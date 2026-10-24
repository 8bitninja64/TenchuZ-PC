"""Compile the game module's sources in parallel with clang-cl and link tenchu_game.dll.

Used by tools/make_module.ps1 (the player's first start); needs the Visual Studio
environment (tools/vsenv.ps1) for clang-cl and lld-link. The flags match the
tenchu_game target in CMakeLists.txt. Prints "[n/N] file" per finished unit and,
on the first failure, that unit's compiler output.

  compile_module.py --gen <generated dir> --out <tenchu_game.dll> --obj <dir> --include <dir>...
"""
from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

FLAGS = ["/nologo", "/O2", "/Ob2", "/DNDEBUG", "-MD", "-std:c++20", "/EHsc", "/fp:strict",
         "/clang:-mssse3", "/clang:-msse4.1", "-Wno-everything",
         "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS"]


def physical_memory_gb() -> float:
    class Status(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("available", ctypes.c_ulonglong), ("page_total", ctypes.c_ulonglong),
                    ("page_available", ctypes.c_ulonglong), ("virtual_total", ctypes.c_ulonglong),
                    ("virtual_available", ctypes.c_ulonglong), ("extended", ctypes.c_ulonglong)]
    status = Status()
    status.length = ctypes.sizeof(Status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return 8.0
    return status.total / (1 << 30)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--obj", type=Path, required=True)
    parser.add_argument("--include", type=Path, action="append", default=[])
    parser.add_argument("--extra", type=Path, action="append", default=[], help="additional source")
    args = parser.parse_args()

    sources = [args.gen / line.strip() for line in (args.gen / "sources.txt").read_text().splitlines() if line.strip()]
    sources += args.extra
    # Largest first: the long units start early and the tail stays short.
    sources.sort(key=lambda path: path.stat().st_size, reverse=True)
    args.obj.mkdir(parents=True, exist_ok=True)
    includes = [f"-I{directory}" for directory in [args.gen] + args.include]
    # A big unit can take over a gigabyte to compile.
    jobs = max(1, min(os.cpu_count() or 4, int(physical_memory_gb() // 1.5)))
    print(f"# Compiling {len(sources)} files ({jobs} at a time)", flush=True)

    failed = threading.Event()
    lock = threading.Lock()
    done = [0]

    def compile_one(source: Path) -> None:
        if failed.is_set():
            return
        target = args.obj / (source.stem + ".obj")
        result = subprocess.run(["clang-cl", *FLAGS, *includes, "-c", str(source), f"/Fo{target}"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with lock:
            if result.returncode:
                if not failed.is_set():
                    sys.stdout.write(result.stdout.decode(errors="replace"))
                    print(f"compiling {source.name} failed (exit {result.returncode})", flush=True)
                failed.set()
            else:
                done[0] += 1
                print(f"[{done[0]}/{len(sources)}] {source.name}", flush=True)

    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(compile_one, sources))
    if failed.is_set():
        return 1

    print("# Linking", flush=True)
    temporary = args.out.with_suffix(".tmp.dll")
    response = args.obj / "link.rsp"
    response.write_text("\n".join(f'"{args.obj / (source.stem + ".obj")}"' for source in sources))
    result = subprocess.run(["lld-link", "/nologo", "/DLL", "/OPT:REF", "/OPT:NOICF", f"/OUT:{temporary}",
                             f"@{response}"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.stdout.write(result.stdout.decode(errors="replace"))
        print("linking tenchu_game.dll failed", flush=True)
        return 1
    for leftover in (temporary.with_suffix(".lib"), temporary.with_suffix(".exp")):
        leftover.unlink(missing_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    os.replace(temporary, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

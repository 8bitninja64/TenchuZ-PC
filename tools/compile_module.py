"""Compile the game module's sources in parallel and link tenchu_game.dll / tenchu_game.so.

Used by the player's first start: tools/make_module.ps1 on Windows (clang-cl and lld-link
from the Visual Studio environment, tools/vsenv.ps1), tools/make_module.sh on Linux ($CXX:
the bundled Zig's "zig c++" or clang++). The flags match the tenchu_game target in
CMakeLists.txt. Prints "[n/N] file" per finished unit and, on the first failure, that
unit's compiler output.

  compile_module.py --gen <generated dir> --out <module> --obj <dir> --include <dir>...
  compile_module.py unpack <archive.tar.xz> <dir>   (Linux: the bundled compiler, updates)
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

FLAGS = ["/nologo", "/O2", "/Ob2", "/DNDEBUG", "-MD", "-std:c++20", "/EHsc", "/fp:strict",
         "/clang:-mssse3", "/clang:-msse4.1", "-Wno-everything",
         "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS"]
LINUX_FLAGS = ["-std=c++20", "-O2", "-fPIC", "-DNDEBUG", "-ffp-model=strict", "-mssse3", "-msse4.1",
               "-fvisibility=hidden", "-w"]
WINDOWS = sys.platform == "win32"


def physical_memory_gb() -> float:
    if not WINDOWS:
        try:
            return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
        except (ValueError, OSError):
            return 8.0
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


def content_hash(sources: list[Path], include_dirs: list[Path], compiler: list[str]) -> str:
    """What the module is compiled from: every source, every header the includes can reach and
    the compiler command. An update that changes only the scripts around the build leaves it
    the same, and the module built before is kept."""
    digest = hashlib.sha256()
    digest.update(repr((compiler, FLAGS if WINDOWS else LINUX_FLAGS)).encode())
    files = sorted(set(sources) | {header for directory in include_dirs for header in directory.rglob("*.h")})
    for path in files:
        digest.update(path.name.encode() + b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def unpack(archive: Path, directory: Path) -> int:
    """Unpacks a .tar.xz with Python's tarfile (systems whose tar has no xz)."""
    import tarfile
    directory.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:*") as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(directory, filter="tar")
        else:
            tar.extractall(directory)
    return 0


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "unpack":
        return unpack(Path(sys.argv[2]), Path(sys.argv[3]))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--obj", type=Path, required=True)
    parser.add_argument("--include", type=Path, action="append", default=[])
    parser.add_argument("--extra", type=Path, action="append", default=[], help="additional source")
    args = parser.parse_args()

    sources = [args.gen / line.strip()
               for line in (args.gen / "sources.txt").read_text(encoding="utf-8").splitlines() if line.strip()]
    sources += args.extra
    compiler = ["clang-cl"] if WINDOWS else shlex.split(os.environ.get("CXX", "clang++"))
    content = content_hash(sources, [args.gen] + args.include, compiler)
    stamp = args.out.with_name(args.out.name + ".content")
    if args.out.exists() and stamp.exists() and stamp.read_text().strip() == content:
        print(f"# The game code is unchanged: keeping {args.out.name}", flush=True)
        return 0
    stamp.unlink(missing_ok=True)
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

    suffix = ".obj" if WINDOWS else ".o"

    def compile_one(source: Path) -> None:
        if failed.is_set():
            return
        target = args.obj / (source.stem + suffix)
        command = (["clang-cl", *FLAGS, *includes, "-c", str(source), f"/Fo{target}"] if WINDOWS else
                   [*compiler, *LINUX_FLAGS, *includes, "-c", str(source), "-o", str(target)])
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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
    if not WINDOWS:
        temporary = args.out.with_name(args.out.name + ".tmp")
        response = args.obj / "link.rsp"
        response.write_text("\n".join(shlex.quote(str(args.obj / (source.stem + suffix))) for source in sources),
                            encoding="utf-8")
        # Its own symbols bind inside it (the runtime defines some of the same names).
        result = subprocess.run([*compiler, "-shared", "-s", "-Wl,-Bsymbolic", "-o", str(temporary), f"@{response}"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            sys.stdout.write(result.stdout.decode(errors="replace"))
            print(f"linking {args.out.name} failed", flush=True)
            return 1
        args.out.parent.mkdir(parents=True, exist_ok=True)
        os.replace(temporary, args.out)
        stamp.write_text(content)
        return 0
    temporary = args.out.with_suffix(".tmp.dll")
    response = args.obj / "link.rsp"
    # lld-link reads response files as UTF-8 (the user folder may have any characters).
    response.write_text("\n".join(f'"{args.obj / (source.stem + ".obj")}"' for source in sources), encoding="utf-8")
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
    stamp.write_text(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())

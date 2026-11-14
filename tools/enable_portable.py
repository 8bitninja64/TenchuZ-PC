"""Copy existing user data into an installation and enable portable mode.

Close the game first. Run: python tools/enable_portable.py
The original data is never modified. Existing portable data is never overwritten.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile


def default_source() -> Path:
    if sys.platform == "win32":
        # Match the runtime's Windows known-folder lookup, including redirected profiles.
        import ctypes
        import uuid
        folder_id = (ctypes.c_ubyte * 16).from_buffer_copy(
            uuid.UUID("3eb685db-65f9-4cf6-a03a-e3ef65729f3d").bytes_le)
        result = ctypes.c_void_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(
                ctypes.byref(folder_id), 0, None, ctypes.byref(result)) != 0:
            raise OSError("Cannot locate Windows Roaming AppData; use --source.")
        try:
            return Path(ctypes.wstring_at(result)) / "TenchuZRecomp"
        finally:
            ctypes.windll.ole32.CoTaskMemFree(result)
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "TenchuZRecomp"


def digest(path: Path) -> bytes:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.digest()


def migrate(source: Path, directory: Path) -> None:
    source, directory = source.resolve(), directory.resolve()
    if not any((directory / name).is_file() for name in ("TenchuZ.exe", "TenchuZ")):
        raise ValueError("Choose the installation containing TenchuZ.exe or TenchuZ (--directory).")
    if not source.is_dir():
        raise ValueError(f"No existing data at {source}. For a fresh setup, create portable.txt beside the executable.")
    if directory == source or source in directory.parents:
        raise ValueError("The installation must not be inside the source data folder.")
    destination = directory / "user"
    marker = directory / "portable.txt"
    if destination.exists() or destination.is_symlink() or marker.exists():
        raise ValueError("Portable data or portable.txt already exists; nothing was overwritten.")
    # Never merge into an active data folder. A failed copy remains staged for inspection.
    stage = Path(tempfile.mkdtemp(prefix="portable-migration-", dir=directory))
    try:
        for path in source.rglob("*"):
            if path.is_symlink() or (getattr(path.lstat(), "st_file_attributes", 0) &
                                     getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
                raise ValueError(f"Linked data paths are not supported: {path}")
            target = stage / path.relative_to(source)
            if path.is_dir():
                target.mkdir(exist_ok=True)
            elif path.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                if digest(path) != digest(target):
                    raise OSError(f"Copy verification failed: {path}")
        # rename refuses an existing destination on Windows; check also for other platforms.
        if destination.exists():
            raise ValueError("Portable data appeared during migration; close the game and retry.")
        stage.rename(destination)
        with marker.open("x", encoding="utf-8") as stream:
            stream.write("User data is stored in the adjacent user folder.\n")
    except Exception:
        print(f"Migration did not finish. Copied files are in {stage if stage.exists() else destination}.", file=sys.stderr)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="existing data folder (default: this account's game data)")
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="installation containing the game executable")
    args = parser.parse_args()
    try:
        migrate(args.source if args.source is not None else default_source(), args.directory)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Portable mode enabled. Original data was preserved.")
    print("Start the game once here to remember a relative disc path, then move the whole folder and disc together.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

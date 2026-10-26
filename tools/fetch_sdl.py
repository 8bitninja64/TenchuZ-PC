"""Download the pinned SDL3 development package (gamepad support) into build/deps/SDL3."""
from pathlib import Path
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# 3.4 drives more controllers' vibration over Bluetooth (8BitDo Ultimate 2: its HIDAPI driver).
VERSION = "3.4.18"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL3-devel-{VERSION}-VC.zip"
SHA256 = "78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd"


def main() -> int:
    target = ROOT / "build" / "deps" / "SDL3"
    marker = target / "VERSION"
    if (target / "include" / "SDL3" / "SDL.h").exists() and marker.exists() and marker.read_text().strip() == VERSION:
        print("SDL3", VERSION, "already present in", target)
        return 0
    shutil.rmtree(target, ignore_errors=True)  # another version
    data = urllib.request.urlopen(URL).read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError("SDL3 package hash mismatch")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        prefix = f"SDL3-{VERSION}/"
        for member in archive.infolist():
            if member.is_dir() or not member.filename.startswith(prefix):
                continue
            relative = Path(member.filename[len(prefix):])
            if ".." in relative.parts:
                raise RuntimeError(f"unexpected path {member.filename}")
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, destination.open("wb") as out:
                shutil.copyfileobj(source, out)
    marker.write_text(VERSION + "\n")
    print("SDL3", VERSION, "installed in", target)
    return 0


if __name__ == "__main__":
    sys.exit(main())

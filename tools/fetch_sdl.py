"""Download the pinned SDL3 development package (gamepad support) into build/deps/SDL3."""
from pathlib import Path
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "3.2.10"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL3-devel-{VERSION}-VC.zip"
SHA256 = "afcfafc7e389e048c3693e11fe645ea84392c35673e4f201fe21ac513719935d"


def main() -> int:
    target = ROOT / "build" / "deps" / "SDL3"
    if (target / "include" / "SDL3" / "SDL.h").exists():
        print("SDL3 already present in", target)
        return 0
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
    print("SDL3", VERSION, "installed in", target)
    return 0


if __name__ == "__main__":
    sys.exit(main())

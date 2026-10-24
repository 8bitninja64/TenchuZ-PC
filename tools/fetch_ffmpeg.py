"""Download the prebuilt FFmpeg (XMA audio and WMV movies) into build/deps/ffmpeg.

The archive is what tools/build_xma_codec.py produces (pinned FFmpeg n8.1.2 with its
small XMA decoder option; the script is also the corresponding source), built by the
repository's ffmpeg workflow (.github/workflows/ffmpeg.yml) and attached to the
deps-ffmpeg release. Its SHA-256 is pinned here. Building it yourself instead needs
MSYS2: python tools/build_xma_codec.py.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "build/deps/ffmpeg"
URL = "https://github.com/8bitninja64/TenchuZ-PC/releases/download/deps-ffmpeg-n8.1.2-1/ffmpeg-tenchu-n8.1.2-windows-x64.zip"
SHA256 = "e3aff23c3390bd2aa2b300f8b34450cc6b12cee4c3cd09c6e94b194a570f164f"


def main() -> int:
    if not SHA256:
        print("fetch_ffmpeg: no prebuilt FFmpeg is pinned yet; build it with tools/build_xma_codec.py", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / "ffmpeg.zip"
        print(f"Downloading {URL}", flush=True)
        with urllib.request.urlopen(URL, timeout=120) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != SHA256:
            print(f"fetch_ffmpeg: {archive.name} has SHA-256 {digest}, expected {SHA256}", file=sys.stderr)
            return 1
        unpacked = Path(temporary) / "ffmpeg"
        with zipfile.ZipFile(archive) as zip_file:
            for member in zip_file.namelist():
                destination = (unpacked / member).resolve()
                if not destination.is_relative_to(unpacked.resolve()):
                    print(f"fetch_ffmpeg: unsafe path {member} in the archive", file=sys.stderr)
                    return 1
            zip_file.extractall(unpacked)
        if not (unpacked / "include/libavcodec/avcodec.h").exists():
            print("fetch_ffmpeg: the archive is not an FFmpeg build", file=sys.stderr)
            return 1
        if TARGET.exists():
            shutil.rmtree(TARGET)
        TARGET.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(unpacked), TARGET)
    print(f"FFmpeg in {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

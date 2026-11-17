"""Bundle pinned, verified first-start tools. Run only when making a release."""
from pathlib import Path
import argparse
import hashlib
import shutil
import urllib.request
import zipfile

TOOLS = {
    "python": ("https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip",
               "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159"),
    "zig": ("https://ziglang.org/download/0.17.1/zig-x86_64-windows-0.17.1.zip",
            "b03f477a1943673e02aca0f570e8030dffd536ce1f45cbc4d3326ec31159562e"),
}


def bundle(destination: Path, cache: Path):
    cache.mkdir(parents=True, exist_ok=True)
    destination.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in TOOLS.items():
        archive = cache / (name + ".zip")
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            temporary = archive.with_suffix(".download")
            print(f"Downloading {url}", flush=True)
            urllib.request.urlretrieve(url, temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
                temporary.unlink()
                raise RuntimeError(f"{name} download failed SHA-256 verification")
            temporary.replace(archive)
        if name == "python":
            with zipfile.ZipFile(archive) as package:
                package.extractall(destination / "python")
        else:
            # Keep the compiler compressed until a module build needs it.
            shutil.copyfile(archive, destination / "zig.zip")
    (destination / "bundled-tools.txt").write_text(
        "\n".join(f"{name}: {url}\nSHA-256: {sha}" for name, (url, sha) in TOOLS.items()) + "\n",
        encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--cache", type=Path, default=Path("build/standalone-tools"))
    args = parser.parse_args()
    bundle(args.destination, args.cache)

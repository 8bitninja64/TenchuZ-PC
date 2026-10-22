"""Fetch the pinned XenonRecomp source into the shared tools folder and patch it.

The patch series is The Darkness Recomp's generator fixes (GPLv3, see
tools/patches/README.md) followed by the Tenchu Z additions.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT.parent / "_Tools/XenonRecomp"
UPSTREAM = "https://github.com/hedge-dev/XenonRecomp.git"
COMMIT = "c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527"
PATCHES = (
    ROOT / "tools/patches/xenonrecomp-darkness.patch",
    ROOT / "tools/patches/xenonrecomp-corrections.patch",
    ROOT / "tools/patches/xenonrecomp-tenchuz.patch",
)


def git(directory: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess:
    # The patches use LF endings; never let a global autocrlf rewrite the tree.
    result = subprocess.run(["git", "-c", "core.autocrlf=false", "-C", str(directory), *arguments],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)} failed:\n{result.stderr.strip()}")
    return result


def setup(destination: Path) -> None:
    destination = destination.resolve()
    if not destination.exists() or not any(destination.iterdir()):
        destination.parent.mkdir(parents=True, exist_ok=True)
        print("Downloading XenonRecomp...", flush=True)
        git(destination.parent, "clone", "--no-checkout", "--", UPSTREAM, str(destination))
        git(destination, "config", "core.autocrlf", "false")
        git(destination, "checkout", "--detach", COMMIT)
    if git(destination, "merge-base", "--is-ancestor", COMMIT, "HEAD", check=False).returncode:
        raise RuntimeError(f"{destination} does not contain XenonRecomp {COMMIT}; "
                           "move it aside and rerun.")
    # Later patches touch lines of earlier ones, so only the last patch of an
    # installed series reverse-applies cleanly; a fresh tree takes the first.
    if not git(destination, "apply", "--reverse", "--check", str(PATCHES[-1]), check=False).returncode:
        print("Generator patches are already installed.")
    elif not git(destination, "apply", "--check", str(PATCHES[0]), check=False).returncode:
        for patch in PATCHES:
            git(destination, "apply", "--whitespace=nowarn", str(patch))
            print(f"Applied: {patch.name}")
    else:
        raise RuntimeError(f"{destination} has local changes that conflict with the patch series; "
                           "move it aside and rerun.")
    git(destination, "submodule", "update", "--init", "--recursive")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=DESTINATION)
    args = parser.parse_args()
    try:
        setup(args.destination)
    except (OSError, RuntimeError) as exc:
        print(f"Generator setup: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

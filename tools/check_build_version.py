"""Fail release validation if the packaged executable has the wrong embedded version."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('expected')
    args = parser.parse_args()
    result = subprocess.run([str(args.executable.resolve()), '--version'],
                            capture_output=True, text=True, timeout=15, check=True)
    actual = result.stdout.strip()
    if actual != args.expected:
        raise SystemExit(f'Wrong embedded version: expected {args.expected!r}, got {actual!r}')
    print(f'Embedded version verified: {actual}')


if __name__ == '__main__':
    main()

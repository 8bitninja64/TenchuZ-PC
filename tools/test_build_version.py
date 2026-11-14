"""Exercise version changes in a real reused CMake build directory, without game data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildVersionTests(unittest.TestCase):
    def test_cached_version_tracks_release_environment(self):
        with tempfile.TemporaryDirectory(prefix='tenchu-version-') as directory:
            root = Path(directory)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\nproject(VersionTest NONE)\n'
                f'include("{(ROOT / "cmake/BuildVersion.cmake").as_posix()}")\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${TENCHU_VERSION}")\n')
            env = dict(os.environ)
            env.pop('TENCHU_VERSION', None)
            for version in (None, 'v0.3.2', 'v0.3.3', 'dev'):
                if version is not None:
                    env['TENCHU_VERSION'] = version
                subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')],
                               env=env, check=True, capture_output=True, text=True)
                self.assertEqual((root / 'build/version.txt').read_text(), version or 'dev')


if __name__ == '__main__':
    unittest.main()

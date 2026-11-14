"""Run with python -m unittest discover -s tools -p test_portable.py."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from enable_portable import migrate


class PortableMigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.source = root / "original"
        self.install = root / "Portable game [test]"
        self.source.mkdir()
        self.install.mkdir()
        (self.install / "TenchuZ.exe").write_bytes(b"test executable")
        self.files = {"tenchu_game.dll": b"module", "tenchu_game.dll.stamp": b"kit stamp",
                      "tenchu_game.dll.content": b"content stamp",
                      "content/00000001/TENCHU_Z/tenchu.bin": b"save",
                      "profile/settings.bin": b"profile", "shadercache/shader": b"shader"}
        for name, data in self.files.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def test_copy_preserves_originals_and_stamps(self):
        migrate(self.source, self.install)
        self.assertTrue((self.install / "portable.txt").is_file())
        for name, data in self.files.items():
            self.assertEqual((self.source / name).read_bytes(), data)
            self.assertEqual((self.install / "user" / name).read_bytes(), data)

    def test_existing_data_is_never_overwritten(self):
        (self.install / "user").mkdir()
        save = self.install / "user/precious-save"
        save.write_bytes(b"newer save")
        with self.assertRaises(ValueError):
            migrate(self.source, self.install)
        self.assertEqual(save.read_bytes(), b"newer save")
        self.assertFalse((self.install / "portable.txt").exists())

    def test_failed_copy_does_not_activate(self):
        with patch("enable_portable.shutil.copy2", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                migrate(self.source, self.install)
        self.assertFalse((self.install / "portable.txt").exists())
        self.assertFalse((self.install / "user").exists())
        for name, data in self.files.items():
            self.assertEqual((self.source / name).read_bytes(), data)

    def test_corrupt_copy_does_not_activate(self):
        def corrupt(source, target):
            target.write_bytes(b"corrupted")
        with patch("enable_portable.shutil.copy2", side_effect=corrupt):
            with self.assertRaises(OSError):
                migrate(self.source, self.install)
        self.assertFalse((self.install / "portable.txt").exists())
        self.assertFalse((self.install / "user").exists())

    def test_repeated_migration_refused(self):
        migrate(self.source, self.install)
        with self.assertRaises(ValueError):
            migrate(self.source, self.install)

    def test_missing_source_does_not_activate(self):
        with self.assertRaises(ValueError):
            migrate(self.source / "missing", self.install)
        self.assertFalse((self.install / "portable.txt").exists())


if __name__ == "__main__":
    unittest.main()

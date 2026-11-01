import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import enable


class UpgradeTests(unittest.TestCase):
    base = b"function blocked(added, removed) {\n    return false;\n}\n\nupdate(null, null);\n"
    hook = Path(__file__).with_name("stream-edges.js").read_bytes()

    def setUp(self):
        self.supported = patch.object(enable, "STOCK", {hashlib.sha256(self.base).hexdigest()})
        self.supported.start()
        self.addCleanup(self.supported.stop)

    def test_patch_twice_and_restore_exact_bytes(self):
        updated = enable.content(self.base, self.hook)
        self.assertIn(enable.BLOCKED + enable.GUARD, updated)
        self.assertEqual(enable.content(updated, self.hook), updated)
        self.assertEqual(enable.content(updated, self.hook, restore=True), self.base)
        self.assertEqual(enable.content(self.base, self.hook, restore=True), self.base)
        with self.assertRaises(ValueError):
            enable.content(self.base + b'// unfamiliar stock', self.hook)

    def test_preserve_new_upstream_changes(self):
        integrated = enable.content(self.base, self.hook) + b"// upstream addition\n"
        self.assertEqual(enable.content(integrated, self.hook), integrated)
        with self.assertRaises(ValueError):
            enable.content(integrated, self.hook, restore=True)

    def test_failed_reload_restores_file_mode_and_original_plugin(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "main.js"
            path.write_bytes(self.base)
            path.chmod(0o640)
            reload = Mock(side_effect=[RuntimeError("new script failed"), None])
            with patch.object(enable, "loaded", return_value=True), patch.object(enable, "reload_plugin", reload):
                with self.assertRaisesRegex(RuntimeError, "已还原"):
                    enable.install(path, self.hook)
            self.assertEqual(reload.call_count, 2)
            self.assertEqual(path.read_bytes(), self.base)
            self.assertEqual(path.stat().st_mode & 0o777, 0o640)
            self.assertEqual(list(path.parent.iterdir()), [path])


if __name__ == "__main__":
    unittest.main()

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[2]
SPEC = importlib.util.spec_from_file_location("patch_connect_colour_match", ROOT / "scripts/packaging/patch_connect_colour_match.py")
patch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patch)


class ColourPatchTest(unittest.TestCase):
    def fixture(self, target):
        source = ROOT / "resources/web/flutter_web"
        # Retained upstream entrypoints, independent of whether the local index
        # has already been patched.
        main = "main.b347974c91352c2f.js"
        bootstrap = "flutter_bootstrap.6816d840c75f4de9.js"
        for name in (main, bootstrap):
            (target / name).write_bytes((source / name).read_bytes())
        (target / "index.html").write_text(f'<link href="{main}"><script src="{bootstrap}"></script>', encoding="utf-8")

    def test_patch_cache_urls_and_idempotence(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            self.fixture(target)
            original = (target / "index.html").read_bytes()
            result = patch.patch_web(target)
            self.assertEqual((target / result["index_backup"]).read_bytes(), original)
            self.assertNotEqual((target / "index.html").read_bytes(), original)
            for name, sha in result["patched_hashes"].items():
                self.assertEqual(patch.digest((target / name).read_bytes()), sha)
                if name != "index.html":
                    self.assertIn(sha[:16], name)
                    self.assertIn(name.encode(), (target / "index.html").read_bytes())
            self.assertEqual(patch.patch_web(target), result)
            (target / "index.html").write_text("changed", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Previously patched"):
                patch.patch_web(target)

    def test_unknown_bundle_rejected_without_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            self.fixture(target)
            (target / "main.b347974c91352c2f.js").write_text("unknown", encoding="utf-8")
            before = {p.name: p.read_bytes() for p in target.iterdir()}
            with self.assertRaisesRegex(RuntimeError, "Unsupported"):
                patch.patch_web(target)
            self.assertEqual(before, {p.name: p.read_bytes() for p in target.iterdir()})

    def test_restore_and_reapply(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            self.fixture(target)
            original = (target / "index.html").read_bytes()
            manifest = patch.patch_web(target)
            patch.restore_web(target)
            self.assertEqual((target / "index.html").read_bytes(), original)
            self.assertFalse((target / patch.MANIFEST).exists())
            self.assertEqual(patch.patch_web(target), manifest)

    def test_both_known_bundles_only_change_the_selector(self):
        for name in ("main.b347974c91352c2f.js", "main.465809ba4df3cfa9.js"):
            original = (ROOT / "resources/web/flutter_web" / name).read_bytes()
            output = patch.patch_bundle(original)
            selector, distance = patch.SUPPORTED[patch.digest(original)]
            prefix = f"{selector}(a,b){{".encode()
            begin = original.index(prefix)
            end = original.index(b"return q},", begin) + len(b"return q},")
            patched_end = output.index(b"return selected<0?null:b[selected]},", begin) + len(b"return selected<0?null:b[selected]},")
            self.assertEqual(original[:begin], output[:begin])
            self.assertEqual(original[end:], output[patched_end:])
            self.assertIn(f"this.{distance}(a.x,b[i].f)".encode(), output)
            self.assertIn(b"selectFilamentColour", patch.patch_bundle(original.replace(b"\r\n", b"\n")))


if __name__ == "__main__":
    unittest.main()

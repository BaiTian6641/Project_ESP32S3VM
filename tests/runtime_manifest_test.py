"""Source identity collection tests use local Git repositories, never hardware."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_manifest", ROOT / "tools/runtime-manifest.py")
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


class ManifestTests(unittest.TestCase):
    def setUp(self):
        test_root = ROOT / "build-manifest-tests"
        test_root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assertTrue(self.root.absolute().is_relative_to(test_root.absolute()))

    def repository(self, name):
        path = self.root / name
        path.mkdir()
        git(path, "init", "-q")
        git(path, "config", "user.name", "Manifest test")
        git(path, "config", "user.email", "manifest@example.invalid")
        (path / "data.txt").write_text("initial\n")
        git(path, "add", "data.txt")
        git(path, "commit", "-qm", "initial")
        return path

    def test_symbolic_detached_and_packed_head_are_exact(self):
        path = self.repository("repo")
        expected = git(path, "rev-parse", "HEAD")
        self.assertEqual(manifest.git_head(path), expected)
        git(path, "pack-refs", "--all", "--prune")
        self.assertEqual(manifest.git_head(path), expected)
        git(path, "checkout", "-q", "--detach", expected)
        self.assertEqual(manifest.git_head(path), expected)

    def test_nested_gitlinks_are_verified_and_mismatch_is_not_silently_recorded(self):
        root = self.repository("root")
        child = self.repository("child")
        nested = self.repository("nested")
        git(child, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(nested), "nested/lib")
        git(child, "commit", "-qam", "add nested")
        git(root, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(child), "components/child")
        git(root, "commit", "-qam", "add child")
        git(root, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive", "-q")
        records = manifest.submodule_revisions(root)
        self.assertEqual([item["path"] for item in records], ["components/child", "components/child/nested/lib"])
        self.assertTrue(all(item["commit"] == item["expected_commit"] for item in records))
        checkout = root / "components/child/nested/lib"
        git(checkout, "config", "user.name", "Manifest test")
        git(checkout, "config", "user.email", "manifest@example.invalid")
        (checkout / "data.txt").write_text("different revision\n")
        git(checkout, "commit", "-qam", "drift")
        with self.assertRaisesRegex(ValueError, "differs from its pinned gitlink"):
            manifest.submodule_revisions(root)

    def test_repeated_vendor_metadata_does_not_hide_submodule_identity(self):
        root = self.repository("root")
        child = self.repository("child")
        path = "components/sensor library"
        git(root, "-c", "protocol.file.allow=always", "submodule", "add",
            "-q", str(child), path)
        key = f"submodule.{path}.sbom-cpe"
        git(root, "config", "-f", ".gitmodules", "--add", key, "vendor:first")
        git(root, "config", "-f", ".gitmodules", "--add", key, "vendor:second")
        git(root, "commit", "-qam", "repeated vendor metadata")
        records = manifest.submodule_revisions(root)
        self.assertEqual(records, [{
            "path": path, "commit": manifest.git_head(root / path),
            "expected_commit": git(child, "rev-parse", "HEAD")}])

    def test_duplicate_path_option_cannot_replace_or_hide_a_gitlink(self):
        root = self.repository("root")
        child = self.repository("child")
        for path in ("components/child", "components/other"):
            git(root, "-c", "protocol.file.allow=always", "submodule", "add",
                "-q", str(child), path)
        git(root, "config", "-f", ".gitmodules", "--add",
            "submodule.components/child.path", "components/other")
        git(root, "commit", "-qam", "ambiguous path metadata")
        with self.assertRaisesRegex(ValueError, "Duplicate submodule path"):
            manifest.submodule_revisions(root)

    def test_missing_checkout_does_not_produce_incomplete_provenance(self):
        path = self.root / "not-initialized"
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "not initialized"):
            manifest.git_head(path)


if __name__ == "__main__":
    unittest.main()

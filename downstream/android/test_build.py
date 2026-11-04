"""Regression for nested-checkout patch application in clean CI builds."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from build import apply_patch


class SourcePatchTest(unittest.TestCase):
    def test_patch_changes_extracted_source_inside_another_git_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(["git", "init", "-q", str(outer)], check=True)
            source = outer / "build/sources/dependency"
            source.mkdir(parents=True)
            (source / "example.txt").write_text("before\n")
            patch = outer / "dependency.patch"
            patch.write_text("diff --git a/example.txt b/example.txt\n"
                             "--- a/example.txt\n+++ b/example.txt\n"
                             "@@ -1 +1 @@\n-before\n+after\n")
            apply_patch(source, patch)
            self.assertEqual("after\n", (source / "example.txt").read_text())
            self.assertFalse((outer / "example.txt").exists())


if __name__ == "__main__":
    unittest.main()

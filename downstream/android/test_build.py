"""Regressions for isolated dependency builds, patches and header caches."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from build import apply_patch, cmake_build, prepare_boost_headers


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


class BoostHeadersTest(unittest.TestCase):
    def test_source_upgrade_replaces_cached_headers_and_removes_obsolete_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            headers = source / "libs/config/include/boost"
            headers.mkdir(parents=True)
            (headers / "version.hpp").write_text("old version")
            (headers / "obsolete.hpp").touch()
            include = root / "work/boost/include"
            prepare_boost_headers(source, include, "old source digest")
            (headers / "version.hpp").write_text("new version")
            (headers / "obsolete.hpp").unlink()
            prepare_boost_headers(source, include, "new source digest")
            self.assertEqual("new version", (include / "boost/version.hpp").read_text())
            self.assertFalse((include / "boost/obsolete.hpp").exists())


class SourceBuildIsolationTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
    def test_dependency_cannot_read_enclosing_repository_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(["git", "init", "-q", str(outer)], check=True)
            subprocess.run(["git", "-C", str(outer), "-c", "user.name=Test", "-c",
                            "user.email=test@example.com", "commit", "-q", "--allow-empty", "-m", "fixture"], check=True)
            source = outer / "build/sources/dependency"
            source.mkdir(parents=True)
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.15)\nproject(Isolation LANGUAGES NONE)\n'
                'find_package(Git REQUIRED)\n'
                'execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel\n'
                '  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE result ERROR_QUIET)\n'
                'if(result EQUAL 0)\nmessage(FATAL_ERROR "Dependency read enclosing repository")\nendif()\n')
            cmake_build(shutil.which("cmake"), source, outer / "output", outer / "install", 1, install=False)


if __name__ == "__main__":
    unittest.main()

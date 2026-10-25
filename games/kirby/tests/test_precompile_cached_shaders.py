import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/precompile_cached_shaders.py"
SPEC = importlib.util.spec_from_file_location("kirby_precompile", SCRIPT)
PRECOMPILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRECOMPILE)


class ShaderCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.output = self.root / "libs"
        self.cache.mkdir()
        self.output.mkdir()
        self.source = self.cache / "variant_b.metal"
        self.source.write_bytes(b"test shader bytes\n")
        self.digest = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.library = self.output / f"{self.digest}.metallib"
        self.stamp = self.output / "compiler.txt"
        self.calls = []

    def compiler(self, command, **kwargs):
        self.calls.append(command)
        output = Path(command[command.index("-o") + 1])
        output.write_bytes(b"MTLBnew compiler output")
        self.assertEqual(Path(command[-1]).read_bytes(), b"test shader bytes\n")
        return subprocess.CompletedProcess(command, 0, "", "")

    def compile(self, identity="compiler-v2"):
        with redirect_stdout(io.StringIO()):
            PRECOMPILE.compile_cache(self.cache, self.output, "mock-metal", identity, 2)

    def test_changed_compiler_rebuilds_and_retires_all_old_libraries(self):
        self.library.write_bytes(b"MTLBold")
        extra = self.output / "not-in-current-cache.metallib"
        extra.write_bytes(b"MTLBold extra")
        self.stamp.write_text("compiler-v1")
        with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=self.compiler):
            self.compile()
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.library.read_bytes(), b"MTLBnew compiler output")
        self.assertEqual(self.stamp.read_text(), "compiler-v2")
        self.assertFalse(extra.exists())
        backups = list((self.output / "compiler-backups").glob("*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / self.library.name).read_bytes(), b"MTLBold")
        self.assertEqual((backups[0] / extra.name).read_bytes(), b"MTLBold extra")

    def test_matching_compiler_reuses_valid_library(self):
        self.library.write_bytes(b"MTLBexisting")
        self.stamp.write_text("compiler-v2")
        with mock.patch.object(PRECOMPILE.subprocess, "run") as compiler:
            self.compile()
        compiler.assert_not_called()

    def test_missing_identity_cannot_reuse_existing_library(self):
        self.library.write_bytes(b"MTLBunknown")
        with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=self.compiler):
            self.compile()
        self.assertEqual(len(self.calls), 1)

    def test_corrupt_library_recompiled_even_with_matching_compiler(self):
        for bad in (b"", b"MTLB", b"NOT A METAL LIBRARY"):
            with self.subTest(data=bad):
                self.library.write_bytes(bad)
                self.stamp.write_text("compiler-v2")
                self.calls.clear()
                with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=self.compiler):
                    self.compile()
                self.assertEqual(len(self.calls), 1)

    def test_compiler_failure_keeps_previous_identity_and_output(self):
        self.library.write_bytes(b"MTLBknown working")
        self.stamp.write_text("compiler-v1")
        failed = subprocess.CompletedProcess([], 1, "", "bad shader")
        with mock.patch.object(PRECOMPILE.subprocess, "run", return_value=failed):
            with self.assertRaises(SystemExit):
                self.compile()
        self.assertEqual(self.library.read_bytes(), b"MTLBknown working")
        self.assertEqual(self.stamp.read_text(), "compiler-v1")
        self.assertFalse(list(self.output.glob(".compile-*")))
        self.assertIn("bad shader", (self.output / "compile-failures.log").read_text())

    def test_invalid_success_output_does_not_publish_identity(self):
        def invalid(command, **kwargs):
            Path(command[command.index("-o") + 1]).write_bytes(b"garbage")
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=invalid):
            with self.assertRaises(SystemExit):
                self.compile()
        self.assertFalse(self.stamp.exists())
        self.assertFalse(self.library.exists())

    def test_duplicate_source_bytes_compile_once(self):
        (self.cache / "duplicate_b.metal").write_bytes(self.source.read_bytes())
        with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=self.compiler):
            self.compile()
        self.assertEqual(len(self.calls), 1)

    def test_success_clears_old_failure_log(self):
        (self.output / "compile-failures.log").write_text("old failure")
        with mock.patch.object(PRECOMPILE.subprocess, "run", side_effect=self.compiler):
            self.compile()
        self.assertFalse((self.output / "compile-failures.log").exists())

    def test_worker_count_is_bounded(self):
        for value in ("0", "-1", "17", "999"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                PRECOMPILE.worker_count(value)
        self.assertEqual(PRECOMPILE.worker_count("1"), 1)
        self.assertEqual(PRECOMPILE.worker_count("16"), 16)


if __name__ == "__main__":
    unittest.main()

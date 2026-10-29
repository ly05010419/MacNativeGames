import pathlib
import errno
import os
import plistlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "Apple Foundation native store")
class ProgressStoreTests(unittest.TestCase):
    def test_full_filesystem(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-full-volume-") as directory:
            root = pathlib.Path(directory)
            binary = root / "FullVolumeTests"
            build = subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-framework", "Foundation",
                str(ROOT / "games/totk/runtime/progress_store.m"),
                str(ROOT / "games/totk/runtime/progress_transport.m"),
                str(ROOT / "tests/progress_full_probe.m"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            image, mount, outside = root / "fixture.dmg", root / "mount", root / "outside"
            mount.mkdir(); outside.mkdir()
            subprocess.run(["hdiutil", "create", "-quiet", "-size", "64m", "-fs", "HFS+",
                            "-volname", "TOTKSyncFullFixture", str(image)], check=True, capture_output=True)
            attached = subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", str(mount),
                                       "-plist", str(image)], check=True, capture_output=True)
            entity = next(item for item in plistlib.loads(attached.stdout)["system-entities"] if item.get("mount-point"))
            try:
                (mount / ".metadata_never_index").touch()
                subprocess.run([str(binary), "prepare", str(mount), str(outside)], check=True, capture_output=True, timeout=30)
                descriptor = os.open(mount / "filler", os.O_WRONLY | os.O_CREAT, 0o600)
                try:
                    block = os.urandom(1024 * 1024)
                    while True:
                        os.write(descriptor, block)
                except OSError as error:
                    self.assertEqual(error.errno, errno.ENOSPC)
                finally:
                    os.close(descriptor)
                result = subprocess.run([str(binary), "verify", str(mount), str(outside)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())
            finally:
                subprocess.run(["hdiutil", "detach", entity["dev-entry"]], check=True, capture_output=True)

    def test_native_coordinator(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-coordinator-") as directory:
            binary = pathlib.Path(directory) / "ProgressSyncTests"
            build = subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-framework", "Foundation", "-framework", "AppKit",
                str(ROOT / "games/totk/runtime/progress_store.m"),
                str(ROOT / "games/totk/runtime/progress_transport.m"),
                str(ROOT / "games/totk/runtime/progress_sync.m"),
                str(ROOT / "tests/progress_sync_probe.m"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            scenarios = ("startup", "running", "fork", "restore", "commit", "retry", "recovery", "backup", "pause", "invalid", "pending-error", "pending-inbox", "errors", "enable-during-game", "management", "management-new")
            for language in ("en", "zh"):
                for scenario in scenarios:
                    with self.subTest(language=language, scenario=scenario):
                        result = subprocess.run([str(binary), scenario, str(pathlib.Path(directory) / language / scenario)],
                                                env={**os.environ, "TOTK_UI_LANG": language},
                                                capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        print(result.stdout.strip())

    def test_native_ui_messages(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-ui-") as directory:
            binary = pathlib.Path(directory) / "ProgressUITests"
            build = subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-framework", "Foundation", "-framework", "AppKit",
                str(ROOT / "games/totk/runtime/progress_store.m"),
                str(ROOT / "games/totk/runtime/progress_transport.m"),
                str(ROOT / "games/totk/runtime/progress_sync.m"),
                str(ROOT / "tests/progress_ui_probe.m"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_native_transport(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-transfer-build-") as directory:
            binary = pathlib.Path(directory) / "transport-probe"
            build = subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-framework", "Foundation",
                str(ROOT / "games/totk/runtime/progress_store.m"),
                str(ROOT / "games/totk/runtime/progress_transport.m"),
                str(ROOT / "tests/progress_transport_probe.m"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_native_save_gate(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-gate-build-") as directory:
            binary = pathlib.Path(directory) / "gate-probe"
            build = subprocess.run([
                "xcrun", "clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-I", str(ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn/include"),
                str(ROOT / "tests/progress_gate_probe.c"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_native_store(self):
        with tempfile.TemporaryDirectory(prefix="totk-progress-build-") as directory:
            binary = pathlib.Path(directory) / "progress-probe"
            subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-framework", "Foundation",
                "-DTOTK_PROGRESS_TESTING=1",
                str(ROOT / "games/totk/runtime/progress_store.m"),
                str(ROOT / "tests/progress_store_probe.m"), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("progress checks", result.stdout)
            print(result.stdout.strip())

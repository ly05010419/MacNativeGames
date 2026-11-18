"""DSU wire format, independent clocks and the actual UDP/gamepad receive path."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and Xcode')
class DSUTests(unittest.TestCase):
    def test_native_motion_coexistence(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'native-motion'
            build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-deprecated-declarations', '-Wl,-undefined,dynamic_lookup',
                                    str(TOTK / 'tests/dsu/native_motion.m'), '-framework', 'AppKit',
                                    '-framework', 'CoreHaptics', '-framework', 'GameController', '-framework', 'IOKit',
                                    '-o', str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_', 'BOTW_'))}
            run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'protocol'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(TOTK / 'tests/dsu/protocol.c'), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_udp_and_gamepad(self):
        with tempfile.TemporaryDirectory() as directory:
            exe, fixture = Path(directory) / 'probe', Path(directory) / 'packet'
            # Independent wire fixture, sealed with Python's standard CRC32.
            packet = bytearray(100)
            struct.pack_into('<4sHHIII', packet, 0, b'DSUS', 1001, 84, 0, 42, 0x100002)
            packet[20:24] = bytes([2, 2, 2, 2]); packet[31] = 1
            struct.pack_into('<I', packet, 32, 1)
            packet[37] = 0x20  # DSU A / south, mapped to Switch B
            packet[40:44] = bytes([255, 128, 128, 128])
            struct.pack_into('<Q6f', packet, 68, 1700000000000000, 0, 0, -1, 10, 20, 30)
            struct.pack_into('<I', packet, 8, zlib.crc32(packet)); fixture.write_bytes(packet)
            result = subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=undefined', '-fno-sanitize-recover=all',
                                     str(TOTK / 'tests/dsu/probe.m'), str(TOTK / 'runtime/input_prompts.m'),
                                     '-framework', 'CoreText', '-framework', 'CoreGraphics', '-framework', 'Foundation',
                                     '-framework', 'GameController', '-framework', 'IOKit', '-o', str(exe)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_', 'BOTW_'))}
            result = subprocess.run([str(exe), str(fixture), directory], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_settings_ui(self):
        from test_settings_panel import build_probe
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'settings'
            build = build_probe(shutil.which('clang'), exe)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_', 'BOTW_'))}
            result = subprocess.run([str(exe), 'dsu'], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shared_sources_build_for_ios(self):
        for source in ('gamepad.m', 'gamepad_feedback.m'):
            result = subprocess.run(['xcrun', '--sdk', 'iphonesimulator', 'clang', '-std=gnu11', '-fobjc-arc',
                                     '-target', 'arm64-apple-ios26.0-simulator', '-fsyntax-only',
                                     '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                                     str(TOTK / 'runtime' / source)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

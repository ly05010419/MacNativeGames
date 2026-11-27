"""The room around Ocarina of Time's screens in VR (vr.md row 4): the pure logic compiled and run natively
(zeldaVR/runtime/oot_vr_passthrough.h), the streamer's ALVR settings and packet (zeldaVR/streamer/basaltvr.patch, its new
AlvrPassthrough.swift compiled on its own with a small driver), and the wiring between the game, the link and the VR page.

The expected ALVR bytes are written out here from ALVR 20.14.0's own definitions, not from the Swift code: alvr/packets/src/lib.rs
ServerControlPacket (ReservedBuffer is variant 6) and RealTimeConfig { passthrough: Option<PassthroughMode>,
clientside_post_processing: Option<..> }, alvr/session/src/settings.rs PassthroughMode (RgbChromaKey is variant 1) and
RgbChromaKeyConfig { red, green, blue, distance_threshold: u8, feathering: f32 }, bincode 1.3.3 (little-endian, u32 variant
indices, u64 lengths, u8 option tags); the session form from alvr/session/src/lib.rs merge_from_json."""
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'zeldaVR/streamer/basaltvr.patch'
SWIFT_FILE = 'streamer/Sources/BasaltCore/AlvrPassthrough.swift'

DRIVER = r'''
import Foundation
func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }
let on = AlvrPassthrough(linkPayload: Data([1, 255, 0, 255, 64, 5, 0, 0]))!
let session = try! JSONSerialization.data(withJSONObject: ["session_settings": ["video": ["passthrough": on.sessionValue]]],
                                          options: [.sortedKeys])
print(String(data: session, encoding: .utf8)!)
print(hex(on.realTimeConfigPayload))
print(hex(AlvrPassthrough.off.realTimeConfigPayload))
print(AlvrPassthrough(linkPayload: Data([0, 255, 0, 255, 64, 5, 0, 0])) == AlvrPassthrough.off)
print(AlvrPassthrough(linkPayload: Data([2, 0, 0, 0, 64, 5, 0, 0])) == nil)
print(AlvrPassthrough(linkPayload: Data([1, 0, 0, 0, 0, 5, 0, 0])) == nil)
print(AlvrPassthrough(linkPayload: Data([1, 0, 0, 0, 64, 101, 0, 0])) == nil)
print(AlvrPassthrough(linkPayload: Data([1, 0, 0, 0, 64, 5, 0])) == nil)
print(AlvrPassthrough(linkPayload: Data([1, 0, 0, 0, 64, 5, 1, 0])) == nil)
let state = AlvrPassthroughState()
print(state.current == AlvrPassthrough.off, state.set(on), state.set(on), state.current == on)
'''


def new_file_from_patch(path):
    """The whole text of a file the patch adds (all its lines are '+' lines of one hunk)."""
    text = PATCH.read_text()
    start = text.index('diff --git a/%s b/%s' % (path, path))
    end = text.find('\ndiff --git ', start + 1)
    block = text[start:end if end >= 0 else len(text)]
    assert 'new file mode' in block, path
    body = block[block.index('\n@@') + 1:]
    lines = body.split('\n')[1:]
    return '\n'.join(l[1:] for l in lines if l.startswith('+')) + '\n'


def header_enum(name):
    src = (ROOT / 'zeldaVR/runtime/oot_vr_passthrough.h').read_text()
    return int(re.search(r'\b%s = (\d+)' % name, src).group(1))


class PureLogic(unittest.TestCase):
    def test_the_key_the_guard_and_the_steps(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'passthrough')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_passthrough.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class StreamerSettings(unittest.TestCase):
    def run_driver(self):
        if not shutil.which('xcrun') or subprocess.run(['xcrun', '-f', 'swiftc'], capture_output=True).returncode:
            self.skipTest('no swiftc')
        # Swift 6.4's driver stops (SIGTRAP, no message) when the working folder or TMPDIR has non-ASCII characters
        # (2026-11-27, /Volumes/开发): compile in an ASCII folder.
        base = tempfile.gettempdir()
        if not base.isascii():
            base = '/tmp'
        tmp = tempfile.mkdtemp(dir=base)
        try:
            (Path(tmp) / 'AlvrPassthrough.swift').write_text(new_file_from_patch(SWIFT_FILE))
            (Path(tmp) / 'main.swift').write_text(DRIVER)
            env = dict(os.environ, TMPDIR=tmp)
            subprocess.run(['xcrun', 'swiftc', 'AlvrPassthrough.swift', 'main.swift', '-o', 'driver'], cwd=tmp, env=env,
                           check=True, capture_output=True, text=True)
            return subprocess.run([str(Path(tmp) / 'driver')], cwd=tmp, check=True, capture_output=True, text=True).stdout.split('\n')
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_session_settings_and_real_time_config(self):
        out = self.run_driver()
        session = json.loads(out[0])
        pt = session['session_settings']['video']['passthrough']
        self.assertEqual(pt['enabled'], True)
        self.assertEqual(pt['content']['variant'], 'RgbChromaKey')
        key = pt['content']['RgbChromaKey']
        self.assertEqual({k: key[k] for k in ('red', 'green', 'blue', 'distance_threshold')},
                         {'red': 255, 'green': 0, 'blue': 255, 'distance_threshold': 64})
        self.assertAlmostEqual(key['feathering'], 0.05, places=6)
        self.assertEqual(set(key), {'red', 'green', 'blue', 'distance_threshold', 'feathering'})
        config = b'\x01' + struct.pack('<I', 1) + bytes([255, 0, 255, 64]) + struct.pack('<f', 0.05) + b'\x00'
        self.assertEqual(out[1], (struct.pack('<IQ', 6, len(config)) + config).hex())
        self.assertEqual(out[2], (struct.pack('<IQ', 6, 2) + b'\x00\x00').hex())
        self.assertEqual(out[3:9], ['true'] * 6)
        self.assertEqual(out[9], 'true true false true')

    def test_the_link_payload_is_the_games(self):
        """The C side's payload (oot_pt_payload) is what the Swift side parses in the driver above."""
        self.assertEqual([header_enum(n) for n in ('OOT_PT_KEY_R', 'OOT_PT_KEY_G', 'OOT_PT_KEY_B', 'OOT_PT_THRESHOLD',
                                                   'OOT_PT_FEATHER_PCT')], [255, 0, 255, 64, 5])

    def test_off_leaves_the_session_as_before(self):
        patch = PATCH.read_text()
        self.assertIn('if passthrough.chromaKey { video["passthrough"] = passthrough.sessionValue }', patch)


class Wiring(unittest.TestCase):
    def test_message_numbers_match(self):
        patch = PATCH.read_text()
        link = (ROOT / 'zeldaVR/gfx/totk_vr_link.c').read_text()
        self.assertIn('+        case passthroughOffer = 19', patch)
        self.assertIn('+        case passthrough = 101', patch)
        self.assertRegex(link, r'MSG_PASSTHROUGH_OFFER = 19,')
        self.assertRegex(link, r'MSG_PASSTHROUGH = 101,')
        self.assertIn('send(type: .passthroughOffer, payload: Data())', patch)
        self.assertIn('alvrVideo.sendPassthrough(passthrough)', patch)

    def test_off_by_default_and_drawn(self):
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_passthrough_draw.h').read_text()
        self.assertIn('vr_option("passthrough", 0)', draw)
        vr = (ROOT / 'zeldaVR/gfx/totk_vr.m').read_text()
        self.assertIn('#include "oot_vr_passthrough_draw.h"', vr)
        self.assertIn('eye_target = oot_pt_begin(g.img[im], pics, useds, &eye_clear, &pt_mode);', vr)
        self.assertIn('oot_pt_finish(cb, eye_target, g.img[im], pt_mode);', vr)
        rows = (ROOT / 'games/oot/runtime/settings_vr_window.h').read_text()
        self.assertIn('vr_oot_set_passthrough(v)', rows)
        self.assertIn('void vr_oot_set_passthrough(int v)', draw)


if __name__ == '__main__':
    unittest.main()

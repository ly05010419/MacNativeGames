"""The stepping legs of Ocarina of Time's VR first person: the packaging-time generator (games/oot/scripts/make_vr_gait.py) on a
made-up skeleton and made-up animations (no game data), and the runtime's reader and blend (zeldaVR/runtime/oot_vr_gait.h)
compiled and run natively on what it wrote."""
import importlib.util
import math
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('make_vr_gait', ROOT / 'games/oot/scripts/make_vr_gait.py')
gait = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gait)

# (name, parent, translation): Spine_1 hangs from Skl_Root and is not posed; Waist and below are
BONES = (('Root', -1, (0, 0, 0)), ('Skl_Root', 0, (0, 0.5, 0)), ('Spine_1', 1, (0, 0.1, 0)), ('Waist', 1, (0, 0, 0)),
         ('Leg_1_L', 3, (0.1, 0, 0)), ('Ankle_L', 4, (0, -0.4, 0)))
TRAVEL = {'Move_Walk': (0, 1), 'Move_Jog': (0, 2), 'Move_Run': (0, 3), 'LockOn_Walk_L': (1, 0), 'LockOn_Walk_R': (-1, 0),
          'LockOn_Run_L': (2, 0), 'LockOn_Run_R': (-2, 0), 'LockOn_Run_LF': (1.4, 1.4), 'LockOn_Run_RF': (-1.4, 1.4),
          'LockOn_Run_LB': (1.4, -1.4), 'LockOn_Run_RB': (-1.4, -1.4), 'LockOn_Run_BL': (0, -2), 'LockOn_Run_BR': (0, -2)}


class Blob:
    def __init__(self, head=b''):
        self.data = bytearray(head)

    def put(self, raw):
        while len(self.data) % 8:
            self.data.append(0)
        at = len(self.data)
        self.data += raw
        return at

    def name(self, text):
        raw = text.encode()
        return self.put(struct.pack('<H', len(raw)) + raw + b'\0')


def skeleton():
    b = Blob(b'FRES    ' + bytes(0xb8))
    names = [b.name(n) for n, _, _ in BONES]
    table = b''
    for i, ((_, parent, t), at) in enumerate(zip(BONES, names)):
        entry = bytearray(0x58)
        struct.pack_into('<Q', entry, 0, at)
        struct.pack_into('<Hh', entry, 0x20, i, parent)
        struct.pack_into('<3f4f3f', entry, 0x30, 1, 1, 1, 0, 0, 0, 0, *t)   # (rotation: 4 floats, the 4th unused)
        table += entry
    bones = b.put(table)
    fskl = b.put(b'FSKL' + bytes(12) + struct.pack('<Q', bones) + bytes(0x38 - 0x18) + struct.pack('<H', len(BONES)) + bytes(6))
    fmdl = b.put(b'FMDL' + bytes(0x14) + struct.pack('<Q', fskl) + bytes(0x50))
    struct.pack_into('<Q', b.data, 0x28, fmdl)
    return bytes(b.data)


def curve(b, target, frames, keys, cubic):
    """A curve's entry: frames as bytes; cubic: four floats a key, else 16-bit Hermite keys (value, slope) scaled by 1/1000"""
    f = b.put(bytes(frames))
    if cubic:
        k = b.put(b''.join(struct.pack('<4f', *c) for c in keys))
        flags = 0x4080 | 2
    else:
        k = b.put(struct.pack('<2f', 0.001, -1.0) + b''.join(struct.pack('<2H', round(v * 1000), round((s + 1) * 1000)) for v, s in keys))
        flags = 0x4800 | (1 << 2) | 2
    entry = bytearray(0x30)
    struct.pack_into('<QQHHI2f', entry, 0, f, k, flags, len(frames), target, frames[0], frames[-1])
    struct.pack_into('<2f', entry, 0x24, 0.001 if not cubic else 1.0, 0.0)
    return bytes(entry)


def animations(clips):
    """An animation file with one FSKA per clip: (name, frames, travel x, travel z, lift): Root moves by the travel over the
    cycle (cubic), the left leg swings up `lift` radians at mid-cycle and back (Hermite), so the foot is planted at its start"""
    b = Blob(b'FRES    ')
    for name, frames, tx, tz, lift in clips:
        root_curves = curve(b, 16, [0, frames], [(0, tx, 0, 0), (tx, 0, 0, 0)], True) + curve(b, 24, [0, frames], [(0, tz, 0, 0), (tz, 0, 0, 0)], True)
        leg_curves = curve(b, 32, [0, frames // 2, frames], [(0, 0), (lift, 0), (0, 0)], False)
        entries = b''
        for bone, curves, n in (('Root', root_curves, 2), ('Leg_1_L', leg_curves, 1)):
            cv = b.put(curves)
            base = b.put(struct.pack('<3f4f3f', 1, 1, 1, 0, 0, 0, 0, *dict((x[0], x[2]) for x in BONES)[bone]))
            entry = bytearray(0x38)
            struct.pack_into('<QQQ', entry, 0, b.name(bone), cv, base)
            struct.pack_into('<I', entry, 0x28, 0x38)
            entry[0x2e] = n
            entries += entry
        table = b.put(entries)
        head = bytearray(0x50)
        head[0:4] = b'FSKA'
        struct.pack_into('<Q', head, 8, b.name(name))
        struct.pack_into('<Q', head, 0x28, table)
        struct.pack_into('<i', head, 0x40, frames)
        struct.pack_into('<I', head, 0x4c, 2)
        b.put(bytes(head))
    return bytes(b.data)


class Gait(unittest.TestCase):
    def test_stance_is_the_middle_of_the_planted_stretch(self):
        n = 64
        heights = [0.0 if (k < 16 or k >= 48) else 0.2 for k in range(n)]   # planted from 48 round to 15: its middle is at 0
        self.assertAlmostEqual(min(gait.stance(heights), 1 - gait.stance(heights)), 0.0, delta=1.5 / n)
        heights = [0.2 if k < 32 else 0.0 for k in range(n)]                  # planted 32..63: its middle 47.5 / 64
        self.assertAlmostEqual(gait.stance(heights), 47.5 / 64, delta=1 / n)
        self.assertEqual(gait.stance([0.1] * 8), 0.0)

    def test_quaternions_of_the_skeletons_rotation_order(self):
        for euler in ((0.3, 0, 0), (0, -0.7, 0), (0, 0, 1.2), (0.4, -0.5, 2.9)):
            r, q = gait.rotation(euler), gait.quaternion(gait.rotation(euler))
            x, y, z, w = q
            back = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
            for i in range(3):
                for j in range(3):
                    self.assertAlmostEqual(r[i][j], back[i][j], places=5)
            self.assertGreaterEqual(w, 0)

    def test_refuses_a_missing_clip_and_a_skeleton_without_a_waist(self):
        clips = [(n, 8, tx, tz, 0.8) for n, (tx, tz) in TRAVEL.items()][:-1] + [('Wait', 1, 0, 0, 0)]
        with self.assertRaisesRegex(ValueError, 'no animation named LockOn_Run_BR'):
            gait.make(skeleton(), animations(clips), animations(clips))
        with self.assertRaisesRegex(ValueError, 'not a FRES'):
            gait.make(b'NOPE' * 64, b'', b'')

    def test_writes_what_the_runtime_reads(self):
        clips = [(n, 8, tx, tz, 0.8) for n, (tx, tz) in TRAVEL.items()] + [('Wait', 200, 0, 0, 0)]
        data = gait.make(skeleton(), animations(clips), animations(clips))
        tmp = tempfile.mkdtemp()
        try:
            path, exe = Path(tmp) / 'gait.bin', str(Path(tmp) / 'gait')
            path.write_bytes(data)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', str(ROOT / 'tests/test_oot_vr_gait.c'), '-o', exe], check=True, capture_output=True, text=True)
            run = subprocess.run([exe, str(path)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('gait checks passed', run.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()

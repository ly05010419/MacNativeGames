#!/usr/bin/env python3
"""Link's own stepping cycles for the VR first-person legs, from the player's own animation files (zeldaVR/runtime/oot_vr_gait.h).

In first person the legs face where the player looks, while the game runs Link toward the stick. With the game's own forward
run the legs ran ahead while the body slid sideways; the game also has side, diagonal and back steps (its lock-on cycles), and
with those the renderer can step the way Link really travels. Which clips and why follow HookshotVR's notes (GPL-3: facts
only); the reader and the file layout are ours.

Written per age (gait-child.bin, gait-adult.bin), next to the models make_vr_rig.py writes, never into the checkout:
  the posed bones (Skl_Root, then Waist and everything under it, parents first) by name;
  per clip: its frame count, how far its Root travels in one cycle (the root motion, in the model's x and z), where in the cycle
  the left foot is mid-stance (0..1), and its samples (every STEP-th frame);
  per sample and bone: the local rotation as a quaternion, the translation and the scale.

FSKA layout (BFRES 10.4, measured on the player's files): header +0x08 name, +0x28 bone animations (0x38 each), +0x40 frame
count, +0x4c bone animation count. A bone animation: +0x00 name, +0x08 curves (0x30 each), +0x10 base values (scale 3 floats,
rotation 4 (Euler XYZ radians, the 4th unused), translation 3, each only when its bit 3, 4, 5 of +0x28 is set), +0x2e curve
count. A curve: +0x00 frames, +0x08 keys, +0x10 flags (low 2 bits: frames as float, 16-bit /32 or byte), +0x12 key count,
+0x14 which value (byte offset: 4..12 scale, 16..24 translation, 32..40 rotation), +0x24 scale, +0x28 offset. Keys: 0x408x
cubic (4 floats a key, u 0..1 over the segment), 0x48xx quantised Hermite (step and lowest slope as floats, then value and
slope per key, 16-bit for key type 1, 8-bit for 2).
"""
import math
import struct

MAGIC = b'OOTGAIT1'
STEP = 2   # one sample every second frame; the runtime interpolates
GROUND = 0.015   # (m) an ankle this close to its lowest counts as planted
FILES = {   # age: (model written by make_vr_rig.py, its moving clips' file, its standing clip's file)
    'child': ('Player_Child', 'Model/Player_Child_02_Animation.anim.bfres.zs', 'Model/Player_Child_04_Animation.anim.bfres.zs'),
    'adult': ('Player_Adult', 'Model/Player_Adult_02_Animation.anim.bfres.zs', 'Model/Player_Adult_04_Animation.anim.bfres.zs')}
MOVING = ('Move_Walk', 'Move_Jog', 'Move_Run', 'LockOn_Walk_L', 'LockOn_Walk_R', 'LockOn_Run_L', 'LockOn_Run_R',
          'LockOn_Run_LF', 'LockOn_Run_RF', 'LockOn_Run_LB', 'LockOn_Run_RB', 'LockOn_Run_BL', 'LockOn_Run_BR')
STANDING = 'Wait'   # its first frame, held
TARGETS = {4: ('s', 0), 8: ('s', 1), 12: ('s', 2), 16: ('t', 0), 20: ('t', 1), 24: ('t', 2), 32: ('r', 0), 36: ('r', 1), 40: ('r', 2)}


def text(data, at):
    n = struct.unpack_from('<H', data, at)[0]
    return data[at + 2:at + 2 + n].decode()


def skeleton(model):
    """[(name, parent, scale, rotation, translation)] of the FRES model's skeleton (the layout oot_bfres.h reads)"""
    if model[:8] != b'FRES    ':
        raise ValueError('not a FRES model')
    fmdl = struct.unpack_from('<Q', model, 0x28)[0]
    fskl = struct.unpack_from('<Q', model, fmdl + 0x18)[0]
    if model[fskl:fskl + 4] != b'FSKL':
        raise ValueError('the model has no skeleton')
    table, count = struct.unpack_from('<Q', model, fskl + 0x10)[0], struct.unpack_from('<H', model, fskl + 0x38)[0]
    out = []
    for i in range(count):
        b = table + i * 0x58
        out.append((text(model, struct.unpack_from('<Q', model, b)[0]), struct.unpack_from('<h', model, b + 0x22)[0],
                    struct.unpack_from('<3f', model, b + 0x30), struct.unpack_from('<3f', model, b + 0x3c),
                    struct.unpack_from('<3f', model, b + 0x4c)))
    return out


class Curve:
    def __init__(self, data, at):
        frames, keys = struct.unpack_from('<QQ', data, at)
        self.flags, count, target = struct.unpack_from('<HHI', data, at + 0x10)
        scale, offset = struct.unpack_from('<2f', data, at + 0x24)
        if target not in TARGETS:
            raise ValueError('a curve writes value %d, not a scale, rotation or translation' % target)
        self.kind, self.axis = TARGETS[target]
        layout = {0: ('<f', 4, 1.0), 1: ('<h', 2, 1 / 32), 2: ('<B', 1, 1.0)}.get(self.flags & 3)
        self.frames = [struct.unpack_from(layout[0], data, frames + i * layout[1])[0] * layout[2] for i in range(count)]
        if self.flags & 0xfffc == 0x4080:
            self.cubic = True
            self.keys = [struct.unpack_from('<4f', data, keys + 16 * i) for i in range(count)]
        elif self.flags & 0xff00 == 0x4800 and (self.flags >> 2) & 3 in (1, 2):
            self.cubic = False
            step, lowest = struct.unpack_from('<2f', data, keys)
            size, form = (2, '<H') if (self.flags >> 2) & 3 == 1 else (1, '<B')
            raw = [struct.unpack_from(form, data, keys + 8 + i * size)[0] for i in range(2 * count)]
            self.keys = [(raw[2 * i] * scale + offset, raw[2 * i + 1] * step + lowest) for i in range(count)]
        else:
            raise ValueError('an animation curve encoded as %#x' % self.flags)
        if not self.frames:
            raise ValueError('an animation curve without keys')

    def at(self, t):
        f, k = self.frames, self.keys
        if t >= f[-1]:
            return sum(k[-1]) if self.cubic else k[-1][0]
        i = 0
        while i + 1 < len(f) and f[i + 1] <= t:
            i += 1
        j = min(i + 1, len(f) - 1)
        h = f[j] - f[i]
        u = min(1.0, max(0.0, (t - f[i]) / h)) if h > 0 else 0.0
        if self.cubic:
            c = k[i]
            return c[0] + u * (c[1] + u * (c[2] + u * c[3]))
        (v0, m0), (v1, m1) = k[i], k[j]
        return (2 * u ** 3 - 3 * u * u + 1) * v0 + (u ** 3 - 2 * u * u + u) * h * m0 + (3 * u * u - 2 * u ** 3) * v1 + (u ** 3 - u * u) * h * m1


class Clip:
    def __init__(self, data, at):
        self.name = text(data, struct.unpack_from('<Q', data, at + 8)[0])
        self.frames = struct.unpack_from('<i', data, at + 0x40)[0]
        table, count = struct.unpack_from('<Q', data, at + 0x28)[0], struct.unpack_from('<I', data, at + 0x4c)[0]
        self.bones = {}
        for i in range(count):
            b = table + i * 0x38
            curves, base = struct.unpack_from('<QQ', data, b + 8)
            bits = struct.unpack_from('<I', data, b + 0x28)[0]
            values = {}
            for bit, kind, n in ((3, 's', 3), (4, 'r', 4), (5, 't', 3)):
                if bits >> bit & 1:
                    values[kind] = list(struct.unpack_from('<%df' % n, data, base))[:3]
                    base += 4 * n
            self.bones[text(data, struct.unpack_from('<Q', data, b)[0])] = (values, [Curve(data, curves + 0x30 * c) for c in range(data[b + 0x2e])])

    def local(self, name, rest, t):
        """(scale, rotation, translation) of a bone at frame t: the clip's where it has them, the skeleton's elsewhere"""
        s, r, p = list(rest[0]), list(rest[1]), list(rest[2])
        if name in self.bones:
            values, curves = self.bones[name]
            s, r, p = values.get('s', s), values.get('r', r), values.get('t', p)
            s, r, p = list(s), list(r), list(p)
            for c in curves:
                {'s': s, 'r': r, 't': p}[c.kind][c.axis] = c.at(t)
        return s, r, p


def clips(data, names):
    found, at = {}, 0
    while True:
        at = data.find(b'FSKA', at)
        if at < 0:
            break
        name = text(data, struct.unpack_from('<Q', data, at + 8)[0])
        if name in names and name not in found:
            found[name] = Clip(data, at)
        at += 4
    missing = [n for n in names if n not in found]
    if missing:
        raise ValueError('no animation named ' + ', '.join(missing))
    return found


def rotation(euler):
    """R = Rz Ry Rx, as oot_bfres.h builds a bone"""
    cx, sx, cy, sy, cz, sz = (math.cos(euler[0]), math.sin(euler[0]), math.cos(euler[1]), math.sin(euler[1]),
                              math.cos(euler[2]), math.sin(euler[2]))
    return [[cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx], [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
            [-sy, cy * sx, cy * cx]]


def quaternion(m):
    """(x, y, z, w) of a rotation matrix, w >= 0"""
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = 2 * math.sqrt(1 + trace)
        q = [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4]
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = 2 * math.sqrt(1 + m[0][0] - m[1][1] - m[2][2])
        q = [s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s]
    elif m[1][1] > m[2][2]:
        s = 2 * math.sqrt(1 + m[1][1] - m[0][0] - m[2][2])
        q = [(m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s]
    else:
        s = 2 * math.sqrt(1 + m[2][2] - m[0][0] - m[1][1])
        q = [(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s]
    if q[3] < 0:
        q = [-v for v in q]
    n = math.sqrt(sum(v * v for v in q))
    return [v / n for v in q]


def lower_body(bones):
    """Indices of Skl_Root and of Waist and everything under it, parents before children"""
    by = {b[0]: i for i, b in enumerate(bones)}
    if 'Skl_Root' not in by or 'Waist' not in by or bones[by['Waist']][1] != by['Skl_Root']:
        raise ValueError('the skeleton has no Waist under Skl_Root')
    order = [by['Skl_Root'], by['Waist']]
    for i, b in enumerate(bones):   # (parents come before children in the file: oot_bfres.h checks it)
        if b[1] in order[1:]:
            order.append(i)
    return order


def ankle_heights(bones, order, clip, frames):
    """Height of the left ankle above the model's origin at each frame, the root motion left out"""
    at = {b: k for k, b in enumerate(order)}
    ankle = next(i for i, b in enumerate(bones) if b[0] == 'Ankle_L')
    chain = []
    i = ankle
    while i in at:
        chain.append(i)
        i = bones[i][1]
    out = []
    for f in frames:
        r, p = [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.0, 0.0, 0.0]
        for i in reversed(chain):
            _, lr, lp = clip.local(bones[i][0], bones[i][2:], f)
            m = rotation(lr)
            p = [p[a] + sum(r[a][c] * lp[c] for c in range(3)) for a in range(3)]
            r = [[sum(r[a][c] * m[c][e] for c in range(3)) for e in range(3)] for a in range(3)]
        out.append(p[1])
    return out


def stance(heights):
    """Where in the cycle (0..1) the left foot is mid-stance: the middle of its longest planted stretch"""
    n = len(heights)
    low = min(heights)
    down = [h < low + GROUND for h in heights]
    if all(down) or not any(down):
        return 0.0
    start = down.index(False)
    best, best_at, run, run_at = 0, 0, 0, 0
    for k in range(1, n + 1):
        if down[(start + k) % n]:
            run += 1
            if run > best:
                best, best_at = run, run_at
        else:
            run, run_at = 0, k
    return ((start + best_at + 1 + (best - 1) / 2) / n) % 1.0


def make(model, moving, standing):
    """The gait file's bytes from a decompressed Player_* model and its two animation files"""
    bones = skeleton(model)
    order = lower_body(bones)
    if len(order) > 255:
        raise ValueError('too many lower-body bones')
    found = clips(moving, MOVING)
    found.update(clips(standing, [STANDING]))
    rows, table = [], []
    for name in MOVING + (STANDING,):
        clip = found[name]
        held = name == STANDING
        if clip.frames <= 0 or (not held and clip.frames % STEP):
            raise ValueError('%s: %d frames' % (name, clip.frames))
        frames = [0.0] if held else [float(f) for f in range(0, clip.frames, STEP)]
        first = len(rows)
        for f in frames:
            row = []
            for i in order:
                s, r, p = clip.local(bones[i][0], bones[i][2:], f)
                row.extend(quaternion(rotation(r)) + list(p) + list(s))
            if not all(math.isfinite(v) for v in row):
                raise ValueError('%s: a value is not finite at frame %g' % (name, f))
            rows.append(row)
        travel = [0.0, 0.0]
        if not held and 'Root' in clip.bones:
            rest = next(b[2:] for b in bones if b[0] == 'Root')
            a, b = clip.local('Root', rest, 0.0)[2], clip.local('Root', rest, float(clip.frames))[2]
            travel = [b[0] - a[0], b[2] - a[2]]
        phase = 0.0 if held else stance(ankle_heights(bones, order, clip, [clip.frames * k / 64 for k in range(64)]))
        table.append((name, clip.frames, first, len(rows) - first, travel[0], travel[1], phase))
    out = bytearray(MAGIC + struct.pack('<5I', 1, len(order), len(table), len(rows), STEP))
    index = {b: k for k, b in enumerate(order)}
    for k, i in enumerate(order):
        name = bones[i][0].encode()
        if len(name) >= 48:
            raise ValueError('a bone name is too long: ' + bones[i][0])
        out += name.ljust(48, b'\0') + struct.pack('<hxx', -1 if k == 0 else index[bones[i][1]])
    for name, frames, first, count, tx, tz, phase in table:
        out += name.encode().ljust(32, b'\0') + struct.pack('<3I3f', frames, first, count, tx, tz, phase)
    for row in rows:
        out += struct.pack('<%df' % len(row), *row)
    return bytes(out)

"""Super Smash Bros. Ultimate's shader seed reads data.arc: BNSH files and SSBH shader files inside Zstandard frames."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from compression import zstd

GAME = Path(__file__).resolve().parents[1]   # games/ssbu
SCRIPT = GAME/'scripts/prepare_shader_seed.py'
spec = importlib.util.spec_from_file_location('prepare_shader_seed', SCRIPT)
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def control(stage, length, size=0x850):
    c = bytearray(size)
    struct.pack_into('<I', c, 0x20, size)
    struct.pack_into('<II', c, 0x700, 0x30, length)
    struct.pack_into('<I', c, 0x714, stage)
    return bytes(c)


def code(length, fill):
    return bytes((fill+i) & 0xff for i in range(length))


def shdr(blobs):
    """An SSBH shader file: the table at +0x28 (one 0x38-byte entry per blob), names, then the blobs."""
    table, names_at = 0x28, 0x28+0x38*len(blobs)
    out = bytearray(names_at+0x10*len(blobs))
    out[:4] = b'HBSS'; out[0x10:0x14] = b'RDHS'
    struct.pack_into('<QQ', out, 0x18, table-0x18, len(blobs))
    for i, blob in enumerate(blobs):
        e, name = table+0x38*i, names_at+0x10*i
        out[name:name+5] = b'SH_%02d' % i
        start = len(out)
        out += blob
        struct.pack_into('<QII', out, e, name-e, 0, 2)
        struct.pack_into('<QQQ', out, e+0x10, start-(e+0x10), len(blob), len(blob))
    return bytes(out)


def binary(ctl, body):
    """An NVN shader binary as Smash stores it: a 0x2d0-byte header (magic 0x19866891), the control, the code."""
    head = bytearray(0x2d0)
    struct.pack_into('<I', head, 0, 0x19866891)
    return bytes(head)+ctl+body


def bnsh(stage_code):
    """A BNSH file with one program of a vertex and a fragment stage (0x880-byte controls, as the effects carry)."""
    b = bytearray(0x3000)
    b[:4] = b'BNSH'; b[12:14] = b'\xff\xfe'; b[96:100] = b'grsc'
    struct.pack_into('<I', b, 28, len(b))
    struct.pack_into('<IQ', b, 124, 1, 0xc0)
    struct.pack_into('<Q', b, 0xc0+16, 0x100)
    for i, (stage, rel) in enumerate([(0, 8), (1, 40)]):
        header, ctl_at, code_at = 0x200+i*64, 0x400+i*0x880, 0x2000+i*0x100
        struct.pack_into('<Q', b, 0x100+rel, header)
        struct.pack_into('<QQII', b, header+8, ctl_at, code_at, 0x100, 0x880)
        b[ctl_at:ctl_at+0x880] = control(stage, 0x100, 0x880)
        b[code_at:code_at+0x100] = stage_code[i]
    return bytes(b)


def key(ctl, body):
    return hashlib.sha256(ctl[:seed.control_key_len(ctl)]+body).hexdigest()


class SsbuShaderSeed(unittest.TestCase):
    def test_chain_is_the_runtime_s(self):
        header = (GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
        self.assertIn(f'#define DMSH_CHAIN "{seed.CHAIN}"', header)
        self.assertEqual(seed.CHAIN, 'totk-native-v7-ssbu-offsets')

    def test_shdr_controls_are_keyed_as_the_runtime_keys_them(self):
        vs, ps = control(0, 0x200), control(1, 0x180)
        vcode, pcode = code(0x200, 7), code(0x180, 99)
        bad = control(2, 0x100)   # a geometry stage: no translation chain
        data = shdr([binary(vs, vcode), binary(ps, pcode), binary(bad, code(0x100, 1))])
        found, refused = seed.shdr_shaders(data, 0x10)
        self.assertEqual({(k, s) for k, s, _, _ in found}, {(key(vs, vcode), 'vs'), (key(ps, pcode), 'ps')})
        self.assertEqual(refused, 1)
        self.assertEqual(seed.control_key_len(vs), 0x850)

    def test_code_running_past_its_buffer_is_refused(self):
        ctl = control(0, 0x400)
        found, refused = seed.shdr_shaders(shdr([binary(ctl, code(0x100, 3))]), 0x10)
        self.assertEqual((found, refused), ([], 1))

    def test_archive_walk_finds_both_containers_and_skips_false_frames(self):
        vs = control(0, 0x200)
        vcode = code(0x200, 5)
        effect_codes = [code(0x100, 11), code(0x100, 22)]
        frames = [zstd.compress(shdr([binary(vs, vcode)])), zstd.compress(b'EFFN'+bytes(60)+bnsh(effect_codes))]
        # A magic that is not a frame (it does not decode), between real ones, and data before the first.
        image = bytes(100)+seed.ZSTD+b'not a frame'*4+frames[0]+bytes(17)+frames[1]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'data.arc'
            path.write_bytes(image)
            shaders, spans, raw, counts = seed.arc_part((str(path), 0, len(image), 0, len(image), 'ssbu'))
        self.assertEqual(counts['frames'], 2)
        self.assertEqual(counts['refused'], 0)
        self.assertEqual(len(spans), 2)
        self.assertEqual(spans[0], (image.index(frames[0]), len(frames[0])))
        keys = {row[0] for _, _, _, rows in shaders for row in rows}
        effect = {key(control(i, 0x100, 0x880), effect_codes[i]) for i in range(2)}
        self.assertEqual(keys, {key(vs, vcode)} | effect)
        self.assertEqual(raw, [])   # the BNSH is inside a frame, not stored as it is

    def test_depth_mode_is_the_game_s(self):
        self.assertIs(seed.GAMES['ssbu']['depth_minus_w'], False)
        with tempfile.TemporaryDirectory() as tmp:
            ctl, body = control(0, 0x100), code(0x100, 1)
            request = seed.translation_request('v', 'vs', ctl, body, seed.ANSWERS, Path(tmp)/'v.code', False)
            self.assertIs(request['depth_minus_w'], False)
            self.assertEqual((Path(tmp)/'v.code').read_bytes(), body[0x30:])


if __name__ == '__main__':
    unittest.main()

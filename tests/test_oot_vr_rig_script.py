"""games/oot/scripts/make_vr_rig.py on a made-up RomFS (no game data): dictionary lookup, refusals, unchanged files."""
import compression.zstd as zstd
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('make_vr_rig', ROOT / 'games/oot/scripts/make_vr_rig.py')
rig = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rig)


def sarc(members):
    names = b''
    offsets = []
    for name in members:
        offsets.append(len(names) // 4)
        raw = name.encode() + b'\0'
        names += raw + b'\0' * (-len(raw) % 4)
    header = 0x14
    sfat = header + 12 + 16 * len(members)
    start = sfat + 8 + len(names)
    body = b''
    entries = b''
    for offset, data in zip(offsets, members.values()):
        entries += struct.pack('<4I', 0, 0x01000000 | offset, len(body), len(body) + len(data))
        body += data
    size = start + len(body)
    out = b'SARC' + struct.pack('<HH', header, 0xfeff) + struct.pack('<II', size, start) + struct.pack('<HH', 0x100, 0)
    out += b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + entries
    out += b'SFNT' + struct.pack('<HH', 8, 0) + names + body
    return out


class VrRig(unittest.TestCase):
    def make(self, folder, model_bytes=b'FRES' + bytes(range(200)) * 40, dictionary=True):
        samples = [bytes([i % 256]) * 64 + b'FRES common model bytes ' * 8 + bytes(range(i % 200)) for i in range(400)]
        trained = zstd.train_dict(samples, 4096)
        pack = zstd.compress(sarc({'zs.zsdic': trained.dict_content}))
        models = {}
        for name, path in rig.MODELS.items():
            models[path] = zstd.compress(model_bytes + name.encode(), zstd_dict=trained if dictionary else zstd.ZstdDict(zstd.train_dict(
                [b'x' + bytes([i % 256]) * 80 + bytes(range(i % 100)) for i in range(300)], 2048).dict_content))
        image, index = b'', []
        for path, raw in [(rig.DICTIONARIES, pack), *models.items()]:
            index.append({'path': path, 'offset': len(image), 'size': len(raw)})
            image += raw
        (folder / 'romfs.bin').write_bytes(image)
        (folder / 'index.json').write_text(json.dumps({'files': index}))
        return model_bytes

    def test_writes_both_models_and_keeps_unchanged_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            model = self.make(tmp)
            out = tmp / 'rig'
            self.assertEqual(rig.generate(tmp, out), len(rig.MODELS))
            self.assertEqual((out / 'Player_Child.bfres').read_bytes(), model + b'Player_Child')
            self.assertEqual((out / 'Player_Adult.bfres').read_bytes(), model + b'Player_Adult')
            before = os.stat(out / 'Player_Child.bfres').st_mtime_ns
            self.assertEqual(rig.generate(tmp, out), 0)
            self.assertEqual(os.stat(out / 'Player_Child.bfres').st_mtime_ns, before)

    def test_refuses_what_is_not_a_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            self.make(tmp, model_bytes=b'NOPE' + bytes(range(200)) * 40)
            with self.assertRaisesRegex(ValueError, 'not a FRES'):
                rig.generate(tmp, tmp / 'rig')

    def test_refuses_a_missing_dictionary(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            self.make(tmp, dictionary=False)
            with self.assertRaisesRegex(ValueError, 'dictionary'):
                rig.generate(tmp, tmp / 'rig')

    def test_stays_out_of_the_checkout(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'outside the source checkout'):
                rig.generate(tmp, ROOT / 'games/oot/vr-rig')


if __name__ == '__main__':
    unittest.main()

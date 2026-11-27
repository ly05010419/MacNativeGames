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

    def test_what_he_wears_is_optional(self):
        """A game without the worn models or the animations: each is named and left out, nothing fails; one present is written."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            model = self.make(tmp)
            out = tmp / 'rig'
            rig.generate(tmp, out)
            written, missing = rig.generate_optional(tmp, out)
            self.assertEqual(written, 0)
            self.assertIn('Armor_Adult_Goron', missing)
            self.assertIn('Item_BombBag_Small', missing)
            self.assertTrue(any(m.startswith('gait-child') for m in missing) and any(m.startswith('gait-adult') for m in missing))
            self.assertFalse(list(out.glob('gait-*.bin')))
            # one of them in the RomFS: written like the others
            image = (tmp / 'romfs.bin').read_bytes()
            index = json.loads((tmp / 'index.json').read_text())
            raw = zstd.compress(model + b'tunic')   # (no dictionary: the frame says none, and none is in the pack)
            path = rig.model_path('Armor_Adult_Goron')[0]
            index['files'].append({'path': path, 'offset': len(image), 'size': len(raw)})
            (tmp / 'romfs.bin').write_bytes(image + raw)
            (tmp / 'index.json').write_text(json.dumps(index))
            with self.assertRaisesRegex(ValueError, 'dictionary'):
                rig.generate_optional(tmp, out)

    def test_the_optional_model_names(self):
        self.assertEqual(rig.model_path('Item_Bottle.Item_Bottle_Pouch'), ('Model/Item_Bottle.Item_Bottle_Pouch.bfres.zs', 'Item_Bottle_Pouch'))
        self.assertEqual(rig.model_path('Armor_Adult_Zora'), ('Model/Armor_Adult_Zora.Armor_Adult_Zora.bfres.zs', 'Armor_Adult_Zora'))
        self.assertEqual(len(rig.OPTIONAL), len(rig.WORN) + len(rig.CARRIED) + len(rig.OWN))

    def test_stays_out_of_the_checkout(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'outside the source checkout'):
                rig.generate(tmp, ROOT / 'games/oot/vr-rig')
            with self.assertRaisesRegex(ValueError, 'outside the source checkout'):
                rig.generate_optional(tmp, ROOT / 'games/oot/vr-rig')


if __name__ == '__main__':
    unittest.main()

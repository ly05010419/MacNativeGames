"""Synthetic HUD fingerprints and custom keyboard icons; no game assets."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
from unittest.mock import patch
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import prompt_assets as assets
from tools.builder.keyboard_layout import parse_layout


def fake_bntx(size):
    data = bytearray()
    expected = []
    for number, (name, cp) in enumerate(assets.HUD_TEXTURES.items()):
        at = len(data)
        data.extend(bytes(0x80))
        data[at:at + 4] = b'BRTI'
        struct.pack_into('<H', data, at + 0x16, 1)
        struct.pack_into('<I', data, at + 0x1C, 0x201)
        struct.pack_into('<II', data, at + 0x24, size, size)
        struct.pack_into('<I', data, at + 0x34, 3)
        name_at = len(data)
        data.extend(struct.pack('<H', len(name)) + name.encode())
        struct.pack_into('<Q', data, at + 0x60, name_at)
        pointer = len(data)
        data.extend(bytes(8))
        struct.pack_into('<Q', data, at + 0x70, pointer)
        start = len(data)
        struct.pack_into('<Q', data, pointer, start)
        data.extend(bytes(((size + 63) // 64) * 512 * 8 * ((size + 63) // 64)))
        linear = bytes(((x * 7 + y * 11 + number) & 255) for y in range(size) for x in range(size))
        for y in range(size):
            for x in range(size):
                data[start + assets.gob_offset(x, y, (size + 63) // 64, 8)] = linear[y * size + x]
        expected.append((cp, size, size, hashlib.sha256(linear).hexdigest()))
    return bytes(data), expected


class PromptAssets(unittest.TestCase):
    def test_fingerprints_match_linear_pixels_at_supported_sizes(self):
        for size in (72, 108):
            data, expected = fake_bntx(size)
            self.assertEqual(assets.hud_fingerprints(data), expected)

    def test_missing_masks_are_rejected(self):
        with self.assertRaises(ValueError):
            assets.hud_fingerprints(b'')

    def test_packaging_uses_cache_or_installed_app(self):
        import build_app
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tree, staged, installed, logs = (root / name for name in ('tree', 'staged.app', 'installed.app', 'logs'))
            with patch.object(build_app, 'run') as run:
                build_app.prepare_prompt_assets(tree, staged, 'v121', logs)
                run.assert_not_called()
                build_app.prepare_prompt_assets(tree, staged, 'v143', logs, prompt_app=installed)
                command = run.call_args.args[0]
                self.assertEqual(command[command.index('--app') + 1], installed)
                self.assertEqual(command[-1], staged / 'Contents/Resources/totk/build/prompt-hud.tsv')
                romfs = tree / 'extracted/v143/romfs'
                romfs.mkdir(parents=True)
                (romfs / 'romfs.bin').touch()
                build_app.prepare_prompt_assets(tree, staged, 'v143', logs)
                command = run.call_args.args[0]
                self.assertEqual(command[command.index('--romfs') + 1], romfs)

    def test_preparation_reads_a_synthetic_romfs(self):
        import compression.zstd as zstd
        import prepare_prompt_hud
        from test_mod_manifest import fake_romfs, sarc
        data, expected = fake_bntx(108)
        archive = sarc({'timg/__Combined.bntx': data})
        files = {'Pack/ZsDic.pack.zs': zstd.compress(sarc({})),
                 'UI/LayoutArchive/Common.Test.blarc.zs': zstd.compress(archive)}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            romfs = fake_romfs(root / 'romfs', files)
            output = root / 'build/prompt-hud.tsv'
            self.assertEqual(prepare_prompt_hud.prepare(None, 'v143', output, romfs), 8)
            self.assertEqual(output.read_text(), ''.join(f'{cp:x}\t{w}\t{h}\t{sha}\n' for cp, w, h, sha in expected))
            self.assertFalse(output.with_suffix('.tmp').exists())

    def test_binding_errors_match_runtime_rules(self):
        for text in ('a = f\nb = f', 'a = e\na = return', 'a = e e', 'a = 1'):
            self.assertFalse(parse_layout(text).valid, text)

    @unittest.skipUnless(importlib.util.find_spec('fontTools'), 'static generator needs fontTools')
    def test_custom_keyboard_glyphs_use_saved_bindings(self):
        import make_button_icons as icons

        class Pen:
            def moveTo(self, *args): pass
            def lineTo(self, *args): pass
            def curveTo(self, *args): pass
            def closePath(self): pass

        class Glyphs:
            def __init__(self): self.labels = []
            def hole(self, pen, text, *args): self.labels.append(text)

        layout = parse_layout('a = f\ny = mouse_right g\nminus = none\nmove_up = arrow_up')
        self.assertTrue(layout.valid)
        for mouse, expected in ((True, 'RMB'), (False, 'G')):
            glyphs = Glyphs()
            style = icons.keyboard_style(glyphs, bindings=layout.bindings, mouse_enabled=mouse)
            style[0xE040][1](Pen()); self.assertEqual(glyphs.labels.pop(), 'F')
            style[0xE043][1](Pen()); self.assertEqual(glyphs.labels.pop(), expected)
            style[0xE04C][1](Pen()); self.assertEqual(glyphs.labels.pop(), '—')


if __name__ == '__main__':
    unittest.main()

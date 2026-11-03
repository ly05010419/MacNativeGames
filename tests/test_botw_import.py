"""BOTW separate-package regressions, using synthetic NCAs and keys only."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import build_app
from test_game_containers import HEADER, AREA, TITLEKEK, encrypt, nca, partition, ticket
from extract_exefs import decrypt, load_keys
from cryptography.hazmat.primitives.ciphers import modes

ROOT = Path(__file__).resolve().parents[1]
BOTW = ROOT / 'games/botw/scripts'
TITLE = 0x01007EF00011E000


def program(update=False, managed=True, title=TITLE):
    data = nca(managed, title=title)
    if not update:
        return data
    header = bytearray(b''.join(decrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')),
                                     data[i*512:(i+1)*512]) for i in range(6)))
    header[0x604] = 4
    header[0x2a0:0x2c0] = hashlib.sha256(header[0x600:0x800]).digest()
    return b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), header[i*512:(i+1)*512])
                    for i in range(6)) + data[0xc00:]


class BotwImport(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.keys = self.root / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in
                            [('header_key', HEADER), ('key_area_key_application_00', AREA), ('titlekek_00', TITLEKEK)]))
        self.base = self.root / 'base.nsp'
        self.update = self.root / 'update.nsp'
        self.base.write_bytes(partition([('base.nca', program()), ('base.tik', ticket())]))
        self.update.write_bytes(partition([('update.nca', program(True)), ('update.tik', ticket())]))

    def extract(self):
        return subprocess.run([sys.executable, BOTW / 'extract_exefs.py', self.update,
                               '--keys', self.keys, '--output', self.root / 'exefs'], capture_output=True, text=True)

    def test_common_ticket_update_exefs(self):
        result = self.extract()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'exefs/main').read_bytes(), b'synthetic main')

    def test_missing_ticket_rejected(self):
        self.update.write_bytes(partition([('update.nca', program(True))]))
        self.assertIn('Missing common ticket', self.extract().stderr)

    def test_personalized_ticket_rejected(self):
        self.update.write_bytes(partition([('update.nca', program(True)), ('key.tik', ticket(personalized=True))]))
        self.assertIn('Personalized', self.extract().stderr)

    def test_wrong_title_rejected(self):
        self.update.write_bytes(partition([('update.nca', program(True, title=123)), ('key.tik', ticket())]))
        self.assertIn('Expected one update Program', self.extract().stderr)

    def test_separate_and_combined_program_selection(self):
        spec = importlib.util.spec_from_file_location('botw_romfs', BOTW / 'extract_romfs.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        base, update, section, tickets = module.programs(self.base, load_keys(self.keys), self.update)
        self.assertEqual((base['name'], update['name'], section, len(tickets)), ('base.nca', 'update.nca', 1, 1))
        self.base.write_bytes(partition([('base.nca', program(False, False)), ('update.nca', program(True, False))]))
        self.assertEqual(module.programs(self.base, load_keys(self.keys))[2], 1)

    def test_conflicting_tickets_rejected(self):
        spec = importlib.util.spec_from_file_location('botw_romfs', BOTW / 'extract_romfs.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.update.write_bytes(partition([('update.nca', program(True)), ('key.tik', ticket(bad_key=True))]))
        with self.assertRaisesRegex(ValueError, 'Conflicting common tickets'):
            module.programs(self.base, load_keys(self.keys), self.update)

    def test_separate_install_removes_cached_dlc_and_configuration(self):
        tree = self.root / 'tree'
        folder = tree / 'extracted/botw160/romfs'
        folder.mkdir(parents=True)
        (folder / 'romfs.bin').write_bytes(b'cached')
        expected = dict(build_app.BOTW160, romfs={'sha256': 'verified', 'size': 6})
        (folder / 'index.json').write_text(json.dumps({'status': 'pass', 'romfs_sha256': 'verified'}))
        aoc = tree / 'extracted/botw160/aoc/1'
        aoc.mkdir(parents=True)
        (aoc / 'index.json').write_text('{}')
        config = tree / 'build/game-data/botw160/aoc-config.txt'
        config.parent.mkdir(parents=True)
        config.write_text('AOC_PRESENT_MASK 3')
        with patch.dict(build_app.VERSIONS, {'botw160': expected}), patch.object(build_app, 'fonts_valid', return_value=True), \
                patch.object(build_app, 'build_hactool') as build:
            build_app.step_data_botw(tree, BOTW.parent, self.base, self.keys, self.root / 'logs', update=self.update)
        build.assert_not_called()
        self.assertFalse(aoc.exists())
        self.assertFalse(config.exists())

    def cached_romfs(self):
        tree = self.root / 'tree'
        folder = tree / 'extracted/botw160/romfs'
        folder.mkdir(parents=True)
        (folder / 'romfs.bin').write_bytes(b'cached')
        (folder / 'index.json').write_text(json.dumps({'status': 'pass', 'romfs_sha256': 'verified'}))
        return tree, dict(build_app.BOTW160, romfs={'sha256': 'verified', 'size': 6})

    def test_fonts_from_firmware(self):
        tree, expected = self.cached_romfs()
        firmware = self.root / 'firmware'
        with patch.dict(build_app.VERSIONS, {'botw160': expected}), patch.object(build_app, 'fonts_valid', return_value=False), \
                patch.object(build_app, 'run') as run, patch.object(build_app, 'validate_fonts') as validate:
            build_app.step_data_botw(tree, BOTW.parent, self.base, self.keys, self.root / 'logs', firmware=firmware,
                                     update=self.update)
        command = run.call_args.args[0]
        self.assertIn('scripts/extract_shared_fonts.py', command)
        self.assertEqual(command[command.index('--firmware') + 1], firmware)
        self.assertEqual(command[command.index('--output') + 1], tree / 'extracted/firmware/shared-fonts')
        validate.assert_called_once()

    def test_no_font_source_names_both_options(self):
        tree, expected = self.cached_romfs()
        with patch.dict(build_app.VERSIONS, {'botw160': expected}), patch.object(build_app, 'fonts_valid', return_value=False), \
                self.assertRaisesRegex(build_app.BuildError, '--firmware .* or --shared-fonts'):
            build_app.step_data_botw(tree, BOTW.parent, self.base, self.keys, self.root / 'logs', update=self.update)

    def test_bundle_without_expansion_pass(self):
        tree = self.root / 'tree'
        files = ['build/totk-native-botw160', 'build/pso_manifest.tsv', 'build/pso_title.tsv',
                 'build/spirv-cross/spirv-cross', 'prebuilt/macos-arm64/totk_xlate', 'build/app-launcher',
                 'extracted/botw160/romfs/romfs.bin', 'extracted/botw160/romfs/runtime-index.bin',
                 'build/shader-seed/botw160/pso_manifest.tsv']
        files += [f'rewritten/botw160/{n}.patched.bin' for n in ('main', 'sdk', 'subsdk0')]
        files += [f'build/game-data/botw160/{n}' for n in build_app.game_data_files('botw160')]
        for rel in files:
            path = tree / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'test')
        (tree / 'build/shader-seed/botw160/seed-manifest.json').write_text(json.dumps(dict(
            version='botw160', chain='totk-native-v8-unverified', shaders=2000,
            files={'pso_manifest.tsv': hashlib.sha256(b'test').hexdigest()})))
        out = self.root / 'BotW.app'
        with patch.object(build_app, 'sign_app'), patch.object(build_app, 'app_seal_valid', return_value=True), \
                patch.object(build_app, 'resolve_lists') as lists:
            build_app.step_app(tree, out, self.root / 'logs', version='botw160', game='botw')
        self.assertEqual(lists.call_args[0][2], 'botw')   # the Breath of the Wild list
        self.assertTrue((out / 'Contents/MacOS/BotW').is_file())
        self.assertFalse((out / 'Contents/Resources/totk/extracted/botw160/aoc').exists())

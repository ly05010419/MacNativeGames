"""Donkey Kong Bananza from a .nup or a dump folder, its add-on content included: the two sources give the same files, and
an add-on pack is installed only when it is the verified one. The packages here are made up: no game data."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

import build_app  # noqa: E402
from nup_game import BANANZA, BANANZA_UPDATE, fmt, make_game_nup  # noqa: E402
from tools import nup_title  # noqa: E402
from tools.builder_jobs import classify  # noqa: E402

SCRIPT = ROOT / 'games/totk/scripts/import_dump.py'
NUP_LIB = ROOT / 'games/oot/third_party/nup'
ADDON = BANANZA + 0x1001
EXEFS = {'main': b'code' * 9, 'main.npdm': b'npdm', 'rtld': b'rtld', 'sdk': b'sdk!'}
ROMFS = {'Pack/a.pack': b'a' * 21, 'Effect/e.zs': b'e' * 5, 'z.bin': b'z'}
DLC = {'Voice/v.bwav': b'v' * 33, 'Model/m.zs': b'm' * 7}


def load():
    spec = importlib.util.spec_from_file_location('import_dump_ns2', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_tree(folder, files):
    for rel, data in files.items():
        (folder / rel).parent.mkdir(parents=True, exist_ok=True)
        (folder / rel).write_bytes(data)


def bananza_nup(path, addon=DLC):
    packages = [(BANANZA, fmt.KIND_BASE, 0, {'main': b'old', 'main.npdm': b'npdm'}, {'old.bin': b'old'}),
                (BANANZA + 0x800, fmt.KIND_UPDATE, BANANZA_UPDATE, EXEFS, ROMFS)]
    if addon is not None:
        packages.append((ADDON, fmt.KIND_ADDON, 0, None, addon))
    return make_game_nup(path, *packages)


def bananza_folder(dump, addon=DLC):
    write_tree(dump / 'base/exefs', {'main': b'old', 'main.npdm': b'npdm'})
    write_tree(dump / 'base/romfs', {'old.bin': b'old'})
    write_tree(dump / f'updates/{BANANZA_UPDATE}/exefs', EXEFS)
    write_tree(dump / f'updates/{BANANZA_UPDATE}/romfs', {**ROMFS, '.DS_Store': b'junk', 'Pack/.listing': b'junk'})
    if addon is not None:
        write_tree(dump / f'dlc/{ADDON:016x}/0/romfs', {**addon, '.DS_Store': b'junk'})
    return dump


def run_import(*args):
    return subprocess.run([sys.executable, '-B', str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class TwoSources(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name).resolve()

    def imported(self, source, name, *extra):
        out = self.dir / name
        r = run_import(source, '--output', out, '--update', BANANZA_UPDATE, '--nup-lib', NUP_LIB, *extra)
        self.assertEqual(r.returncode, 0, r.stderr)
        return out

    def test_a_nup_imports_what_its_dump_folder_imports(self):
        a = self.imported(bananza_nup(self.dir / 'g.nup'), 'nup')
        b = self.imported(bananza_folder(self.dir / 'dump'), 'folder')
        for name, data in EXEFS.items():
            self.assertEqual((a / 'exefs' / name).read_bytes(), data)
            self.assertEqual((b / 'exefs' / name).read_bytes(), data)
        self.assertEqual((a / 'romfs/romfs.bin').read_bytes(), (b / 'romfs/romfs.bin').read_bytes())
        ia, ib = (json.loads((x / 'romfs/index.json').read_text()) for x in (a, b))
        self.assertEqual(ia['files'], ib['files'])
        self.assertEqual(ia['romfs_sha256'], hashlib.sha256((a / 'romfs/romfs.bin').read_bytes()).hexdigest())

    def test_a_damaged_file_in_a_nup_stops_the_import(self):
        path = bananza_nup(self.dir / 'g.nup')
        data = bytearray(path.read_bytes())
        at = data.rindex(b'z' + bytes(15))   # the update's z.bin, past the table the header's hash covers
        data[at] = ord('Y')
        path.write_bytes(data)
        r = run_import(path, '--output', self.dir / 'out', '--update', BANANZA_UPDATE, '--nup-lib', NUP_LIB)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('z.bin', r.stderr)

    def addons(self, source, name):
        out = self.dir / name
        r = run_import(source, '--addons', out, '--title', f'{BANANZA:016X}', '--nup-lib', NUP_LIB)
        self.assertEqual(r.returncode, 0, r.stderr)
        return out, json.loads(r.stdout.splitlines()[-1])

    def test_the_add_on_is_the_same_from_either_source(self):
        a, ra = self.addons(bananza_nup(self.dir / 'g.nup'), 'aoc-nup')
        b, rb = self.addons(bananza_folder(self.dir / 'dump'), 'aoc-folder')
        self.assertEqual(sorted(ra['addons']), ['1'])
        self.assertEqual(ra['addons']['1']['title_id'], f'{ADDON:016X}')
        self.assertEqual(ra['addons']['1']['sha256'], rb['addons']['1']['sha256'])
        self.assertEqual((a / '1/romfs.bin').read_bytes(), (b / '1/romfs.bin').read_bytes())
        report = json.loads((a / '1/index.json').read_text())
        self.assertEqual((report['status'], report['index']), ('pass', 1))
        self.assertEqual([f['path'] for f in report['files']], sorted(DLC))

    def test_a_source_without_add_ons_gives_none(self):
        _, report = self.addons(bananza_nup(self.dir / 'g.nup', addon=None), 'aoc')
        self.assertEqual(report['addons'], {})

    def test_listing_reads_no_file(self):
        path = bananza_nup(self.dir / 'g.nup')
        r = run_import(path, '--list-addons', '--title', f'{BANANZA:016X}', '--nup-lib', NUP_LIB)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout.splitlines()[-1])['addons'],
                         {'1': dict(title_id=f'{ADDON:016X}', files=2, bytes=sum(map(len, DLC.values())))})


class Route(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name).resolve()
        env = mock.patch.dict(os.environ, {'MNG_LANG': 'en'})
        env.start()
        self.addCleanup(env.stop)

    def test_the_gate(self):
        self.assertTrue(nup_title.builds(f'{BANANZA:016X}', [0, BANANZA_UPDATE]))
        self.assertFalse(nup_title.builds(f'{BANANZA:016X}', [0, 393216]))
        self.assertFalse(nup_title.builds('0400A3A01EE4C000', [655360]))

    def test_the_launcher_takes_it_as_bananza(self):
        path = bananza_nup(self.dir / 'any name.nup')
        plan = classify.plan(classify.classify([str(path)]))
        self.assertEqual((plan.game, plan.base, plan.blocks), ('dkb', str(path), []))

    def test_it_is_built_by_the_switch_2_path(self):
        path = bananza_nup(self.dir / 'g.nup')
        with mock.patch.object(build_app, 'main_ns2') as ns2, mock.patch.object(build_app, 'build_oot') as oot:
            build_app.main([str(path)])
        oot.assert_not_called()
        self.assertEqual(ns2.call_args.args[1], path)

    def test_another_version_is_refused_with_its_own_version(self):
        path = make_game_nup(self.dir / 'g.nup', (BANANZA, fmt.KIND_BASE, 0, EXEFS, ROMFS),
                             (BANANZA + 0x800, fmt.KIND_UPDATE, 393216, EXEFS, ROMFS))
        with self.assertRaisesRegex(build_app.BuildError, 'v393216'):
            build_app.main([str(path)])


class AddOnStep(unittest.TestCase):
    """build_app.step_aoc_ns2: only the verified pack goes in; another one leaves the build going without it."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name).resolve()
        self.tree = self.dir / 'work/totk'
        (self.tree / '.venv/bin').mkdir(parents=True)
        (self.tree / '.venv/bin/python').symlink_to(sys.executable)
        (self.tree / 'scripts').mkdir()
        (self.tree / 'scripts/import_dump.py').symlink_to(SCRIPT)
        self.logs = self.dir / 'logs'
        self.logs.mkdir()
        self.g = dict(build_app.NS2[f'{BANANZA:016X}'])
        probe = self.dir / 'probe'
        r = run_import(bananza_nup(self.dir / 'probe.nup'), '--addons', probe, '--title', f'{BANANZA:016X}', '--nup-lib', NUP_LIB)
        self.assertEqual(r.returncode, 0, r.stderr)
        made = json.loads(r.stdout.splitlines()[-1])['addons']['1']
        self.pin = dict(title_id=f'{ADDON:016X}', size=made['size'], sha256=made['sha256'])

    def step(self, source, pin):
        expected = {**build_app.VERSIONS['dkb'], 'aoc': {'1': pin}}
        said = []
        with mock.patch.dict(build_app.VERSIONS, {'dkb': expected}), mock.patch.object(build_app, 'say', said.append):
            build_app.step_aoc_ns2(self.tree, source, self.g, self.logs)
        return ' '.join(said)

    def test_the_verified_pack_is_installed(self):
        self.step(bananza_nup(self.dir / 'g.nup'), self.pin)
        self.assertTrue((self.tree / 'extracted/dkb/aoc/1/romfs.bin').is_file())

    def test_another_pack_is_left_out_and_said(self):
        said = self.step(bananza_nup(self.dir / 'g.nup', addon={'other.bin': b'other'}), self.pin)
        self.assertFalse((self.tree / 'extracted/dkb/aoc/1').exists())
        self.assertIn('not the verified', said)

    def test_a_source_without_it_removes_the_old_pack(self):
        self.step(bananza_nup(self.dir / 'g.nup'), self.pin)
        config = self.tree / 'build/game-data/dkb/aoc-config.txt'
        config.parent.mkdir(parents=True)
        config.write_text('AOC_PRESENT_MASK 1\n')
        self.step(bananza_nup(self.dir / 'none.nup', addon=None), self.pin)
        self.assertFalse((self.tree / 'extracted/dkb/aoc/1').exists())
        self.assertFalse(config.exists())

    def test_a_valid_pack_is_not_imported_again(self):
        self.step(bananza_nup(self.dir / 'g.nup'), self.pin)
        (self.tree / 'extracted/dkb/aoc/1/runtime-index.bin').write_bytes(b'index')
        image = self.tree / 'extracted/dkb/aoc/1/romfs.bin'
        before = image.stat().st_mtime_ns
        self.step(bananza_nup(self.dir / 'g.nup'), self.pin)
        self.assertEqual(image.stat().st_mtime_ns, before)


class Runtime(unittest.TestCase):
    """The Bananza runtime answers nn::aoc and mounts the packs; Pokopia keeps its "none"."""

    def test_the_real_volumes_are_compiled_for_bananza(self):
        for name in ('fs_host.h', 'aoc_romfs.c', 'services.c'):
            text = (ROOT / 'games/totk/runtime' / name).read_text()
            self.assertIn('defined(GAME_BOTW) || defined(GAME_DKB)', text, name)

    def test_the_volumes_are_linked_into_bananza(self):
        text = (ROOT / 'games/totk/Makefile').read_text()
        recipe = text[text.index('build/totk-native-dkb:'):text.index('runtime-dkb binary-dkb: SHADER_LIST')]
        self.assertIn('runtime/aoc_romfs.c', recipe)

    def test_the_stub_is_not_bananzas(self):
        text = (ROOT / 'games/totk/runtime/ns2_host.c').read_text()
        stub = text.index('static Result aoc_query')
        self.assertIn('#ifndef GAME_DKB', text[max(0, stub - 200):stub])
        table = text.index('_ZN2nn2fs17MountAddOnContentEPKciPvm')
        self.assertIn('#ifndef GAME_DKB', text[max(0, table - 200):table])

    def test_an_app_packed_before_add_ons_starts(self):
        text = (ROOT / 'games/totk/runtime/aoc_romfs.c').read_text()
        load_config = text[text.index('static void config_load'):text.index('static int present')]
        self.assertRegex(load_config, r'access\(path, R_OK\)')

    def test_the_package_step_carries_the_packs_to_a_device(self):
        text = (ROOT / 'games/totk/ios/tools/build_ipa.py').read_text()
        self.assertTrue(re.search(r"if botw or a\.version=='dkb':", text))


class ShaderCorpus(unittest.TestCase):
    def test_the_add_on_is_scanned(self):
        text = (ROOT / 'games/totk/scripts/ns2_shader_corpus.py').read_text()
        self.assertIn('--extra-romfs', text)
        seed = (ROOT / 'games/totk/scripts/prepare_ns2_seed.py').read_text()
        self.assertIn('--extra-romfs', seed)

    def test_a_build_without_add_ons_keeps_its_corpus_stamp(self):
        seed = (ROOT / 'games/totk/scripts/prepare_ns2_seed.py').read_text()
        self.assertRegex(seed, r"if addons:\s*\n\s*want\['addons'\]")


if __name__ == '__main__':
    unittest.main()

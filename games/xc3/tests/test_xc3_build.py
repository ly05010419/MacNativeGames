"""Xenoblade Chronicles 3's one-command build (build.py): the inputs it takes or refuses, read without keys from synthetic
packages, and the checks that let a run reuse the work folder. Needs no game files."""
import importlib.util
import json
import plistlib
from pathlib import Path
import struct
import tempfile
import unittest

GAME_DIR = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('xc3_build', GAME_DIR / 'build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def nsp(path, titles):
    """A PFS0 package holding one common ticket (signature type 0x10004) per title, and nothing else."""
    files = []
    for i, title in enumerate(titles):
        body = 0x140
        ticket = bytearray(body + 0x180)
        struct.pack_into('<I', ticket, 0, 0x10004)
        ticket[body + 0x160:body + 0x168] = bytes.fromhex(title)
        files.append((f'{title}{i:016x}.tik', bytes(ticket)))
    names = b''.join(name.encode() + b'\0' for name, _ in files)
    names += b'\0' * (-len(names) % 16)
    table, offset, at = b'', 0, 0
    for name, data in files:
        table += struct.pack('<QQII', offset, len(data), at, 0)
        offset += len(data)
        at += len(name) + 1
    path.write_bytes(b'PFS0' + struct.pack('<II', len(files), len(names)) + b'\0' * 4 + table + names
                     + b''.join(data for _, data in files))
    return path


class Packages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_titles_are_read_from_the_tickets(self):
        package = nsp(self.dir / 'base.nsp', [build.TITLE_ID])
        self.assertEqual(build.ticket_titles(package), [build.TITLE_ID])

    def test_each_part_of_the_game_is_told_apart(self):
        self.assertEqual(build.role('010074F013262000'), 'base')
        self.assertEqual(build.role('010074F013262800'), 'update')
        self.assertEqual(build.role('010074F013263004'), ('dlc', 4))
        self.assertIsNone(build.role('0100F2C0115B6000'))

    def test_the_right_part_in_the_right_place_is_taken(self):
        self.assertIsNone(build.check_package(nsp(self.dir / 'b.nsp', [build.TITLE_ID]), 'base'))
        self.assertIsNone(build.check_package(nsp(self.dir / 'u.nsp', ['010074F013262800']), 'update'))
        self.assertEqual(build.check_package(nsp(self.dir / 'd.nsp', ['010074F013263002']), 'dlc'), 2)

    def test_a_part_in_the_wrong_place_says_where_it_goes(self):
        update = nsp(self.dir / 'u.nsp', ['010074F013262800'])
        with self.assertRaisesRegex(build.BuildError, 'with --update') as caught:
            build.check_package(update, 'base')
        self.assertEqual(caught.exception.code, 'wrong_game')
        with self.assertRaisesRegex(build.BuildError, 'with --dlc'):
            build.check_package(nsp(self.dir / 'd.nsp', ['010074F013263001']), 'update')

    def test_another_game_or_a_non_package_is_refused(self):
        with self.assertRaisesRegex(build.BuildError, 'is not Xenoblade'):
            build.check_package(nsp(self.dir / 'o.nsp', ['0100F2C0115B6000']), 'base')
        other = self.dir / 'x.xci'
        other.write_bytes(b'\0' * 0x200)
        with self.assertRaisesRegex(build.BuildError, 'expected an NSP'):
            build.check_package(other, 'base')

    def test_an_unpinned_add_on_is_refused(self):
        with self.assertRaisesRegex(build.BuildError, 'add-on 9'):
            build.check_package(nsp(self.dir / 'd.nsp', ['010074F013263009']), 'dlc')


class Inputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.base = nsp(self.dir / 'base.nsp', [build.TITLE_ID])
        self.update = nsp(self.dir / 'update.nsp', ['010074F013262800'])
        self.keys = self.dir / 'prod.keys'
        self.keys.write_text('')

    def tearDown(self):
        self.tmp.cleanup()

    def resolve(self, *extra):
        return build.resolve_inputs(build.parse_args([str(self.base), '-k', str(self.keys), *extra]))

    def test_the_base_alone_is_1_0_0_with_defaults_beside_it(self):
        game, update, dlcs, *_, out, work, version = self.resolve()
        self.assertEqual((update, dlcs, version), (None, {}, 'v100'))
        self.assertEqual(out, self.dir.resolve() / 'Xenoblade Chronicles 3.app')
        self.assertEqual(work, self.dir.resolve().parent / 'MacNativeGames-work/xc3', 'build_app.default_work, as every port')

    def test_a_rebuild_reuses_the_work_folder_the_app_records(self):
        app = self.dir / 'Xenoblade Chronicles 3.app'
        (app / 'Contents').mkdir(parents=True)
        recorded = self.dir / 'elsewhere' / 'xc3'
        recorded.parent.mkdir()
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleName': 'XC3'}))
        build.record_work_folder(app, recorded)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual(info['MNGWorkFolder'], str(recorded.resolve()))
        self.assertEqual(info['CFBundleName'], 'XC3')
        *_, work, _ = self.resolve()
        self.assertEqual(work, recorded.resolve())

    def test_the_update_makes_2_2_1_with_its_add_ons(self):
        dlc = nsp(self.dir / 'fr.nsp', ['010074F013263004'])
        _, update, dlcs, *_, version = self.resolve('--update', str(self.update), '--dlc', str(dlc))
        self.assertEqual(version, 'v221')
        self.assertEqual(update, self.update.resolve())
        self.assertEqual(dlcs, {dlc.resolve(): 4})

    def test_add_ons_need_the_update(self):
        dlc = nsp(self.dir / 'w1.nsp', ['010074F013263001'])
        with self.assertRaisesRegex(build.BuildError, 'need the 2.2.1 update'):
            self.resolve('--dlc', str(dlc))

    def test_one_volume_twice_is_refused(self):
        a = nsp(self.dir / 'a.nsp', ['010074F013263002'])
        b = nsp(self.dir / 'b.nsp', ['010074F013263002'])
        with self.assertRaisesRegex(build.BuildError, 'same Expansion Pass volume'):
            self.resolve('--update', str(self.update), '--dlc', str(a), '--dlc', str(b))

    def test_the_work_folder_and_the_app_stay_apart(self):
        with self.assertRaisesRegex(build.BuildError, 'outside each other'):
            self.resolve('-o', str(self.dir / 'w/XC3.app'), '--work', str(self.dir / 'w'))
        with self.assertRaisesRegex(build.BuildError, 'outside the checkout'):
            self.resolve('--work', str(build.REPO / 'XC3_build'))

    def test_mac_only_and_an_app_only(self):
        with self.assertRaisesRegex(build.BuildError, 'for the Mac only'):
            self.resolve('--platform', 'ios')
        with self.assertRaisesRegex(build.BuildError, 'must end in .app'):
            self.resolve('-o', str(self.dir / 'XC3.ipa'))

    def test_a_missing_file_is_named(self):
        with self.assertRaisesRegex(build.BuildError, 'no such file'):
            self.resolve('--update', str(self.dir / 'gone.nsp'))


class Reuse(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_both_versions_are_pinned(self):
        self.assertEqual(build.expected('v100')['display_version'], '1.0.0')
        self.assertEqual(build.expected('v221')['display_version'], '2.2.1')
        self.assertEqual(build.expected('v221')['update_title_id'], build.UPDATE_TITLE_ID)

    def test_a_romfs_counts_only_when_its_report_and_size_match(self):
        romfs = self.work / 'extracted/v100/romfs'
        romfs.mkdir(parents=True)
        self.assertFalse(build.romfs_done(romfs, 'v100'))
        pinned = build.expected('v100')['romfs']
        (romfs / 'index.json').write_text(json.dumps({'status': 'pass', 'romfs_sha256': pinned['sha256']}))
        (romfs / 'romfs.bin').write_bytes(b'\0' * 16)
        self.assertFalse(build.romfs_done(romfs, 'v100'), 'a short image is not the RomFS')

    def test_code_from_another_source_is_extracted_again(self):
        folder = self.work / 'extracted/v221'
        folder.mkdir(parents=True)
        (folder / 'extraction-manifest.json').write_text(json.dumps({'source': '/elsewhere/update.nsp'}))
        self.assertFalse(build.code_done(self.work, 'v221', Path('/games/update.nsp')))

    def test_an_add_on_is_kept_only_for_the_same_file(self):
        folder = self.work / 'extracted/aoc/4'
        folder.mkdir(parents=True)
        (folder / 'index.json').write_text(json.dumps({'status': 'pass', 'source': '/games/fr.nsp'}))
        self.assertTrue(build.addon_done(folder, Path('/games/fr.nsp')))
        self.assertFalse(build.addon_done(folder, Path('/games/other.nsp')))

    def test_add_ons_no_longer_given_are_removed(self):
        aoc = self.work / 'extracted/aoc'
        for index, source in ((1, '/games/w1.nsp'), (4, '/games/fr.nsp')):
            (aoc / str(index)).mkdir(parents=True)
            (aoc / str(index) / 'index.json').write_text(json.dumps({'status': 'pass', 'source': source}))
        build.step_addons(None, self.work, 'v221', {Path('/games/fr.nsp'): 4}, None, self.work / 'logs')
        self.assertEqual(sorted(p.name for p in aoc.iterdir()), ['4'])
        build.step_addons(None, self.work, 'v100', {}, None, self.work / 'logs')
        self.assertFalse(aoc.exists(), '1.0.0 carries no add-ons')

    def test_make_sees_a_work_folder_with_spaces_through_a_link(self):
        plain = self.work / 'XC3_build'
        (plain / 'venv/bin').mkdir(parents=True)
        self.assertEqual(build.make_args(plain / 'venv/bin/python', plain, 'v100'),
                         [f'WORK={plain}', f'PY={plain}/venv/bin/python', 'VERSION=v100'])
        spaced = self.work / 'Xenoblade Chronicles 3/XC3_build'
        (spaced / 'venv/bin').mkdir(parents=True)
        args = build.make_args(spaced / 'venv/bin/python', spaced, 'v221')
        link = Path(args[0].split('=', 1)[1])
        try:
            self.assertNotIn(' ', str(link))
            self.assertEqual(link.resolve(), spaced.resolve())
            self.assertEqual(build.make_args(spaced / 'venv/bin/python', spaced, 'v221'), args, 'the same link on the next run')
            self.assertEqual(args, [f'WORK={link}', f'PY={link}/venv/bin/python', 'VERSION=v221'])
        finally:
            link.unlink()

    def test_game_json_names_this_script_and_its_inputs(self):
        game = json.loads((GAME_DIR / 'game.json').read_text())
        self.assertEqual(game['title_id'], '010074F013262000')
        self.assertEqual(game['build'], 'games/xc3/build.py')
        self.assertEqual(game['files'], {'game': ['nsp'], 'update': ['nsp'], 'dlc': ['nsp']})
        self.assertFalse(game['work_holds_game_data'])


if __name__ == '__main__':
    unittest.main()

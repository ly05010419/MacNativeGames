"""Build safety and portable-toolchain checks for the Mario Kart 8 Deluxe target; synthetic inputs only, no game files or keys."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
import build_mk8d as build


def nsp(path, names):
    """A PFS0 header with the given file names and empty payloads: enough for the ticket-based title check."""
    strings = b''.join(n.encode() + b'\0' for n in names)
    entries, offset = b'', 0
    for n in names:
        entries += struct.pack('<QQI4x', 0, 0, offset)
        offset += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<II4x', len(names), len(strings)) + entries + strings)
    return path


def load_builder():
    spec = importlib.util.spec_from_file_location('nsp_builder_test', GAME / 'third_party/nxnative/build_ipa_from_nsp.py')
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(GAME / 'runtime/tools'))
    spec.loader.exec_module(module)
    return module


class BuildTests(unittest.TestCase):
    def test_tooling_checksums(self):
        build.verify_prebuilt()

    def test_tooling_rejects_unlisted_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copytree(GAME / 'prebuilt', root / 'prebuilt')
            (root / 'prebuilt/macos-arm64/dmsh_xlate/unexpected.dll').write_bytes(b'extra')
            with mock.patch.object(build, 'GAME', root):
                with self.assertRaisesRegex(ValueError, 'file set differs'):
                    build.verify_prebuilt()

    def test_source_staging_preserves_generated_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            feed = work / 'dependencies/nuget'
            source = build.stage(work, feed)
            table = source / f'dm/games/{build.TITLE}/c1_full.tsv'
            table.write_text('generated locally')
            build.stage(work, feed)
            self.assertEqual(table.read_text(), 'generated locally')
            self.assertTrue((source / 'dm/tools/nx_extract/Program.cs').is_file())
            self.assertFalse((source / 'dm/gfx/tools/spirv_cross_bin/spirv-cross').exists())   # built after staging (build_spirv_cross)
            for exe in ('dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate',):
                self.assertTrue((source / exe).stat().st_mode & 0o111, exe)
            self.assertIn(f'value="{feed}"', (source / 'NuGet.Config').read_text())
            self.assertIn('<clear />', (source / 'NuGet.Config').read_text())
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True).strip()
            self.assertIn(f'"{chain}"', (source / 'dm/gfx/nvn/dmsh_chain.h').read_text())

    def test_spirv_cross_is_built_from_its_pinned_release(self):
        # no binary is carried (after PR #1226): the lock pins the --revision the build writes, from the release and commit time
        self.assertFalse((GAME / 'prebuilt/macos-arm64/spirv_cross/spirv-cross').exists())
        lock = dict(l.rstrip('\n').split('\t', 1) for l in (GAME / 'runtime/gfx/tools/dmsh_chain.lock').read_text().splitlines(True)
                    if '\t' in l and not l.startswith('#'))
        self.assertEqual(lock['spirv-cross'], build.SPIRV_CROSS_REVISION)
        header = build.spirv_cross_header('vulkan-sdk-1.4.363.0', 1789558039)   # f11ba9f's commit time
        self.assertIn(f'"{build.SPIRV_CROSS_REVISION}"', header)
        self.assertEqual(build.SPIRV_CROSS[1], 'vulkan-sdk-1.4.363.0')

    def test_chain_header_matches_runtime(self):
        chain = subprocess.check_output([sys.executable, str(GAME / 'runtime/gfx/tools/dmsh_chain.py')], text=True).strip()
        self.assertIn(f'"{chain}"', (GAME / 'runtime/gfx/nvn/dmsh_chain.h').read_text())

    def test_input_selection_takes_the_booster_course_pass_by_its_ticket(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()   # macOS: the temporary folder is under /var, a link to /private/var
            inputs, work = root / 'originals', root / 'build'
            inputs.mkdir(); work.mkdir()
            base = nsp(inputs / 'any name.nsp', [f'{build.TITLE}0000000000000000.tik', 'a.nca'])
            update = nsp(inputs / 'update [v1507328].nsp', [f'{build.UPDATE}0000000000000004.tik', 'b.nca'])
            game = [f'Mario Kart 8 Deluxe [{build.TITLE}][v0].nsp', f'Mario Kart 8 Deluxe [{build.UPDATE}][v1507328].nsp']
            self.assertEqual(sorted(p.name for p in build.link_inputs(inputs, work).iterdir()), game)   # the add-on content is optional
            nsp(inputs / f'no ticket [{build.AOC}][v65536].nsp', ['c.nca'])   # a name alone does not make it add-on content
            nsp(inputs / 'other add-on [0100152000023002].nsp', ['0100152000023002000000000000000d.tik'])
            aoc = nsp(inputs / 'booster [v65536].nsp', [f'{build.AOC}000000000000000d.tik', 'd.nca'])
            result = build.link_inputs(inputs, work)
            links = sorted(p.name for p in result.iterdir())
            self.assertEqual(links, game + [f'Mario Kart 8 Deluxe [{build.AOC}][v65536].nsp'])
            self.assertEqual([(result / name).resolve() for name in links], [base, update, aoc])

    def test_input_selection_rejects_other_add_on_versions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            inputs = root / 'originals'; inputs.mkdir()
            nsp(inputs / 'base.nsp', [f'{build.TITLE}0000000000000000.tik'])
            nsp(inputs / 'update [v1507328].nsp', [f'{build.UPDATE}0000000000000004.tik'])
            nsp(inputs / 'booster [v131072].nsp', [f'{build.AOC}000000000000000d.tik'])
            with self.assertRaisesRegex(ValueError, 'only the Booster Course Pass v65536'):
                build.link_inputs(inputs, root / 'build')

    def test_input_selection_replaces_links_to_moved_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            old, new, work = root / 'old', root / 'new', root / 'build'
            old.mkdir(); work.mkdir()
            nsp(old / 'base.nsp', [f'{build.TITLE}0000000000000000.tik'])
            nsp(old / 'update [v1507328].nsp', [f'{build.UPDATE}0000000000000004.tik'])
            result = build.link_inputs(old, work)
            old.rename(new)   # the player moved the folder: the links now lead nowhere
            build.link_inputs(new, work)
            self.assertTrue(all(p.resolve().parent == new for p in result.iterdir()))
            (result / f'Mario Kart 8 Deluxe [{build.TITLE}][v0].nsp').unlink()
            (result / f'Mario Kart 8 Deluxe [{build.TITLE}][v0].nsp').write_bytes(b'not a link')
            with self.assertRaisesRegex(ValueError, 'refusing to replace an existing input'):
                build.link_inputs(new, work)

    def test_input_selection_rejects_other_updates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            inputs = root / 'originals'; inputs.mkdir()
            nsp(inputs / 'base.nsp', [f'{build.TITLE}0000000000000000.tik'])
            nsp(inputs / 'update [v1441792].nsp', [f'{build.UPDATE}0000000000000004.tik'])
            with self.assertRaisesRegex(ValueError, 'only the 4.0.1 update'):
                build.link_inputs(inputs, root / 'build')
            (inputs / 'update [v1441792].nsp').unlink()
            with self.assertRaisesRegex(ValueError, 'no Mario Kart 8 Deluxe 4.0.1 update'):
                build.link_inputs(inputs, root / 'build')

    def test_build_caches_stay_in_work_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            env = build.environment(work)
            for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
                self.assertTrue(Path(env[key]).is_relative_to(work))
                self.assertTrue(Path(env[key]).is_dir())

    def test_rejects_missing_or_modified_offline_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            package = work / 'local.nupkg'
            with self.assertRaisesRegex(ValueError, 'package is missing'):
                build.prepare_offline_dependencies(work, package)
            package.write_bytes(b'wrong package')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                build.prepare_offline_dependencies(work, package)
            self.assertFalse((work / 'dependencies').exists())

    def test_builder_rejects_other_titles_dlc_and_versions(self):
        module = load_builder()
        fam = {'base': Path('base.nsp'), 'updates': [(1507328, Path('u.nsp'))], 'dlc': []}
        module.target_inputs(0x0100152000022000, fam)
        module.target_inputs(0x0100152000022000, dict(fam, dlc=[(0x0100152000023001, 65536, Path('aoc.nsp'))]))   # the Booster Course Pass
        with self.assertRaisesRegex(module.BuildError, 'supports only Mario Kart 8 Deluxe'):
            module.target_inputs(0x0100965017338000, fam)
        for dlc in ((0x0100152000023002, 65536), (0x0100152000023001, 131072), (0x0100152000023001, None)):
            with self.assertRaisesRegex(module.BuildError, 'other than the Booster Course Pass'):
                module.target_inputs(0x0100152000022000, dict(fam, dlc=[(*dlc, Path('aoc.nsp'))]))
        with self.assertRaisesRegex(module.BuildError, 'exactly the 4.0.1 update'):
            module.target_inputs(0x0100152000022000, dict(fam, updates=[(1441792, Path('u.nsp'))]))

    def test_builder_rejects_modified_originals_before_rewrite(self):
        module = load_builder()
        pins = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())['update']['exefs']
        with tempfile.TemporaryDirectory() as tmp:
            repo, exefs = Path(tmp), Path(tmp) / 'ExeFS'
            (repo / 'assets').mkdir(); exefs.mkdir()
            shutil.copy2(GAME / 'assets/expected-4.0.1.json', repo / 'assets')
            for name, pin in pins.items():
                (exefs / name).write_bytes(bytes(pin['size']))
            with self.assertRaisesRegex(module.BuildError, 'differs from assets/expected-4.0.1.json'):
                module.verify_originals(repo, 'v1507328', exefs)
            with self.assertRaisesRegex(module.BuildError, 'packs only 4.0.1'):
                module.verify_originals(repo, 'v1441792', exefs)
            (exefs / 'extra').write_bytes(b'x')
            with self.assertRaisesRegex(module.BuildError, 'not the pinned'):
                module.verify_originals(repo, 'v1507328', exefs)

    def test_builder_rejects_modified_add_on_content(self):
        module = load_builder()
        pin = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())['add_on_content'][build.AOC]['romfs_image']
        with tempfile.TemporaryDirectory() as tmp:
            repo, aoc = Path(tmp), Path(tmp) / 'aoc'
            (repo / 'assets').mkdir(); aoc.mkdir()
            shutil.copy2(GAME / 'assets/expected-4.0.1.json', repo / 'assets')
            (aoc / '1.img').write_bytes(bytes(pin['size']))   # the pinned size, other bytes
            with self.assertRaisesRegex(module.BuildError, 'differs from assets/expected-4.0.1.json'):
                module.verify_aoc(repo, aoc)
            (aoc / '1.img').unlink()
            (aoc / '2.img').write_bytes(b'x')
            with self.assertRaisesRegex(module.BuildError, 'no pin'):
                module.verify_aoc(repo, aoc)

    def test_pins_name_the_profile_builds(self):
        pins = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())
        self.assertEqual((pins['title_id'], pins['package'], pins['version']), (build.TITLE, build.VERSION, '4.0.1'))
        rows = [line.split('\t') for line in (GAME / f'runtime/games/{build.TITLE}/game.tsv').read_text().splitlines() if line.startswith('build\t')]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][1], build.VERSION)
        for module_build in rows[0][3:]:
            name, build_id = module_build.split('=')
            self.assertEqual(pins['update']['exefs'][name]['build_id'], build_id)
        for pin in pins['update']['exefs'].values():
            self.assertRegex(pin['sha256'], r'^[0-9a-f]{64}$')

    def test_add_on_pins_match_the_gates(self):
        module = load_builder()
        pins = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())['add_on_content']
        self.assertEqual(list(pins), [build.AOC])
        pin = pins[build.AOC]
        self.assertEqual(pin['package'], build.AOC_VERSION)
        self.assertEqual(module.TARGET_AOC, {int(build.AOC, 16): int(build.AOC_VERSION[1:])})
        self.assertEqual(pin['index'], int(build.AOC, 16) - int(build.TITLE, 16) - 0x1000)   # extract_aoc's <index>.img
        self.assertRegex(pin['romfs_image']['sha256'], r'^[0-9a-f]{64}$')

    def test_profile_carries_no_original_paths(self):
        profile = GAME / f'runtime/games/{build.TITLE}'
        for name in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv'):
            self.assertFalse((profile / name).exists(), name)
        for name in ('pso_manifest.tsv', 'bf_want.tsv', 'shader_map.tsv.conflicts.tsv'):
            data = [line for line in (profile / name).read_text().splitlines() if line and not line.startswith('#')]
            self.assertFalse(any('/' in line for line in data), name)
        rows = dict(line.split('\t', 1) for line in (profile / 'game.tsv').read_text().splitlines() if '\t' in line and not line.startswith('#'))
        self.assertEqual(rows['romfs_marker'], '.')
        self.assertEqual(rows['shader_groups'], '/=archive')

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'the launcher builds on macOS only')
    def test_launcher_backs_up_saves(self):
        harness = ('#define main launcher_main\n#include "launcher.c"\n#undef main\nint dm_mac_launcher(void) { return 0; }\n'
                   'int main(int argc, char **argv) { (void)argc; back_up_saves(argv[1], (time_t)atoll(argv[2])); return 0; }\n')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / 'harness.c').write_text(harness)
            subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-Wall', '-I', GAME / 'runtime/mac', '-I', GAME / 'runtime/host', root / 'harness.c',
                            '-framework', 'CoreFoundation', '-o', root / 'harness'], check=True)
            data = root / 'nxnative/0100152000022000'
            save, device, backups = data / 'save', data / 'save_device', data / 'backups'
            start = 1792540800   # 2026-10-21 00:00:00 UTC
            def launch(minutes):
                out = subprocess.run([root / 'harness', save, str(start + 60 * minutes)], capture_output=True, text=True, check=True, env=dict(os.environ, TZ='UTC'))
                return out.stderr, sorted(p.name for p in backups.iterdir()) if backups.is_dir() else []
            save.mkdir(parents=True)
            self.assertEqual(launch(0)[1], [])   # nothing saved yet
            (save / 'userdata.dat').write_bytes(b'progress 1'); device.mkdir(); (device / 'rp00.dat').write_bytes(b'replay')
            log, names = launch(1)
            self.assertEqual(names, ['20261021-000100'])
            self.assertIn('saves backed up to', log)
            self.assertEqual((backups / names[0] / 'save/userdata.dat').read_bytes(), b'progress 1')
            self.assertEqual((backups / names[0] / 'save_device/rp00.dat').read_bytes(), b'replay')
            self.assertIn('saves unchanged', launch(2)[0])   # the same bytes: no second copy of them
            (backups / '20261021-000200.partial').mkdir(); (backups / 'mine').mkdir()   # an interrupted backup, and a folder of the player's
            for minute in range(3, 10):
                (save / 'userdata.dat').write_bytes(b'progress %d' % minute)
                names = launch(minute)[1]
            self.assertEqual(names, [f'20261021-00{m:02d}00' for m in range(5, 10)] + ['mine'])   # the five newest, the player's folder kept
            self.assertEqual((backups / '20261021-000900/save/userdata.dat').read_bytes(), b'progress 9')
            (device / 'rp00.dat').write_bytes(b'replay 2')   # a change in the device save alone is a change too
            self.assertIn('saves backed up to', launch(10)[0])

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('make') and shutil.which('clang'), 'the host tests build on macOS only')
    def test_library_applets_take_the_focus(self):
        # The copied host's t_hid.c (players, the controller applet's answer, canceled with too few players) and t_oe.c (focus notifications, applets opening and
        # closing), with their Makefile's flags; the game profile header comes from the committed game.tsv, as the host makes it
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            for name in ('host', 'games'):
                shutil.copytree(GAME / 'runtime' / name, root / name)
            host = root / 'host'
            subprocess.run(['make', '-s', f'GAME={build.TITLE}', f'gen/{build.TITLE}/game_profile.h'], cwd=host, check=True, capture_output=True)
            inc = ['-I..', f'-I../gen/{build.TITLE}', f'-I../../games/{build.TITLE}']
            for name, flags, sources, done in (('t_hid', ['-Wall', '-Werror'], ['t_hid.c', '../hle_haptics.c'], r'hid players: \d+ checks, 0 failed'),
                                               ('t_oe', [], ['t_oe.c'], r'== oe \d+ checks, 0 failed ==')):
                subprocess.run(['clang', '-O1', *flags, '-Wno-unused-function', *inc, '-o', root / name, *sources], cwd=host / 'tests', check=True)
                out = subprocess.run([root / name], capture_output=True, text=True)
                self.assertEqual(out.returncode, 0, out.stderr[-2000:])
                self.assertRegex(out.stdout, done)

    def test_keyboard_player_is_a_setting(self):
        rows = [line.split('\t') for line in (GAME / 'runtime/games/settings.tsv').read_text().splitlines() if line.startswith('DM_KBD_PLAYER\t')]
        self.assertEqual(len(rows), 1)
        variable, kind, values, default, shown, section = rows[0][:6]
        self.assertEqual((kind, default, shown, section), ('choice', 'shared', 'platform:mac', 'Controls'))
        self.assertEqual([v.split('=', 1)[0] for v in values.split(',')], ['shared', 'own'])
        self.assertIn('level=basic', rows[0][8:])   # among the basic settings: the Mac window folds the others under Advanced settings
        self.assertIn('!strcmp(e, "own")', (GAME / 'runtime/host/front_mac.m').read_text())   # the front end's own value for the keyboard as a player of its own

    def test_build_environment_disables_nuget_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(build.environment(Path(tmp))['NuGetAudit'], 'false')


if __name__ == '__main__':
    unittest.main()

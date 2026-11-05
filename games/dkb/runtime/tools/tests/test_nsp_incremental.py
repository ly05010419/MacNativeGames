"""NSP script reuse across derived and provisional builds. Real fingerprints and mock tool runs; no game files, keys or Xcode needed."""
import collections
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain, game, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

TID = 0x0100AAAA00000000
BID, BID2 = 'ab' * 16, 'cd' * 16


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


def control(path, disp):   # control.nacp with the display version at 0x3060
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(bytes(0x3060) + disp.encode().ljust(0x10, b'\0') + bytes(0xa0))


class ProvisionalVersionSelection(unittest.TestCase):
    def test_stale_base_profile_never_silently_ignores_update(self):
        with self.assertRaisesRegex(nsp.BuildError, "profile-out.*Refusing to silently"):
            nsp.provisional_version({"v0": None, "v983040": None}, {"v0": {}}, "", Path("cached-profile"))

    def test_explicit_base_selection_is_preserved(self):
        self.assertEqual(nsp.provisional_version({"v0": None, "v983040": None}, {"v0": {}}, "v0", Path("cached-profile")), "v0")

    def test_selected_update_must_be_available(self):
        with self.assertRaisesRegex(nsp.BuildError, "not in the input folder"):
            nsp.provisional_version({"v0": None}, {"v0": {}, "v983040": {}}, "v983040", Path("cached-profile"))

    def test_current_profile_uses_numeric_newest_input(self):
        self.assertEqual(nsp.provisional_version({"v0": None, "v9": None, "v10": None}, {"v0": {}, "v9": {}, "v10": {}}, "", Path("cached-profile")), "v10")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        self.src = self.tmp / 'games' / f'{TID:016X}'; self.src.mkdir(parents=True)
        (self.src / 'game.tsv').write_text('\n'.join([f'title_id\t{TID:016X}', f'program_id\t{TID:016X}', 'name\tTest Game', 'modules\tmain', 'app_category\tpublic.app-category.games',
                                                      'romfs_marker\ta/b', f'build\tv0\t1.0.0\tmain={BID}', 'package\tv0', 'shader_versions\tv0']) + '\n')
        (self.src / 'shader_map.tsv').write_text('# registered\npkg.bin\t0\tvertex\tregistered\tx\n')
        (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 1}\n')
        (self.src / 'pso_manifest.tsv').write_text('S\tk\tvertex\n')
        (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "old"\n')
        self.ex = self.tmp / 'extract/v65536'; nso(self.ex / 'ExeFS/main', BID2); control(self.ex / 'control.nacp', '1.1.0')
        (self.ex / 'RomFS').mkdir(); (self.ex / 'RomFS/a.bin').write_bytes(b'romfs')
        self.pdir = self.tmp / 'derived' / f'{TID:016X}'
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def derive(self, ver='v65536', mods=('main',), app=None):
        return nsp.derive_profile(game.Game(self.src), self.pdir, ver, self.ex, list(mods), app)

    def redump(self, path):   # identical bytes and size, but a fresh extraction or capture timestamp
        before = path.stat()
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns + 1_000_000_000))


class DeriveProfile(Base):
    def later_stages(self):   # what refine_after_rewrite and the shader split write into the derived profile
        nsp.set_keys(game.Game(self.pdir), {'mem_used': ('0x4800000', 'rewritten modules')}, 'After the rewrite')
        (self.pdir / 'shader_map.tsv').write_text('# split\npkg.bin\t0\tvertex\tderived\tx\n')

    def assert_kept(self):
        self.assertEqual(game.Game(self.pdir).get('mem_used'), '0x4800000')
        self.assertIn('derived', (self.pdir / 'shader_map.tsv').read_text())

    def assert_fresh(self):
        self.assertNotIn('mem_used', [k for k, _ in game.Game(self.pdir).rows])
        self.assertFalse((self.pdir / 'shader_map.tsv').exists())

    def test_unchanged_inputs_keep_later_stages(self):
        self.assertEqual(self.derive(), '1.1.0')
        self.later_stages()
        self.assertEqual(self.derive(), '1.1.0')
        self.assert_kept()
        self.assertEqual(game.Game(self.pdir).builds()['v65536']['main'], BID2)
        self.assertEqual(self.derive(), '1.1.0'); self.assert_kept()   # still kept on a third run

    def test_a_module_the_version_does_not_ship_is_left_out(self):   # Mario Kart World 1.0.0 has no subsdk1, which the 1.8.0 profile loads
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain subsdk1 sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID)
        self.derive(mods=('main', 'subsdk1', 'sdk'))
        g = game.Game(self.pdir)
        self.assertEqual(g.words('modules'), ['main', 'sdk'])
        self.assertIn('# this version ships no subsdk1', (self.pdir / 'game.tsv').read_text())
        self.assertNotIn('subsdk1', g.builds()['v65536'])
        nso(self.ex / 'ExeFS/subsdk1', BID2); self.derive(mods=('main', 'subsdk1', 'sdk'))   # a version that ships it keeps the profile's list
        self.assertEqual(game.Game(self.pdir).words('modules'), ['main', 'subsdk1', 'sdk'])

    def test_the_same_profile_in_another_checkout_keeps_later_stages(self):
        self.derive(); self.later_stages()
        other = self.tmp / 'checkout2' / 'games' / f'{TID:016X}'; shutil.copytree(self.src, other)   # the repository cloned again, or a front end's own copy of it
        nsp.derive_profile(game.Game(other), self.pdir, 'v65536', self.ex, ['main'])
        self.assert_kept()
        self.assertNotIn(str(self.tmp), (self.pdir / 'game.tsv').read_text())

    def test_changed_inputs_derive_again(self):
        changes = {
            'source game.tsv': lambda: (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'pad_layout\tfull\n'),
            'source manual table': lambda: (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 2}\n'),
            'source chain header': lambda: (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "new"\n'),
            'new source file': lambda: (self.src / 'bf_want.tsv').write_text('# new\n'),
            'main build': lambda: nso(self.ex / 'ExeFS/main', 'ef' * 16),
            'display version': lambda: control(self.ex / 'control.nacp', '1.2.0'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                change(); self.derive(); self.assert_fresh()
                for p in self.src.iterdir():
                    if p.name not in nsp.DERIVED_SKIP: self.assertEqual((self.pdir / p.name).read_bytes(), p.read_bytes())
        self.assertEqual(game.Game(self.pdir).builds()['v65536'], {'display': '1.2.0', 'main': 'ef' * 16})

    def test_changed_arguments_derive_again(self):
        nso(self.ex / 'ExeFS/sdk', BID)   # in the build row only when the modules name it
        for what, kw in (('version', {'ver': 'v131072'}), ('modules', {'mods': ('main', 'sdk')}), ('title', {'app': 0x0100AAAA00FFF000})):
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                self.derive(**kw); self.assert_fresh()
        self.assertEqual(game.Game(self.pdir).title_id, 0x0100AAAA00FFF000)

    def test_removed_source_file_goes(self):
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        (self.src / 'pso_manifest.tsv').unlink(); self.derive()
        self.assertFalse((self.pdir / 'pso_manifest.tsv').exists()); self.assert_fresh()

    def test_edited_copy_is_kept_until_inputs_change(self):   # the stored digest is of the inputs, not of the derived files
        self.derive(); (self.pdir / 'pso_manifest.tsv').write_text('S\tk2\tpixel\n'); self.derive()
        self.assertEqual((self.pdir / 'pso_manifest.tsv').read_text(), 'S\tk2\tpixel\n')

    def test_shader_only_derivation_preserves_builds_and_registered_patch_file(self):
        source = self.src / 'game.tsv'
        source.write_text(source.read_text() + f'build\tv65536\t1.1.0\tmain={BID2}\nshader_versions\tv0 v65536\n')
        patch = self.src / 'patches.py'; patch.write_text(f'CUTS = {{"{BID}": []}}\n')
        g = game.Game(self.src)
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        derived = game.Game(self.pdir)
        self.assertEqual(derived.rows, g.rows)
        self.assertEqual(derived.builds(), g.builds())
        self.assertTrue((self.pdir / 'patches.py').is_symlink())
        self.assertEqual((self.pdir / 'patches.py').resolve(), patch.resolve())
        derived.table('shader_map.tsv').write_text('generated DLC shader rows\n')
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual(derived.table('shader_map.tsv').read_text(), 'generated DLC shader rows\n')

    def test_derived_main_without_frame_step_stubs_keeps_the_games_step(self):
        source = self.src / 'game.tsv'; source.write_text(source.read_text() + 'fps_target\t60\n')
        (self.src / 'patches.py').write_text(f'def frame_step(ctx): pass\nFRAME_STEP = {{"{BID}": frame_step}}\n')   # stubs for the registered main only, never run
        self.derive()
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '0')
        self.assertIn('frame step stubs for other builds only', (self.pdir / 'game.tsv').read_text())
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1, "{BID2}": 2}}\n')   # the derived main has stubs too
        self.derive(); self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text('CUTS = {}\n'); self.derive()   # no frame step in the profile
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1}}\n'); self.derive()
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], shader_only=True)   # shader tables only: the settings stay
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')

    def test_missing_corrupt_and_duplicate_override_rows_derive_again(self):
        for change in ('missing', 'corrupt', 'override', 'indented override', 'truncated'):
            with self.subTest(change):
                self.derive(); p = self.pdir / 'game.tsv'; original = p.read_text()
                if change == 'missing': broken = original.replace('romfs_marker\ta/b\n', '')
                elif change == 'corrupt': broken = original.replace('romfs_marker\ta/b', 'romfs_marker\tx/y')
                elif change == 'override': broken = 'romfs_marker\tx/y\n' + original
                elif change == 'indented override': broken = ' romfs_marker\tx/y\n' + original
                else: broken = original[:original.index('romfs_marker')]
                p.write_text(broken)
                self.derive()
                self.assertEqual(p.read_text(), original)

    def test_repeated_source_rows_and_computed_additions_are_kept(self):
        p = self.src / 'game.tsv'; p.write_text(p.read_text() + 'extra\tone\nextra\tone\n')
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        p = self.pdir / 'game.tsv'; p.write_text(p.read_text().replace('extra\tone\n', '', 1))
        self.derive()
        self.assertEqual(game.Game(self.pdir).values('extra'), [['one'], ['one']])

    def test_provisional_computed_mem_used_grows_shrinks_and_stays_stable(self):
        self.derive(); g = game.Game(self.pdir)
        # These reason comments were written by earlier versions of this script.
        for reason in ('the rewritten modules are larger than the default 64 MB of used memory (Age of Calamity: 66.6 MB → 72 MB)',
                       'used memory for the rewritten modules (above the default 64 MB)',
                       'used memory for the current rewritten modules, at least the default 64 MB'):
            g = nsp.set_keys(g, {'mem_used': ('0x4800000', reason)}, 'After the rewrite')
            with mock.patch.object(nsp.pg, 'mem_used') as used:
                for value in (0x5000000, None, 0x4800000):
                    used.return_value = value
                    g = nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched')
                    self.assertEqual(int(g.get('mem_used'), 0), value or 0x4000000)
                    before = g.table('game.tsv').read_bytes()
                    nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched')
                    self.assertEqual(g.table('game.tsv').read_bytes(), before)

    def test_explicit_mem_used_without_registered_profile_is_kept(self):
        self.derive(); p = self.pdir / 'game.tsv'; p.write_text(p.read_text() + 'mem_used\t0x5800000\n')
        g = game.Game(self.pdir)
        with mock.patch.object(nsp.pg, 'mem_used', side_effect=AssertionError('explicit value must win')):
            self.assertIs(nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched'), g)
            self.assertIs(nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched', game.Game(self.src)), g)

    def test_computed_equal_numeric_value_keeps_bytes(self):
        self.derive()
        g = nsp.set_keys(game.Game(self.pdir), {'mem_used': ('75497472', 'used memory for the current rewritten modules, at least the default 64 MB')}, 'After the rewrite')
        before = g.table('game.tsv').read_bytes()
        with mock.patch.object(nsp.pg, 'mem_used', return_value=0x4800000) as used:
            nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched')
        used.assert_called_once()
        self.assertEqual(g.table('game.tsv').read_bytes(), before)

    def test_derived_replace_failure_preserves_original(self):
        self.derive(); p = self.pdir / 'game.tsv'; before = p.read_bytes()
        with mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'): self.derive(ver='v131072')
        self.assertEqual(p.read_bytes(), before)

    def test_set_keys_replace_failure_preserves_original(self):
        self.derive(); g = game.Game(self.pdir); before = g.table('game.tsv').read_bytes()
        with mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'):
                nsp.set_keys(g, {'mem_used': ('0x4800000', 'rewritten modules')}, 'After the rewrite')
        self.assertEqual(g.table('game.tsv').read_bytes(), before)
        self.assertEqual(sorted(p.name for p in self.pdir.iterdir() if p.name.startswith('.game.tsv')), [])

    def test_heap_stamp_replace_failure_preserves_original(self):
        self.derive(); g = game.Game(self.pdir); before = g.table('game.tsv').read_bytes()
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(*args): (logs / 'heap_check.log').write_text('>>> nnMain\n'); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), \
             mock.patch.object(nsp.pg, 'heap_signals', return_value={}), mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'):
                nsp.heap_check(REPO, self.ex, self.tmp / 'patched', self.tmp / 'work', {}, logs, g)
        self.assertEqual(g.table('game.tsv').read_bytes(), before)

    def check_boot(self, name, log):   # a check on the derived profile, whose boot writes this log; the profile after it
        logs = self.tmp / 'logs'; logs.mkdir(exist_ok=True); patched = self.tmp / 'work/patched_v65536'
        def boot(*args): (logs / f'{name}_check.log').write_text(log); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
            if name == 'heap': return nsp.heap_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
            return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))

    def test_a_check_boot_sets_the_depth_mode_the_game_calls(self):   # before the prebuild reads it (Super Mario RPG: 0 of 34 lookups hit an archive made at the default mode)
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x80000000\nmem_reserve\t0x140000000\nmem_stack\t0x10000000\napp_pool\t0x40000000\n')
        for name in ('heap', 'region'):
            with self.subTest(check=name):
                self.derive()
                self.assertEqual(self.check_boot(name, '>>> nnMain\n').get('depth_mode', 'none'), 'none')   # no call: unknown, not GL
                g = self.check_boot(name, '>>> nnMain\n[dm] [nvn] depth mode near=0 (as D3D / Metal)\n')
                self.assertEqual(g.get('depth_mode'), 'zero')
                self.assertEqual(g.get('mem_alias'), '0x80000000')   # the keys around it stay
                g = self.check_boot(name, '>>> nnMain\n')   # a later boot that stops earlier keeps what an earlier one saw
                self.assertEqual(g.get('depth_mode'), 'zero')
                self.assertEqual([k for k, _ in g.rows].count('depth_mode'), 1)
                (self.pdir / 'game.tsv').unlink()   # the next check derives afresh

    def test_depth_mode_is_no_shader_input(self):   # a check that sets it must not schedule the game's shaders again
        self.derive(); before = dmsh_chain.inputs(game.Game(self.pdir))
        g = nsp.set_keys(game.Game(self.pdir), {'depth_mode': ('zero', 'seen')}, 'Address region check')
        self.assertEqual(dmsh_chain.inputs(g), before)

    def test_a_provisional_profile_moves_to_dm_games_with_its_shader_inputs(self):   # Super Mario RPG: "(provisional profile)" in the note scheduled all 82,032 libraries again for identical shaders
        F = {'romfs': {'dirs': collections.Counter(), 'extb': {}, 'exts': {}, 'extc': {}, 'biggest': (0, 'big.bin')}, 'sdk': '14', 'program': None, 'mods': {'main': {'bid': BID}}}
        pdir = self.tmp / 'provisional' / f'{TID:016X}'
        with mock.patch.object(nsp.pg, 'guess', return_value={'modules': ('main', '')}):
            nsp.write_profile(pdir, TID, {'F': F, 'name': 'Some Game', 'survey': 'survey.md', 'version': 'v0', 'display': '1.0.0'})
        g = game.Game(pdir)
        self.assertEqual(g.get('shader_note'), 'Some Game shaders')
        self.assertIn('Provisional game profile', g.table('game.tsv').read_text())   # the file still says what it is, in a comment
        moved = self.tmp / 'moved' / f'{TID:016X}'; shutil.copytree(pdir, moved)
        text = [l for l in (moved / 'game.tsv').read_text().splitlines() if not l.startswith('#')]   # a registered profile's own comments
        (moved / 'game.tsv').write_text('\n'.join(text) + '\n')
        self.assertEqual(dmsh_chain.inputs(game.Game(moved)), dmsh_chain.inputs(g))

    def test_region_stamp_follows_the_rewrite(self):   # a derived build is checked once per rewrite: a new cut checks again, a reused derivation keeps the stamp
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x80000000\n')   # every profile has one (the host is built with it)
        self.derive(); patched = self.tmp / 'work/patched_v65536'
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(*args): (logs / 'region_check.log').write_text('>>> nnMain\n'); (logs / 'regions.tsv').write_text('# table\n'); return 0
        def check(key):
            patched.with_name(patched.name + '.key').parent.mkdir(parents=True, exist_ok=True); patched.with_name(patched.name + '.key').write_text(key + '\n')
            with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
                return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
        g = check('a' * 64)
        self.assertTrue(nsp.region_checked(g, patched))
        self.derive(); self.assertTrue(nsp.region_checked(game.Game(self.pdir), patched))   # the derivation's inputs are unchanged: its stamp stays
        patched.with_name(patched.name + '.key').write_text('b' * 64 + '\n')
        self.assertFalse(nsp.region_checked(game.Game(self.pdir), patched))
        g = check('b' * 64)
        stamps = [l for l in g.table('game.tsv').read_text().splitlines() if l.startswith('# address regions: ')]
        self.assertEqual(len(stamps), 1); self.assertTrue(stamps[0].endswith(f'(rewrite {"b" * 12})'))
        self.assertNotIn('\n\n\n', g.table('game.tsv').read_text())

    def test_region_check_sets_stdalloc_cap(self):   # set when the regions need it, worked out again by a later check, never over a value set by hand
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x118000000\n')
        self.derive(); patched = self.tmp / 'work/patched_v65536'; key = patched.with_name(patched.name + '.key')
        key.parent.mkdir(parents=True); key.write_text('a' * 64 + '\n')
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(*args): (logs / 'region_check.log').write_text('>>> nnMain\n'); (logs / 'regions.tsv').write_text('0x4000000\t0xa00000000\tmain+0x10\t0\t-\t-\t0\n'); return 0
        def check(need):
            with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), \
                 mock.patch.object(nsp.pg, 'stdalloc_cap_needed', return_value=need) if not isinstance(need, Exception) else mock.patch.object(nsp.pg, 'stdalloc_cap_needed', side_effect=need):
                return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
        g = check(0xa4000000)
        self.assertEqual(g.get('stdalloc_cap'), '0xa4000000'); self.assertTrue(nsp.checked_cap(g))
        self.derive(); self.assertEqual(game.Game(self.pdir).get('stdalloc_cap'), '0xa4000000')   # a reused derivation keeps it
        g = check(0xa2000000); self.assertEqual(g.get('stdalloc_cap'), '0xa2000000')
        self.assertEqual(sum(l.startswith('stdalloc_cap\t') for l in g.table('game.tsv').read_text().splitlines()), 1)
        g = check(None); self.assertFalse(g.values('stdalloc_cap'))   # no longer needed: the value it set goes
        g = nsp.set_keys(g, {'stdalloc_cap': ('0xa0000000', 'by hand')}, 'Memory')
        g = check(AssertionError('a value set by hand is checked, not worked out again'))
        self.assertEqual(g.get('stdalloc_cap'), '0xa0000000')

    def test_rewrite_changes_update_auto_mem_used(self):
        self.derive(); g = game.Game(self.pdir); reg = game.Game(self.src)
        with mock.patch.object(nsp.pg, 'mem_used') as used:
            for value in (0x4800000, 0x5000000, None):
                used.return_value = value
                g = nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched', reg)
                self.assertEqual(int(g.get('mem_used'), 0), value or 0x4000000)
                before = g.table('game.tsv').read_bytes()
                self.derive(); g = game.Game(self.pdir)
                g = nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched', reg)
                self.assertEqual(g.table('game.tsv').read_bytes(), before)

    def test_explicit_mem_used_is_kept_and_equal_values_do_not_rewrite(self):
        self.derive(); g = nsp.set_keys(game.Game(self.pdir), {'mem_used': ('75497472', '')}, 'After the rewrite')
        before = g.table('game.tsv').read_bytes()
        with mock.patch.object(nsp.pg, 'mem_used', return_value=0x4800000):
            nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched', game.Game(self.src))
        self.assertEqual(g.table('game.tsv').read_bytes(), before)
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_used\t0x5800000\n')
        self.derive(); g = game.Game(self.pdir)
        with mock.patch.object(nsp.pg, 'mem_used', side_effect=AssertionError('explicit value must win')):
            self.assertIs(nsp.refine_after_rewrite(g, self.ex, self.tmp / 'patched', game.Game(self.src)), g)
        self.assertEqual(g.get('mem_used'), '0x5800000')

    def test_provisional_run_invalidates_derivation(self):
        self.derive()
        key = nsp.derived_key(self.pdir)
        self.assertTrue(key.is_file())
        nsp.set_keys(game.Game(self.pdir), {'mem_alias': ('0x80000000', '')}, 'Heap check')
        keys = self.tmp / 'prod.keys'; keys.write_text('test')
        fam = {'base': self.tmp / 'base.nsp', 'updates': [], 'dlc': []}
        args = nsp.parse_args([str(self.tmp), '-k', str(keys), '--platform', 'mac', '--provisional', '--profile-out', str(self.pdir.parent)])
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'apple_silicon', return_value=True), \
             mock.patch.object(nsp, '_importable', return_value=True), mock.patch.object(nsp.shutil, 'which', return_value='/dotnet'), \
             mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), mock.patch.object(nsp, 'decompress_nsz', return_value=fam), \
             mock.patch.object(nsp, 'nsp_versions', side_effect=RuntimeError('profile selected')):
            with self.assertRaisesRegex(RuntimeError, 'profile selected'): nsp.main(args)
        self.assertFalse(key.exists())
        self.derive()
        self.assertFalse(game.Game(self.pdir).values('mem_alias'))


class DlcOnlyShaders(Base):
    def test_only_dlc_rows_and_facts_can_change(self):
        tables = self.tmp / 'new-tables'; tables.mkdir()
        base = 'pkg.bin\t8\tcompute\tbase\t/base.shader\n'
        dlc = 'pkg.bin\t0\tvertex\tdlc\t/aoc/5/new.shader\n'
        old_facts = 'base\t{"local":[8,1,1]}\ndlc\t{"other":1}\n'
        self.src.joinpath('shader_map.tsv').write_text(base + dlc)
        self.src.joinpath('shader_facts.tsv').write_text(old_facts)
        g = game.Game(self.src)
        cases = [
            ('removed DLC and reindexed base', base.replace('\t8\t', '\t0\t'), 'base\t{"local":[8,1,1]}\n', True),
            ('added DLC', base + dlc + 'pkg.bin\t9\tpixel\tnew-dlc\t/aoc/7/new.shader\n', old_facts, True),
            ('shaderless DLC', base + dlc, old_facts, False),
            ('changed base key', base.replace('\tbase\t', '\tother\t'), old_facts, False),
            ('changed base source', base.replace('/base.shader', '/different.shader'), old_facts, False),
            ('changed base facts', base, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('no DLC difference', base + dlc, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('invalid map', 'not a shader map\n', old_facts, False),
        ]
        for name, shader_map, facts, want in cases:
            with self.subTest(name=name):
                tables.joinpath('shader_map.tsv').write_text(shader_map)
                tables.joinpath('shader_facts.tsv').write_text(facts)
                self.assertEqual(nsp.dlc_only_shader_change(g, tables), want)


class ComputeReaderAdditions(Base):
    V, P, C = 'a' * 16, 'b' * 16, 'c' * 16
    SOURCE = '/payload#Skinning#run.0'

    def setUp(self):
        super().setUp()
        self.tables = self.tmp / 'new-tables'; self.tables.mkdir()
        self.old = {'shader_map.tsv': f'vs_data.bin\t0\tvertex\t{self.V}\t/original-vs\nps_data.bin\t0\tpixel\t{self.P}\t/original-ps\n',
                    'shader_facts.tsv': f'{self.V}\t{{"manual":1}}\n', 'shader_pairs.tsv': f'{self.V}\t{self.P}\n',
                    'pso_pairs.tsv': '# no supplemental pairs\n'}
        self.new = dict(self.old)
        self.new['shader_map.tsv'] += f'cs_data.bin\t0\tcompute\t{self.C}\t{self.SOURCE}\n'
        self.new['shader_facts.tsv'] += f'{self.C}\t{{"local":[8,1,1],"shared":0}}\n'
        self.log = 'unity compute source: ' + json.dumps([self.C, self.SOURCE]) + '\n'
        for n, text in self.old.items(): (self.src / n).write_text(text)

    def check(self, tables=None, log=None):
        for n, text in (self.new if tables is None else tables).items(): (self.tables / n).write_text(text)
        return nsp.compute_reader_additions(game.Game(self.src), self.tables, self.log if log is None else log)

    def test_only_new_reader_proven_compute_kernels_qualify(self):
        self.assertTrue(self.check())
        changed = dict(self.new, **{'shader_map.tsv': self.new['shader_map.tsv'].replace('\t0\tvertex', '\t9\tvertex')})
        self.assertTrue(self.check(changed))  # Existing package indices may move; their identities cannot.
        changed['pso_pairs.tsv'] = f'{self.V}\t{self.P}\n'
        self.assertTrue(self.check(changed))  # Supplemental pass covers may be added beside the compute reader.
        for what, tables, log in (
            ('no additions', self.old, self.log),
            ('no reader evidence', self.new, ''),
            ('different source evidence', self.new, self.log.replace(self.SOURCE, '/other')),
            ('malformed evidence', self.new, 'unity compute source: broken\n'),
            ('wrong evidence type', self.new, 'unity compute source: {"key":"source"}\n'),
            ('graphics addition', dict(self.new, **{'shader_map.tsv': self.new['shader_map.tsv'].replace('\tcompute\t', '\tvertex\t')}), self.log),
            ('missing new facts', dict(self.new, **{'shader_facts.tsv': self.old['shader_facts.tsv']}), self.log),
            ('extra facts', dict(self.new, **{'shader_facts.tsv': self.new['shader_facts.tsv'] + 'other\t{}\n'}), self.log),
        ):
            with self.subTest(what=what): self.assertFalse(self.check(tables, log))

    def test_original_shaders_facts_and_pairs_cannot_change(self):
        for what, name, text in (
            ('removed shader', 'shader_map.tsv', self.new['shader_map.tsv'].split('\n', 1)[1]),
            ('changed key', 'shader_map.tsv', self.new['shader_map.tsv'].replace(self.V, 'd' * 16)),
            ('changed source', 'shader_map.tsv', self.new['shader_map.tsv'].replace('/original-vs', '/other')),
            ('changed package', 'shader_map.tsv', self.new['shader_map.tsv'].replace('vs_data.bin', 'vs_other.bin')),
            ('changed stage', 'shader_map.tsv', self.new['shader_map.tsv'].replace('\tvertex\t', '\tpixel\t')),
            ('duplicate key', 'shader_map.tsv', self.new['shader_map.tsv'] + self.old['shader_map.tsv']),
            ('changed fact', 'shader_facts.tsv', self.new['shader_facts.tsv'].replace('"manual":1', '"manual":2')),
            ('removed fact', 'shader_facts.tsv', self.new['shader_facts.tsv'].split('\n', 1)[1]),
            ('removed evidence pair', 'shader_pairs.tsv', '# none\n'),
            ('changed evidence pair', 'shader_pairs.tsv', f'{self.P}\t{self.V}\n'),
            ('compute in supplemental pair', 'pso_pairs.tsv', f'{self.V}\t{self.C}\n'),
        ):
            with self.subTest(what=what): self.assertFalse(self.check(dict(self.new, **{name: text})))
        (self.src / 'pso_pairs.tsv').write_text(f'{self.V}\t{self.P}\n')
        self.assertFalse(self.check())
        self.assertTrue(self.check(dict(self.new, **{'pso_pairs.tsv': f'{self.V}\t{self.P}\n'})))


class SplitShaders(Base):
    NAMES = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')

    def setUp(self):
        super().setUp()
        self.repo = self.tmp / 'repo'
        for f in ('dm/gfx/tools/shader_split.py', 'dm/gfx/tools/dmsh_chain.lock', 'dm/games/game.py', 'dm/ios/tools/ios_metal.py'):
            p = self.repo / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('v1')
        self.pkg = self.tmp / 'work/shader/pkg'; self.logs = self.tmp / 'logs'; self.logs.mkdir()
        self.splits, self.rows, self.log = 0, 'derived', 'shaders: 3 (3 vertex)\n'
        cm = mock.patch.object(nsp, 'run', self.split); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def split(self, cmd, log, env=None, cwd=None, on_line=None):   # shader_split.py: the package and the four tables, deterministic for the same rows
        self.splits += 1; self.last_cmd = cmd; out = Path(cmd[4]); out.mkdir(parents=True); (out / 'main.bin').write_bytes(self.rows.encode())
        for n, t in zip(self.NAMES, cmd[5:]): Path(t).write_text(self.table(n))
        Path(log).write_text(self.log)

    def table(self, n): return self.tables[n] if hasattr(self, 'tables') else f'# split header\n{n}\t{self.rows}\n'

    def go(self, g, own=True, oodle=None, dlc_roms=(), derive_dlc=None, derive_compute=None, env=None):
        return nsp.split_shaders(sys.executable, self.repo, [self.ex / 'RomFS'], [self.ex / 'ExeFS/main'], self.pkg, env or {}, self.logs, g, own, oodle, dlc_roms, derive_dlc, derive_compute)

    def compute_addition(self):
        self.tables = {'shader_map.tsv': 'vs_data.bin\t0\tvertex\tbase\t/original\ncs_data.bin\t0\tcompute\tadded\t/assets#Compute#run.0\n',
                       'shader_facts.tsv': 'added\t{"local":[8,1,1],"shared":0}\n', 'shader_pairs.tsv': '# no pairs\n', 'pso_pairs.tsv': '# none\n'}
        old = dict(self.tables, **{'shader_map.tsv': self.tables['shader_map.tsv'].split('\n', 1)[0] + '\n', 'shader_facts.tsv': '# none\n'})
        for n, text in old.items(): (self.src / n).write_text(text)
        self.log = 'unity compute source: ' + json.dumps(['added', '/assets#Compute#run.0']) + '\nshaders: 2 (1 vertex, 1 compute)\n'
        return old

    def test_compute_reader_derivation_preserves_profile_and_reuses_only_verified_inputs(self):
        old = self.compute_addition(); g = game.Game(self.src)
        patch = self.src / 'patches.py'; patch.write_text(f'CUTS = {{"{BID}": []}}\n')
        def derive():
            nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
            return game.Game(self.pdir)
        promote, dlc = mock.Mock(side_effect=derive), mock.Mock()
        first = self.go(g, own=False, derive_dlc=dlc, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (1, 1)); dlc.assert_not_called()
        self.assertTrue(self.pkg.with_name('pkg.compute.key').is_file())
        self.assertFalse(self.pkg.with_name('pkg.dlc.key').exists())
        derived = game.Game(self.pdir)
        self.assertEqual(derived.rows, g.rows)
        self.assertEqual((self.pdir / 'patches.py').resolve(), patch.resolve())
        for n in self.NAMES:
            self.assertEqual(g.table(n).read_text(), old[n])
            self.assertEqual(derived.table(n).read_text(), self.tables[n])
        self.assertEqual(self.go(g, own=False, derive_compute=promote), first)
        self.assertEqual((self.splits, promote.call_count), (1, 2))
        (self.repo / 'dm/gfx/tools/shader_split.py').write_text('reader_version = 2')
        self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (2, 3))
        verifier = self.tmp / 'comparison-v2.py'; verifier.write_text('changed comparison code')
        with mock.patch.object(nsp, '__file__', str(verifier)):
            self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (3, 4))
        g.table('shader_map.tsv').write_text(old['shader_map.tsv'].replace('/original', '/changed'))
        for _ in range(2):
            with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
                self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (5, 4))  # Stale proof cannot hide changes or failed checks.

    def test_compute_reader_derivation_rejects_missing_evidence_and_does_not_promote_own_profiles(self):
        self.compute_addition(); g = game.Game(self.src); promote = mock.Mock()
        self.log = 'shaders: 2 (1 vertex, 1 compute)\n'
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
            self.go(g, own=False, derive_compute=promote)
        promote.assert_not_called()
        own = self.own()
        self.go(own, derive_compute=promote)
        promote.assert_not_called()

    def test_dlc_only_derivation_is_incremental_and_keeps_registered_tables(self):
        base = 'other.bin\t1\tvertex\tbase\t/base.shader\n'
        old = {'shader_map.tsv': 'other.bin\t0\tvertex\tdlc\t/aoc/7/new.shader\n' + base,
               'shader_facts.tsv': 'base\t{"fact":1}\ndlc\t{"fact":2}\n',
               'shader_pairs.tsv': 'base\tdlc\n', 'pso_pairs.tsv': '# none\n'}
        for n in self.NAMES: (self.src / n).write_text(old[n])
        g = game.Game(self.src)
        self.tables = {'shader_map.tsv': base.replace('\t1\t', '\t0\t'), 'shader_facts.tsv': 'base\t{"fact":1}\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        def derive():
            nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
            return game.Game(self.pdir)
        promote = mock.Mock(side_effect=derive)
        first = self.go(g, own=False, derive_dlc=promote)
        self.assertEqual((self.splits, promote.call_count), (1, 1))
        self.assertEqual(self.go(g, own=False, derive_dlc=promote), first)
        self.assertEqual((self.splits, promote.call_count), (1, 2))   # proof of the strict non-DLC comparison is reused
        self.assertEqual(game.Game(self.pdir).rows, g.rows)
        for n in self.NAMES:
            self.assertEqual(g.table(n).read_text(), old[n])
            self.assertEqual(game.Game(self.pdir).table(n).read_text(), self.tables[n])
        # A new DLC extraction, even of the same file sizes, must perform the strict comparison again.
        dlc = self.tmp / 'aoc/7'; dlc.mkdir(parents=True); asset = dlc / 'shader'; asset.write_bytes(b'bytes')
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 2)
        self.redump(asset)
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 3)
        moved = dlc.with_name('8'); dlc.rename(moved)
        self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 4)   # index changes count even if every file statistic is identical
        verifier = self.tmp / 'comparison-v2.py'; verifier.write_text('changed comparison code')
        with mock.patch.object(nsp, '__file__', str(verifier)):
            self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 5)

    def test_base_shader_mismatch_never_derives_for_dlc(self):
        (self.src / 'shader_map.tsv').write_text('p.bin\t0\tvertex\tbase\t/base\np.bin\t1\tvertex\tdlc\t/aoc/7/shader\n')
        (self.src / 'shader_facts.tsv').write_text('# none\n')
        (self.src / 'shader_pairs.tsv').write_text('# none\n')
        self.tables = {'shader_map.tsv': 'p.bin\t0\tvertex\twrong-base\t/base\n', 'shader_facts.tsv': '# none\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        promote = mock.Mock()
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
            self.go(game.Game(self.src), own=False, derive_dlc=promote)
        promote.assert_not_called()

    def test_dlc_shaders_are_passed_and_invalidate_the_split(self):
        g = self.own(); self.go(g); self.assertEqual(self.splits, 1)
        dlc = self.tmp / 'work/aoc/7'; dlc.mkdir(parents=True)
        asset = dlc / 'shader.archive'; asset.write_bytes(b'new shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        self.assertEqual(self.last_cmd[-1], str(dlc))
        # Extra content has no main: the base RomFS/main pairing remains unchanged.
        self.assertEqual(self.last_cmd[2:4], [str(self.ex / 'RomFS'), str(self.ex / 'ExeFS/main')])
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        asset.write_bytes(b'updated DLC shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)   # removing the DLC removes its shaders from the union

    def own(self):   # a derived profile with the empty tables, as main makes it
        self.derive(); g = game.Game(self.pdir); nsp.provisional_prep(sys.executable, self.repo, {}, self.logs, g); return g

    def test_own_profile_reuses_and_returns_the_package_key(self):
        g = self.own()
        k1 = self.go(g)
        self.assertEqual((self.splits, k1), (1, nsp.package_key(self.pkg)))
        for n in self.NAMES: self.assertEqual(g.table(n).read_text(), self.table(n))
        k2 = self.go(g)
        self.assertEqual((self.splits, k2), (1, k1))   # the key stored after the tables were written: the second run sees the same inputs

    def test_translation_and_profile_metadata_changes_keep_extraction(self):
        g = self.own(); before = self.go(g)
        for name in ('dmsh_build.py', 'dmsh_chain.py', 'pso_offline.py', 'unrelated_reader.py'):
            (self.repo / 'dm/gfx/tools' / name).write_text('version = 2\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        for name in ('README.md', 'bf_want.tsv', 'shader_facts.tsv.notes', 'dmsh_chain.h'):
            g.table(name).write_text('changed\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        g = nsp.set_keys(g, {'mem_used': ('0x4800000', ''), 'nvn_all_pulling': ('1', '')}, 'Runtime settings')
        self.assertEqual(self.go(g), before)
        self.assertEqual(self.splits, 1)

    def test_transitive_reader_dependency_changes_reextract(self):
        (self.repo / 'dm/gfx/tools/shader_split.py').write_text('import reader\n')
        (self.repo / 'dm/gfx/tools/reader.py').write_text('from reader_bits import FORMAT\n')
        helper = self.repo / 'dm/tools/reader_bits.py'; helper.parent.mkdir(parents=True)
        helper.write_text('FORMAT = 1\n')
        g = self.own(); self.go(g)
        helper.write_text('FORMAT = 2\n')
        self.go(g); self.assertEqual(self.splits, 2)
        helper.unlink()
        self.go(g); self.assertEqual(self.splits, 3)

    def test_extraction_settings_and_auxiliary_modules_reextract(self):
        g = self.own(); self.go(g)
        for i, (key, value) in enumerate((('shader_groups', 'foo=bar'), ('shader_note', 'Other sources'), ('shader_key_skip', '0x770+0x78'), ('modules', 'main sdk')), 2):
            g = nsp.set_keys(g, {key: (value, '')}, 'Extraction setting')
            self.go(g); self.assertEqual(self.splits, i)
        nso(self.ex / 'ExeFS/sdk', BID)
        self.go(g); self.assertEqual(self.splits, 6)
        nso(self.ex / 'ExeFS/sdk', BID2)
        self.go(g); self.assertEqual(self.splits, 7)

    def test_capture_overrides_are_inputs(self):
        g = self.own()
        for setting, suffix in (('DM_GLSLC_CAPTURE', '.glslc'), ('DM_UE_SHADERS', '.ushaderbytecode')):
            with self.subTest(setting=setting):
                capture = self.tmp / setting; capture.mkdir()
                file = capture / ('a' + suffix); file.write_bytes(b'first shader')
                env = {setting: str(capture)}
                self.go(g, env=env); before = self.splits
                file.write_bytes(b'new shader')
                self.go(g, env=env); self.assertEqual(self.splits, before + 1)
                self.go(g, env=env); self.assertEqual(self.splits, before + 1)
                moved = self.tmp / f'{setting}-moved'; shutil.copytree(capture, moved)   # the same files elsewhere: its contents are the input, not its location
                self.go(g, env={setting: str(moved)}); self.assertEqual(self.splits, before + 1)

    def test_missing_or_modified_package_is_not_reused(self):
        g = self.own(); self.go(g)
        output = self.pkg / 'main.bin'
        output.unlink()
        self.go(g); self.assertEqual(self.splits, 2)
        self.assertEqual(output.read_bytes(), b'derived')
        output.write_bytes(b'corrupt')
        self.go(g); self.assertEqual(self.splits, 3)
        self.assertEqual(output.read_bytes(), b'derived')

    def test_verified_decoder_rebuild_reuses_but_binary_edits_do_not(self):
        g = self.own()
        host = self.tmp / 'decoder-host'; host.write_bytes(b'host with old chain')
        modules = self.tmp / 'patched'; modules.mkdir()
        module = modules / 'main.patched.bin'; module.write_bytes(b'code')
        def provenance():
            host.with_name(host.name + '.decoder.json').write_text(json.dumps({'format': 1, 'binary': nsp.fingerprint(('files', [host])), 'inputs': 'a' * 64}))
        decoder = lambda: {'DM_OODLE_HOST': str(host), 'DM_OODLE_MODULES': str(modules)}
        provenance(); self.go(g, oodle=decoder)
        host.write_bytes(b'host with new chain'); provenance()
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 1)
        host.write_bytes(b'externally replaced')   # the old provenance no longer describes these bytes
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 2)
        before = module.stat()
        module.write_bytes(b'edit'); os.utime(module, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 3)

    def test_redumped_inputs_invalidate_only_own_split(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        captures = [self.ex / 'glslc_capture/a.glslc', self.ex / 'ue_shaders/a.ushaderbytecode']
        for p in captures: p.parent.mkdir(); p.write_bytes(b'shader')
        for own in (False, True):
            g = self.own() if own else game.Game(self.src)
            self.go(g, own=own)
            for p in [self.ex / 'RomFS/a.bin', *captures]:
                with self.subTest(own=own, path=p):
                    before = self.splits; self.redump(p); self.go(g, own=own)
                    self.assertEqual(self.splits, before + int(own))
                    self.go(g, own=own); self.assertEqual(self.splits, before + int(own))

    def test_decoder_host_and_modules_invalidate_split(self):
        g = self.own()
        host = self.tmp / 'decoder-host'; host.write_bytes(b'host')
        modules = self.tmp / 'patched'; modules.mkdir()
        module = modules / 'main.patched.bin'; module.write_bytes(b'code')
        decoder = mock.Mock(return_value={'DM_OODLE_HOST': str(host), 'DM_OODLE_MODULES': str(modules)})
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 1)
        host.write_bytes(b'fixed host')
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 2)
        module.write_bytes(b'new rewritten module')
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 3)
        self.assertEqual(decoder.call_count, 6)

    def test_own_profile_changes_split_again(self):
        changes = {'manual table': lambda g: g.table('shader_facts_manual.tsv').write_text('k\t{"manual": 3}\n'),
                   'main': lambda g: nso(self.ex / 'ExeFS/main', 'ef' * 16),
                   'RomFS': lambda g: (self.ex / 'RomFS/b.bin').write_bytes(b'more'),
                   'tool': lambda g: (self.repo / 'dm/gfx/tools/shader_split.py').write_text('v2')}
        g = self.own(); self.go(g)
        for i, (what, change) in enumerate(changes.items(), 2):
            with self.subTest(what):
                change(g); self.assertEqual(self.go(g), nsp.package_key(self.pkg)); self.assertEqual(self.splits, i)
                self.go(g); self.assertEqual(self.splits, i)

    def test_own_generated_tables_are_checked(self):   # an edited or missing table of the split is never taken as is: the split's output comes back (split again, or restored)
        g = self.own(); self.go(g)
        for n in self.NAMES:
            with self.subTest(edited=n):
                g.table(n).write_text('# edited\nstale\trow\n'); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
            with self.subTest(missing=n):
                g.table(n).unlink(); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
        runs = self.splits; self.go(g); self.assertEqual(self.splits, runs)

    def test_own_new_output_replaces_tables(self):   # a changed input whose split writes other rows: the profile takes them
        g = self.own(); self.go(g)
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more'); self.go(g)
        self.assertEqual(g.table('shader_map.tsv').read_text(), self.table('shader_map.tsv'))
        self.go(g); self.assertEqual(self.splits, 2)

    def test_unreadable_files_are_reported(self):   # the split leaves out a file no reader reads and goes on: a provisional build says so and packs, a registered profile stops
        self.log = '[RED] 2 file(s) could not be read, their shaders are left out (first: /a.dict: not in the layout)\nshaders: 3 (3 vertex)\n'
        g, said = self.own(), []
        with mock.patch.object(nsp, 'say', said.append): self.go(g)
        self.assertEqual(len([m for m in said if m.startswith('[RED] 2 file(s) could not be read')]), 1)
        self.go(g); self.assertEqual(self.splits, 1)
        self.assertIn('2 file(s) unreadable', nsp.PROG.end.call_args[0][0])   # the reused split still names them
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        with self.assertRaisesRegex(nsp.BuildError, r'^2 file\(s\) could not be read'): self.go(game.Game(self.src), own=False)

    def test_mode_is_in_the_key(self):   # the same files split as an own profile, then checked against them as a registered one
        g = self.own(); self.go(g)
        self.assertEqual(self.go(g, own=False), nsp.package_key(self.pkg)); self.assertEqual(self.splits, 2)
        self.go(g, own=False); self.assertEqual(self.splits, 2)
        self.go(g); self.assertEqual(self.splits, 3)

    def test_registered_tables_are_compared(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(f'# older header\n{n}\t{self.rows}\n')
        g = game.Game(self.src)
        k = self.go(g, own=False)
        self.assertEqual((self.splits, k), (1, nsp.package_key(self.pkg)))
        self.assertEqual(self.go(g, own=False), k); self.assertEqual(self.splits, 1)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')   # a registered profile's tables are not written
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)   # a failed check is not reused
        self.assertEqual(self.splits, 3)

    def test_damaged_never_reuses(self):
        g = self.own()
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True):
            self.assertIsNone(self.go(g)); self.assertIsNone(self.go(g))
        self.assertEqual(self.splits, 2)
        self.go(g); self.assertEqual(self.splits, 3)   # a damaged run leaves no key behind

    def test_missing_split_pairs_are_generated_again(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        g = game.Game(self.src)
        self.go(g, own=False)
        pairs = self.pkg.with_name('pso_pairs.tsv'); pairs.unlink()
        self.go(g, own=False)
        self.assertEqual(self.splits, 2)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))
        pairs.write_text('corrupt\tpair\n')
        self.go(g, own=False)
        self.assertEqual(self.splits, 3)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))

    def test_derived_rerun_is_incremental(self):   # the whole derived path of a second run: derive, prepare, rewrite keys, split
        g = self.own(); g = nsp.set_keys(g, {'mem_used': ('0x4800000', '')}, 'After the rewrite'); k = self.go(g)
        g = self.own(); self.assertEqual(g.get('mem_used'), '0x4800000')
        self.assertEqual(self.go(g), k); self.assertEqual(self.splits, 1)


class RewriteWorkers(Base):
    def test_explicit_workers_override_the_automatic_module_budget(self):
        g = game.Game(self.src)
        for value, expected in (('1', '1'), ('3', '3'), ('', '14')):
            with self.subTest(value=value):
                out = self.tmp / f'patched-{value or "auto"}'
                env = {'DM_GAME': str(self.src), 'DM_REWRITE_J': value}
                with mock.patch.object(nsp, 'collect_nros', return_value={}), mock.patch.object(nsp, 'patch_files', return_value=[]), \
                     mock.patch.object(nsp.os, 'cpu_count', return_value=14), mock.patch.object(nsp.time, 'sleep'), \
                     mock.patch.object(nsp.subprocess, 'Popen', return_value=mock.Mock(returncode=0, poll=lambda: 0)) as popen:
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', out, env, self.tmp / 'logs', g)
                self.assertEqual(popen.call_args.kwargs['env']['DM_REWRITE_J'], expected)
                self.assertEqual(env['DM_REWRITE_J'], value)

    def test_invalid_workers_fail_before_collecting_modules(self):
        for value in ('0', '-1', 'many'):
            with self.subTest(value=value), mock.patch.object(nsp, 'collect_nros') as collect:
                with self.assertRaisesRegex(nsp.BuildError, 'DM_REWRITE_J must be a positive integer'):
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', self.tmp / 'patched',
                                {'DM_REWRITE_J': value}, self.tmp / 'logs', game.Game(self.src))
                collect.assert_not_called()


class LayerShaders(Base):
    def test_redumped_romfs_and_captures_invalidate_only_own_libraries(self):
        cap = self.ex / 'capture'; cap.mkdir(); (cap / 'shader.bin').write_bytes(b'shader')
        self.derive(); g = game.Game(self.pdir)
        logs = self.tmp / 'logs'; logs.mkdir(); runs = []
        def translate(cmd, log, **kw):
            api = Path(cmd[1]).stem.split('_')[0]
            lib = Path(cmd[-1]) / api / f'{api}_shaders.metallib'
            lib.parent.mkdir(parents=True); lib.write_bytes(b'library')
            Path(log).write_text('macosx: 1\n'); runs.append(api)
        with mock.patch.object(nsp, 'run', side_effect=translate), mock.patch.object(nsp, 'toolchain', return_value=()), \
             mock.patch.object(nsp.shutil, 'which', return_value='/glslangValidator'), \
             mock.patch.object(nsp.subprocess, 'run', return_value=mock.Mock(stdout='compiler', stderr='')):
            for api, build in (('vk', nsp.build_vk_shaders), ('gl', nsp.build_gl_shaders)):
                roots = [self.ex / 'RomFS', cap] + ([self.ex / 'ExeFS/main'] if api == 'gl' else [])
                for own in (False, True):
                    sh = self.tmp / f'{api}-{own}'
                    def go(): build(sys.executable, REPO, roots, sh, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=own)
                    go(); go()
                    for p in (self.ex / 'RomFS/a.bin', cap / 'shader.bin'):
                        with self.subTest(api=api, own=own, path=p):
                            before = len(runs); self.redump(p); go()
                            self.assertEqual(len(runs), before + int(own))
                            go(); self.assertEqual(len(runs), before + int(own))


class MacApp(Base):
    def test_packer_reports_reuse_and_first_pack_steps(self):
        t = self.tmp; out = t / 'Game.app'; packs = []
        def pack(cmd, log, env=None, on_line=None):
            if not on_line: return   # the launcher settings (dm/games/settings.py)
            packs.append(cmd)
            (out / 'Contents/MacOS').mkdir(parents=True, exist_ok=True)
            (out / 'Contents/MacOS/dmhost').write_bytes(b'host')
            if len(packs) == 1: on_line('step: launcher and host (clones of the game files)')
            on_line(f'app: {out}' + (', reused (inputs unchanged)' if len(packs) > 1 else ''))
        with mock.patch.object(nsp, 'run', side_effect=pack):
            for i in range(2):
                result = nsp.mac_app(sys.executable, REPO, out, TID, t, self.ex, t / 'patched', t / 'host', t / 'font', t / 'shader', None, 'vulkan', 'en-US',
                                     game.Game(self.src), 'Game', '1.0.0', 'org.nxnative.test', None, '', {}, t)
                self.assertIsNone(result)
                if i: nsp.PROG.end.assert_called_with('Game.app, reused (inputs unchanged)')
                else: nsp.PROG.set.assert_called_with(step='launcher and host (clones of the game files)')
        self.assertEqual(len(packs), 2)   # pack_app owns the reuse decision and sidecar key


class GlslcCapture(Base):
    def test_the_capture_boot_presses_a(self):   # Crash Team Racing Nitro-Fueled compiles its run-time programs only after its title screen has had a button press
        g = game.Game(self.src); ex = self.tmp / 'extract/v0'; (ex / 'RomFS').mkdir(parents=True); logs = self.tmp / 'logs'; logs.mkdir()
        out = bytearray(0x48); out[:4] = b'\x91\x68\x86\x19'; out[0x44:0x48] = (0x48).to_bytes(4, 'little')   # a whole GLSLC output: its magic and its own length
        seen = {}
        def boot(host, patched, henv, rd, log, secs, done):
            seen.update(henv); cap = Path(henv['DM_GLSLC_CAPTURE']); cap.mkdir(parents=True); (cap / '0123456789abcdef.glslc').write_bytes(out); return None
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), mock.patch.object(nsp, 'PROG'):
            nsp.capture_glslc(REPO, {'v0': ex}, 'v0', self.tmp / 'patched', self.tmp / 'work', {}, logs, g)
        self.assertEqual(seen['DM_KEYS'], nsp.GLSLC_KEYS)
        self.assertTrue(nsp.GLSLC_KEYS.startswith('5:A'))   # the first press comes well within the run (GLSLC_SECS)
        self.assertEqual(sorted(p.name for p in (ex / 'glslc_capture').iterdir()), ['0123456789abcdef.glslc'])


class GlslcCacheCoverage(Base):
    KEY = '0123456789abcdef'

    def capture(self, rc=None, observed=True, required=False, partial=False, capture_error=False):
        work, logs = self.tmp / 'work', self.tmp / 'logs'; logs.mkdir(exist_ok=True)
        def boot(host, patched, env, rd, log, secs, done):
            shmap = Path(env['DM_NVN_SHMAP'])
            if required:
                self.assertEqual(shmap, self.src / 'shader_map.tsv')
            else:
                self.assertEqual(shmap.read_text(), '# GLSLC capture: observe submitted shaders\n')
            if partial:
                cap = Path(env['DM_GLSLC_CAPTURE'])
                cap.mkdir()
                (cap / 'aaaaaaaaaaaaaaaa.glslc').write_bytes(b'partial output')
            text = f'[dm] [nvn-mtl] [RED] shader {self.KEY} is not in the shader map\n' if observed else ''
            if capture_error:
                text += '[dm] [RED] glslc: unknown output header\n'
            log.write_text(text)
            return rc
        with mock.patch.object(nsp, 'build_mac_host', return_value=work / 'dmhost'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
            return nsp.capture_glslc(REPO, {'v65536': self.ex}, 'v65536', work / 'patched', work, {}, logs, game.Game(self.src))

    def test_surviving_boot_defers_to_offline_coverage(self):
        observed = self.capture()
        self.assertEqual(observed, {self.KEY})
        (self.src / 'shader_map.tsv').write_text(f'pkg.bin\t0\tvertex\t{self.KEY}\t/cache#GLSLC@0x80\n')
        nsp.check_glslc_coverage(observed, game.Game(self.src))

    def test_empty_capture_still_fails_without_boot_evidence(self):
        for rc in (139, 0):
            with self.subTest(rc=rc), self.assertRaisesRegex(nsp.BuildError, 'captured only 0'):
                self.capture(rc=rc)
        for kwargs in ({'observed': False}, {'partial': True}, {'capture_error': True}):
            with self.subTest(**kwargs), self.assertRaisesRegex(nsp.BuildError, 'captured only 0'):
                self.capture(**kwargs)

    def test_registered_capture_requirements_are_not_relaxed(self):
        (self.src / 'shader_map.tsv').write_text(f'pkg.bin\t0\tvertex\t{self.KEY}\t/glslc_capture/aaaaaaaaaaaaaaaa.glslc#GLSLC@0x0\n')
        with self.assertRaisesRegex(nsp.BuildError, 'profile needs 1'):
            self.capture(required=True)

    def test_missing_offline_key_and_no_observations_fail(self):
        for observed, msg in (({self.KEY}, 'observed shaders absent'), (set(), 'no submitted shader keys')):
            with self.subTest(observed=observed), self.assertRaisesRegex(nsp.BuildError, msg):
                nsp.check_glslc_coverage(observed, game.Game(self.src))


if __name__ == '__main__': unittest.main()

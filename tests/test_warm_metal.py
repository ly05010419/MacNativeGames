"""Packaging-time Metal warm-up (build_app.step_warm_metal, games/totk/tools/warm_metal_cache.m)."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_app

TOOL_SOURCE = Path(build_app.GAME) / 'tools/warm_metal_cache.m'


class StepWarmMetalTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        self.tree, self.out, self.logs = root / 'tree', root / 'Game.app', root / 'logs'
        self.logs.mkdir()
        (self.tree / 'build').mkdir(parents=True)
        (self.tree / 'build/warm-metal-cache').write_text('tool')
        self.app_build = self.out / 'Contents/Resources/totk/build'
        self.app_build.mkdir(parents=True)
        self.app_tool = self.app_build / 'warm-metal-cache'
        self.said = []

    def tearDown(self):
        self.folder.cleanup()

    def step(self, game_running=False, reports=None, fail=False, users=(), users_after=()):
        """Run the step with pgrep, lsof and the tool replaced; reports: {log name: JSON object} the tool would print;
        users / users_after: the programs that have Metal's cache open before compiling / when the check failed."""
        calls = []
        cache_users = iter([list(users), list(users_after)])

        def run(cmd, log, **_kwargs):
            calls.append([str(c) for c in cmd])
            self.assertTrue(self.app_tool.is_file(), 'the tool runs from the game binary\'s folder')
            if fail:
                raise build_app.BuildError('warm-metal-cache failed (exit 2)')
            log.write_text('warm_metal_cache: 1000 / 8290 after 20 s\n' + json.dumps((reports or {}).get(log.name, {})) + '\n')

        def system(cmd, **_kwargs):
            if cmd[0] == 'pgrep':
                return mock.Mock(returncode=0 if game_running else 1)
            raise AssertionError(f'unexpected command {cmd}')

        with mock.patch.object(build_app.subprocess, 'run', side_effect=system), mock.patch.object(build_app, 'run', side_effect=run), \
             mock.patch.object(build_app, 'clone', side_effect=lambda src, dst: shutil.copyfile(src, dst)), \
             mock.patch.object(build_app, 'say', side_effect=self.said.append), \
             mock.patch.object(build_app, 'metal_cache_users', side_effect=lambda: next(cache_users)):
            build_app.step_warm_metal(self.tree, self.out, self.logs)
        return calls

    def test_skipped_while_a_game_is_running(self):
        self.assertEqual(self.step(game_running=True), [])
        self.assertIn("a game has Metal's shader cache open", self.said[-1])

    def test_skipped_while_another_program_has_the_cache_open(self):
        self.assertEqual(self.step(users=['llama-server (4242)']), [])
        self.assertIn("llama-server (4242) has Metal's shader cache open", self.said[-1])
        self.assertFalse(self.app_tool.exists())

    def test_runs_from_the_game_binarys_folder_and_leaves_nothing_there(self):
        calls = self.step(reports={
            'warm-metal-cache.log': {'files': 8290, 'compiled': 8290, 'skipped': 0, 'failed': 0, 'threads': 8, 'seconds': 170.0},
            'warm-metal-cache-verify.log': {'sampled': 64, 'hits': 64, 'median_ms': 0.3, 'max_ms': 0.9}})
        seed = str(self.app_build / 'shader-seed')
        self.assertEqual(calls, [[str(self.app_tool), seed], [str(self.app_tool), seed, '--verify', '64']])
        self.assertFalse(self.app_tool.exists(), 'the tool must not stay inside the signed app')
        self.assertIn("finds them in Metal's cache (64 of 64", self.said[-1])

    def test_says_so_when_metal_did_not_keep_them(self):
        self.step(reports={
            'warm-metal-cache.log': {'files': 8290, 'compiled': 8290, 'skipped': 0, 'failed': 0},
            'warm-metal-cache-verify.log': {'sampled': 64, 'hits': 3, 'median_ms': 61.0, 'max_ms': 95.0}})
        self.assertIn('Metal did not keep them (3 of 64', self.said[-1])
        self.assertIn('another program probably used', self.said[-1])

    def test_names_a_program_that_opened_the_cache_meanwhile(self):
        self.step(reports={
            'warm-metal-cache.log': {'files': 8290, 'compiled': 8290, 'skipped': 0, 'failed': 0},
            'warm-metal-cache-verify.log': {'sampled': 64, 'hits': 1, 'median_ms': 48.0, 'max_ms': 250.0}},
            users_after=['tool (777)'])
        self.assertIn("tool (777) has Metal's cache open now", self.said[-1])

    def test_a_failing_tool_does_not_fail_the_build_and_is_removed(self):
        self.step(fail=True)
        self.assertIn('the first start compiles them instead', self.said[-1])
        self.assertFalse(self.app_tool.exists())

    def test_a_tool_left_by_an_interrupted_build_is_removed(self):
        self.app_tool.write_text('left over')
        self.step(game_running=True)
        self.assertFalse(self.app_tool.exists())

    def test_last_json_takes_the_report_line(self):
        log = self.logs / 'x.log'
        log.write_text('{"old": 1}\nnoise {\n{"compiled": 2}\nwarm_metal_cache: done\n')
        self.assertEqual(build_app.last_json(log), {'compiled': 2})
        log.write_text('nothing\n')
        self.assertIsNone(build_app.last_json(log))



class MetalCacheUsersTests(unittest.TestCase):
    """metal_cache_users: lsof on the shared store's files, parsed into 'name (pid)'."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.cache = Path(self.folder.name)
        self.commands = []

    def tearDown(self):
        self.folder.cleanup()

    def users(self, lsof_output=''):
        def run(cmd, **_kwargs):
            self.commands.append(cmd)
            if cmd[0] == 'getconf':
                return mock.Mock(returncode=0, stdout=str(self.cache) + '/\n')
            if cmd[0] == 'lsof':
                return mock.Mock(returncode=0 if lsof_output else 1, stdout=lsof_output)
            raise AssertionError(f'unexpected command {cmd}')
        with mock.patch.object(build_app.subprocess, 'run', side_effect=run):
            return build_app.metal_cache_users()

    def test_no_store_no_lsof(self):
        self.assertEqual(self.users(), [])
        self.assertEqual([c[0] for c in self.commands], ['getconf'])

    def test_parses_the_programs_holding_it(self):
        store = self.cache / 'com.apple.metal/32024'
        store.mkdir(parents=True)
        for name in ('libraries.data', 'libraries.list'):
            (store / name).write_bytes(b'')
        found = self.users('p96998\ncmetal-cache-hold\nf4\nf5\nftxt\np100\nctotk-native\nf9\n')
        self.assertEqual(found, ['metal-cache-hold (96998)', 'totk-native (100)'])
        lsof = self.commands[-1]
        self.assertEqual(lsof[:4], ['lsof', '-F', 'pc', '--'])
        self.assertEqual(sorted(Path(f).name for f in lsof[4:]), ['libraries.data', 'libraries.list'])

    def test_nobody_holding_it(self):
        store = self.cache / 'com.apple.metal/32024'
        store.mkdir(parents=True)
        (store / 'libraries.data').write_bytes(b'')
        self.assertEqual(self.users(''), [])

@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class WarmMetalToolTests(unittest.TestCase):
    """The real tool on three small sources: one compiles, one the game skips (double), one does not compile."""

    def test_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            tool = root / 'warm-metal-cache'
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror', str(TOOL_SOURCE),
                                    '-framework', 'Metal', '-framework', 'Foundation', '-o', str(tool)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            seed = root / 'seed'
            seed.mkdir()
            (seed / 'a_b.metal').write_text('#include <metal_stdlib>\nusing namespace metal;\n'
                                            'kernel void mng_test_warm(device float *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = a[i] * 2.0f; }\n')
            (seed / 'b_b.metal').write_text('kernel void k(device double *a [[buffer(0)]]) {}\n')
            (seed / 'c_b.metal').write_text('this is not metal\n')
            (seed / 'a_b.json').write_text('{}')   # not a source: ignored
            r = subprocess.run([str(tool), str(seed), '2'], capture_output=True, text=True)
            if r.returncode == 2 and 'no Metal device' in r.stderr:
                self.skipTest('no Metal device')
            self.assertEqual(r.returncode, 0, r.stderr)
            report = json.loads(r.stdout.strip().splitlines()[-1])
            self.assertEqual((report['files'], report['compiled'], report['skipped'], report['failed']), (3, 1, 1, 1))
            v = subprocess.run([str(tool), str(seed), '--verify', '3'], capture_output=True, text=True)
            self.assertEqual(v.returncode, 0, v.stderr)
            self.assertEqual(json.loads(v.stdout.strip().splitlines()[-1])['sampled'], 1)


if __name__ == '__main__':
    unittest.main()


class MetalCompilerTests(unittest.TestCase):
    """The offline compiler comes with an Xcode's Metal toolchain; the selected developer folder may be the command line
    tools, so other Xcodes are tried and the one that has it is used to run it too."""

    def fake_run(self, found_in):
        def run(argv, capture_output=True, text=True, env=None):
            developer = (env or {}).get('DEVELOPER_DIR')
            if argv[:2] == ['xcrun', '--find']:
                ok = developer == found_in
                return subprocess.CompletedProcess(argv, 0 if ok else 1, stdout='/toolchain/metal\n' if ok else '', stderr='')
            return subprocess.CompletedProcess(argv, 0, stdout='Apple metal version 32023\n', stderr='')
        return run

    def test_an_xcode_outside_the_selected_folder_supplies_the_compiler(self):
        with mock.patch.object(build_app, 'developer_dirs', return_value=['/Library/Developer/CommandLineTools',
                                                                          '/Volumes/x/Xcode.app/Contents/Developer']), \
             mock.patch.object(build_app.subprocess, 'run', side_effect=self.fake_run('/Volumes/x/Xcode.app/Contents/Developer')), \
             mock.patch.dict(build_app.os.environ, {}, clear=False):
            build_app.os.environ.pop('DEVELOPER_DIR', None)
            metal, version, env = build_app.metal_compiler()
        self.assertEqual((metal, version, env['DEVELOPER_DIR']),
                         ('/toolchain/metal', 'Apple metal version 32023', '/Volumes/x/Xcode.app/Contents/Developer'))

    def test_native_4k_is_built_by_default_on_a_mac_with_16_gb_or_more(self):
        self.assertTrue(build_app.native_4k_default(16 << 30))   # what a 16 GB Mac reports
        self.assertTrue(build_app.native_4k_default(36 << 30))
        self.assertFalse(build_app.native_4k_default(8 << 30))

    def test_volume_search_finds_xcodes_and_does_not_follow_symlinks(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            for app in ('Volumes/Disk/Xcode.app', 'Volumes/Disk/Dev/Xcode-beta.app', 'root/home/Xcode.app'):
                (root/app).mkdir(parents=True)
            (root/'Volumes/Macintosh HD').symlink_to(root/'root')
            self.assertEqual(build_app.xcodes_on_volumes(root/'Volumes'),
                             [root/'Volumes/Disk/Xcode.app', root/'Volumes/Disk/Dev/Xcode-beta.app'])
            self.assertEqual(build_app.xcodes_on_volumes(root/'no such folder'), [])

    def test_no_toolchain_anywhere_is_none(self):
        with mock.patch.object(build_app, 'developer_dirs', return_value=['/Library/Developer/CommandLineTools']), \
             mock.patch.object(build_app.subprocess, 'run', side_effect=self.fake_run('nowhere')):
            self.assertIsNone(build_app.metal_compiler())

"""Everything a release carries runs on macOS 26, whatever Mac makes it (user 2026-11-26: "用测试保证 以后不准错 哪怕我用这个新机器
打包 26依然支持"; "除了 代码 一切不能和 1.1.4 保持不变").

Launcher 1.1.5 was the first release made on a macOS 27 Mac. Nothing set a minimum, so clang took the SDK's (27.2): the
game runtimes of its source (.prebuilt), the offline store's libraries, zstd and spirv-cross all said minos 27.2 and
macOS 26 refused to open the games (#2395); and libvpx's configure, which knows darwin25 (macOS 26) at most, fell back to
plain C without its NEON code. These tests compile on the Mac that runs them, so they fail on a macOS 27 Mac (or any SDK
newer than 26.0) the moment a minimum goes missing:

- every games/*/Makefile gives its shipped binaries (…-native, launcher) 26.0, read from make's own database;
- tools/prebuilt.py builds every program for 26.0, links the store's libraries and takes the recorded spirv-cross;
- tools/deps makes the libraries a game links for 26.0, and libvpx for its arm64 target (NEON), never generic-gnu;
- the offline store carries exactly tools/deps/carried_built.json's files (Launcher 1.1.4's);
- tools/macho_minos.py, which the release runs over the whole app, finds what needs a newer macOS."""
import importlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import macho_minos  # noqa: E402

MINIMUM = '26.0'
NEWER = '27.0'   # what a Mac newer than the minimum would give (the tests name it: an SDK 26.x Mac must fail too)
HAS_CLANG = shutil.which('xcrun') is not None and subprocess.run(['xcrun', '--find', 'clang'],
                                                                 capture_output=True).returncode == 0
MAKE = '/usr/bin/make'


def compile_c(out, minimum=None, env=None, source='int main(void){return 0;}\n', object_only=False):
    """A tiny Mach-O at out: with -mmacosx-version-min when minimum is given, else what env (or the SDK) says."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    src = out.with_suffix('.c')
    src.write_text(source)
    cmd = ['xcrun', 'clang', *(['-c'] if object_only else []), str(src), '-o', str(out)]
    if minimum:
        cmd.insert(2, f'-mmacosx-version-min={minimum}')
    subprocess.run(cmd, check=True, capture_output=True, env=env)
    src.unlink()
    return out


def sdk_default_is_newer():
    """clang with no minimum gives the SDK's version: newer than 26.0 on any Mac the release could be made on today."""
    with tempfile.TemporaryDirectory() as t:
        found = macho_minos.scan(compile_c(Path(t) / 'probe', env={k: v for k, v in os.environ.items()
                                                                     if k != 'MACOSX_DEPLOYMENT_TARGET'}))
    return macho_minos.version_tuple(found[0][1]) > macho_minos.version_tuple(MINIMUM)


# ---- tools/macho_minos.py ----
@unittest.skipUnless(HAS_CLANG, 'needs Xcode command line tools')
class MachoMinos(unittest.TestCase):
    def test_finds_a_program_a_library_member_and_a_file_in_a_tar(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            ok = compile_c(t / 'app/ok', MINIMUM)
            new = compile_c(t / 'app/new', NEWER)
            obj = compile_c(t / 'lib/f.o', NEWER, source='int f(void){return 1;}\n', object_only=True)
            subprocess.run(['xcrun', 'ar', 'rcs', str(t / 'app/libf.a'), str(obj)], check=True)
            with tarfile.open(t / 'app/store.tar', 'w') as tar:
                tar.add(new, arcname='offline/built/x/bin/new')
            self.assertEqual(macho_minos.scan(ok), [(str(ok), MINIMUM)])
            found = {what for what, m in macho_minos.too_new([t / 'app'])}
            self.assertEqual(found, {str(new), f'{t / "app/libf.a"}(f.o)', f'{t / "app/store.tar"}!offline/built/x/bin/new'})
            self.assertEqual(macho_minos.main([str(ok)]), 0)
            self.assertEqual(macho_minos.main([str(t / 'app')]), 1)

    def test_a_universal_binary(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            a, b = compile_c(t / 'a', MINIMUM), compile_c(t / 'b', NEWER)
            subprocess.run(['xcrun', 'lipo', '-create', str(a), '-output', str(t / 'fat')], check=True)
            self.assertEqual([m for _, m in macho_minos.scan(t / 'fat')], [MINIMUM])
            self.assertEqual(macho_minos.too_new([b]), [(str(b), NEWER)])


# ---- games/*/Makefile ----
TARGET = re.compile(r'^([^#\s][^:=]*?):(?!=)')
SHIPPED = re.compile(r'(^|/)([A-Za-z0-9_]+-native(-[A-Za-z0-9_.]+)?|launcher)$')


def make_database(game):
    """make's own view of games/<game>/Makefile (make -p): its variables, and each target's target-specific variables
    and recipe."""
    with tempfile.TemporaryDirectory() as work:
        out = subprocess.run([MAKE, '-C', str(ROOT / 'games' / game), '-p', '-n', '-q', f'WORK={work}',
                              '__mng_no_such_target__'], capture_output=True, text=True).stdout
    variables, targets, current, files = {}, {}, None, False
    for line in out.splitlines():
        if line.startswith('# Files'):
            files = True
        elif not files:
            m = re.match(r'^(?:export )?([A-Za-z_][A-Za-z0-9_]*) :?= (.*)$', line)
            if m:
                variables[m.group(1)] = m.group(2)
        elif not line:
            current = None
        elif line.startswith('\t'):
            if current is not None:
                current['recipe'].append(line[1:])
        elif TARGET.match(line):
            current = targets.setdefault(TARGET.match(line).group(1).strip(), {'vars': {}, 'recipe': []})
        elif current is not None:
            m = re.match(r'^# (\S+) :?= ?(.*)$', line)
            if m:
                current['vars'][m.group(1)] = m.group(2)
    return variables, targets


def expand(text, variables, depth=6):
    for _ in range(depth):
        text = re.sub(r'\$\((\w+)\)', lambda m: variables.get(m.group(1), ''), text)
    return text


def minimum_of(target, variables):
    """The macOS a shipped binary is linked for: its target-specific MACOSX_DEPLOYMENT_TARGET, or the
    -mmacosx-version-min its recipe passes; None when neither."""
    if 'MACOSX_DEPLOYMENT_TARGET' in target['vars']:
        return target['vars']['MACOSX_DEPLOYMENT_TARGET']
    m = re.search(r'-mmacosx-version-min=([0-9.]+)', expand(' '.join(target['recipe']), variables))
    return m.group(1) if m else None


@unittest.skipUnless(Path(MAKE).is_file(), 'needs make')
class MakefileMinimum(unittest.TestCase):
    def test_every_shipped_binary_is_linked_for_macos_26(self):
        """A new game's Makefile is checked too: anything named …-native or launcher that has a recipe."""
        seen, missing = 0, []
        for game in sorted(p.name for p in (ROOT / 'games').iterdir() if (p / 'Makefile').is_file()):
            variables, targets = make_database(game)
            for name, target in targets.items():
                if SHIPPED.search(name) and target['recipe']:
                    seen += 1
                    got = minimum_of(target, variables)
                    if got != MINIMUM:
                        missing.append(f'games/{game}: {Path(name).name} -> {got}')
        self.assertGreater(seen, 35)   # 2026-11-26: 41 binaries in 30 Makefiles
        self.assertEqual(missing, [], 'give these 26.0: a line `<target>: export MACOSX_DEPLOYMENT_TARGET := 26.0` '
                                      'before the rule (games/totk/Makefile)')

    def test_the_main_games_by_name(self):
        """The games the Launcher builds, named, so a renamed target cannot drop out of the scan above."""
        want = {'totk': ['build/totk-native', 'build/totk-native-v121', 'build/totk-native-v143',
                         'build/totk-native-botw160', 'build/totk-native-botw190', 'build/totk-native-dkb',
                         'build/totk-native-pkp'],
                'oot': ['oot-native'], 'acnh': ['acnh-native'], 'eow': ['eow-native'], 'odyssey': ['odyssey-native'],
                'dread': ['dread-native', 'launcher'], 'tmnt': ['tmnt-native']}
        for game, names in want.items():
            variables, targets = make_database(game)
            for name in names:
                found = [t for n, t in targets.items() if n == name or n.endswith('/' + name)]
                self.assertTrue(found, f'games/{game}: no target {name}')
                self.assertEqual(minimum_of(found[0], variables), MINIMUM, f'games/{game}: {name}')

    @unittest.skipUnless(HAS_CLANG, 'needs Xcode command line tools')
    def test_the_target_line_reaches_the_binary_and_its_objects_and_nothing_else(self):
        """The mechanism, compiled here: `<target>: export VAR := 26.0` sets the binary and what make builds for it, and
        leaves other targets (the translators') as they were."""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / 'f.c').write_text('int f(void){return 1;}\n')
            (t / 'm.c').write_text('int f(void); int main(void){return f();}\n')
            (t / 't.c').write_text('int main(void){return 0;}\n')
            (t / 'Makefile').write_text('game: export MACOSX_DEPLOYMENT_TARGET := 26.0\n'
                                        'game: f.o m.c\n\txcrun clang m.c f.o -o $@\n'
                                        'f.o: f.c\n\txcrun clang -c f.c -o $@\n'
                                        'tool: t.c\n\txcrun clang -mmacosx-version-min=15.0 t.c -o $@\n')
            env = {k: v for k, v in os.environ.items() if k != 'MACOSX_DEPLOYMENT_TARGET'}
            subprocess.run([MAKE, '-C', str(t), 'game', 'tool'], check=True, capture_output=True, env=env)
            got = {Path(w).name: m for p in ('game', 'f.o', 'tool') for w, m in macho_minos.scan(t / p)}
            self.assertEqual(got, {'game': MINIMUM, 'f.o': MINIMUM, 'tool': '15.0'})


# ---- tools/prebuilt.py ----
FAKE_BUILD_APP = r'''
"""A stand-in for build_app.py: every compile is a real one of a tiny program, with the environment build() gives it."""
import os, subprocess
from pathlib import Path

def _cc(out, env):
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True)
    src = out.with_name(out.name + '.c'); src.write_text('int main(void){return 0;}\n')
    subprocess.run(['xcrun', 'clang', str(src), '-o', str(out)], check=True, env=env)
    (Path(os.environ['FAKE_LOG']) / 'deps-cache.txt').write_text(os.environ.get('MNG_DEPS_CACHE', ''))

def codec_libraries(target='macos'):
    (Path(os.environ['FAKE_LOG']) / 'codec.txt').write_text(os.environ.get('MACOSX_DEPLOYMENT_TARGET', 'none'))

def run(cmd, log, cwd=None, env=None, quiet=True):
    cmd = [str(c) for c in cmd]
    env = dict(os.environ if env is None else env)
    if cmd[0] == 'make' and '-C' in cmd:
        tree, goal = Path(cmd[cmd.index('-C') + 1]), cmd[-1]
        if goal == 'runtime':
            work = Path(next(c for c in cmd if c.startswith('WORK=')).split('=', 1)[1])
            _cc(work / 'build/oot-native', env); _cc(work / 'build/pool-sources-list', env)
        else:
            _cc(tree / goal, env)
    elif cmd[0] == 'codesign':
        subprocess.run(cmd, check=True, capture_output=True)
    else:
        raise AssertionError(f'unexpected command {cmd}')

def build_spirv_cross(tree, logs):
    raise AssertionError('a release never compiles spirv-cross again')

def build_warm_tool(tree, logs, env):
    _cc(tree / 'build/warm-metal-cache', env)

def build_hactool(tree, logs):
    out = tree.parent / 'src/hactool/hactool'; _cc(out, dict(os.environ)); return out
'''


@unittest.skipUnless(HAS_CLANG, 'needs Xcode command line tools')
class PrebuiltPrograms(unittest.TestCase):
    """tools/prebuilt.py build(), its compiles real (a stand-in build_app compiles tiny programs with the environment
    build() gives them): on this Mac, whatever its SDK, every program comes out for 26.0."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.source = self.tmp / 'source'
        (self.source / 'games/totk').mkdir(parents=True)
        (self.source / 'games/oot').mkdir(parents=True)
        (self.source / 'games/totk/runtime').mkdir()
        (self.source / '.mng-source-commit').write_text('0' * 40 + '\n')
        (self.source / 'build_app.py').write_text(FAKE_BUILD_APP)
        self.log = self.tmp / 'log'
        self.log.mkdir()
        self.reference = self.tmp / 'reference'
        spirv = compile_c(self.reference / 'spirv-cross', MINIMUM, source='int main(void){return 2;}\n')
        self.prebuilt = importlib.import_module('tools.prebuilt')
        self.code = self.prebuilt._tool('translator_code').code_sha256(spirv)
        saved = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(saved)))
        os.environ.pop('MACOSX_DEPLOYMENT_TARGET', None)
        os.environ['FAKE_LOG'] = str(self.log)
        os.environ['MNG_PREBUILT_REFERENCE'] = str(self.reference)
        os.environ['MNG_OFFLINE_STORE'] = str(self.tmp / 'store')
        self.addCleanup(sys.modules.pop, 'build_app', None)
        self.addCleanup(lambda: sys.path.remove(str(self.source)) if str(self.source) in sys.path else None)
        sys.modules.pop('build_app', None)

    def build(self):
        with mock.patch.object(self.prebuilt, 'SPIRV_CROSS_CODE', self.code):
            return self.prebuilt.build(self.source)

    def test_every_program_is_for_macos_26_on_this_mac(self):
        out = self.build()
        self.assertEqual(sorted(p.name for p in out.iterdir() if p.name != 'manifest.json'), sorted(self.prebuilt.NAMES))
        self.assertEqual({m for _, m in macho_minos.scan(out)}, {MINIMUM})
        self.assertEqual((self.log / 'codec.txt').read_text(), MINIMUM)   # the libraries are asked for 26.0 too
        # the libraries come from the store, through an empty deps cache, never this Mac's (1.1.5: its own)
        cache = (self.log / 'deps-cache.txt').read_text()
        self.assertTrue(cache and 'mng-prebuilt-deps-' in cache, cache)

    def test_spirv_cross_is_the_recorded_program(self):
        out = self.build()
        self.assertEqual((out / 'spirv-cross').read_bytes(), (self.reference / 'spirv-cross').read_bytes())

    def test_without_the_recorded_spirv_cross_nothing_is_made(self):
        os.environ.pop('MNG_PREBUILT_REFERENCE')
        with self.assertRaisesRegex(SystemExit, 'MNG_PREBUILT_REFERENCE'):
            self.build()
        self.assertFalse((self.source / '.prebuilt').exists())

    def test_another_spirv_cross_is_refused(self):
        compile_c(self.reference / 'spirv-cross', MINIMUM, source='int main(void){return 3;}\n')
        with self.assertRaisesRegex(SystemExit, 'not the recorded spirv-cross'):
            self.build()

    def test_a_program_for_a_newer_macos_stops_the_build(self):
        """The check build() ends with: had any compile missed the minimum, the release stops."""
        folder = self.tmp / 'folder'
        compile_c(folder / 'totk-native', NEWER)
        with self.assertRaisesRegex(SystemExit, 'need a macOS newer than 26.0'):
            self.prebuilt.check_macos_min(folder)

    def test_the_recorded_spirv_cross_is_launcher_1_1_4s(self):
        self.assertEqual(self.prebuilt.SPIRV_CROSS_CODE,
                         '589d573ea94ac76e0721af38f33c9a6ead43b83c82dd35f8f84d2f1f05d7133c')
        self.assertEqual(self.prebuilt.MACOS_MIN, MINIMUM)


# ---- tools/deps ----
class DepsLibraries(unittest.TestCase):
    def setUp(self):
        self.deps = importlib.import_module('tools.deps')

    def test_libraries_are_made_for_macos_26_and_programs_keep_their_flags(self):
        seen = {}

        def recipe(name):
            return lambda stage: seen.__setitem__(name, os.environ.get('MACOSX_DEPLOYMENT_TARGET'))
        with mock.patch.dict(self.deps.RECIPES, {n: recipe(n) for n in self.deps.RECIPES}), \
                mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('MACOSX_DEPLOYMENT_TARGET', None)
            for name in ('opus', 'libvpx', 'dav1d', 'ogg', 'vorbis', 'zstd', 'spirv-cross'):
                self.deps._make(name, Path('/nonexistent'))
            self.assertNotIn('MACOSX_DEPLOYMENT_TARGET', os.environ)
        self.assertEqual(seen, {'opus': MINIMUM, 'libvpx': MINIMUM, 'dav1d': MINIMUM, 'ogg': MINIMUM,
                                'vorbis': MINIMUM, 'zstd': None, 'spirv-cross': None})

    def test_libvpx_names_its_arm64_target(self):
        """libvpx 1.16 knows darwin25 at most: on macOS 27 its guess was generic-gnu, plain C (1.1.5)."""
        calls = []
        with tempfile.TemporaryDirectory() as t, \
                mock.patch.object(self.deps, '_checkout', return_value=Path(t) / 'src'), \
                mock.patch.object(self.deps, 'cache', return_value=Path(t)), \
                mock.patch.object(self.deps, 'run', side_effect=lambda args, *a, **k: calls.append([str(x) for x in args])):
            self.deps._libvpx(Path(t) / 'stage')
        configure = next(c for c in calls if c[0].endswith('configure'))
        self.assertIn('--target=arm64-darwin25-gcc', configure)

    def test_a_cache_copy_unlike_the_launchers_is_replaced(self):
        """A Mac that took Launcher 1.1.5's copies (minos 27.2, libvpx in plain C) gets the Launcher in use's."""
        offline = importlib.import_module('tools.deps.offline')
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            name = self.deps._folder('opus')
            carried = t / 'store/built' / name
            (carried / 'lib').mkdir(parents=True)
            (carried / 'lib/libopus.a').write_bytes(b'!<arch>\nthe launcher\'s')
            (t / 'store/manifest.json').write_text(json.dumps({'schema': offline.SCHEMA, 'built': {
                name: offline.tree_files(carried)}}))
            stale = t / 'cache' / name / 'lib'
            stale.mkdir(parents=True)
            (stale / 'libopus.a').write_bytes(b'!<arch>\nan older launcher\'s')
            with mock.patch.dict(os.environ, {'MNG_OFFLINE_STORE': str(t / 'store'), 'MNG_DEPS_CACHE': str(t / 'cache')}):
                folder = self.deps.prefix('opus')
                self.assertEqual((folder / 'lib/libopus.a').read_bytes(), b'!<arch>\nthe launcher\'s')
                again = self.deps.prefix('opus')   # the same copy is kept
            self.assertEqual(offline.tree_files(again), offline.tree_files(carried))


# ---- the offline store ----
class OfflineStoreBuilt(unittest.TestCase):
    def setUp(self):
        self.pack = importlib.import_module('tools.deps.offline_pack')

    def test_carried_built_names_every_built_item(self):
        from tools.deps import _folder
        self.assertEqual(set(self.pack.carried_built()), {_folder(n) for n in self.pack.built_names()})

    def test_the_record_is_launcher_1_1_4s(self):
        record = json.loads((ROOT / 'tools/deps/carried_built.json').read_text())
        self.assertEqual(record['history'][0]['from'], 'launcher-v1.1.4 (Resources/offline.tar, built/)')
        vpx = record['built']['libvpx-1.16.0-1024874c5919']['lib/libvpx.a']
        # 1.1.4's libvpx (261 objects with NEON, minos 26.0), not 1.1.5's (183364d4…: plain C, minos 27.2)
        self.assertEqual(vpx, '48f2ba12dd9e44246bc144ce8c2469d65ffd434ade8da9c7bcad42f82ce30121')
        for entry in record['history']:
            for key in ('date', 'from', 'why', 'necessary', 'evidence'):
                self.assertTrue(entry.get(key), f'history entry without {key}')

    def test_other_files_are_refused(self):
        with tempfile.TemporaryDirectory() as t:
            folder = Path(t) / 'built'
            for f in self.pack.carried_built():
                (folder / f).mkdir(parents=True)
            vpx = next(folder.glob('libvpx-*')) / 'lib'
            vpx.mkdir()
            if HAS_CLANG:
                obj = compile_c(Path(t) / 'vp8_dx.o', NEWER, source='int vp8(void){return 1;}\n', object_only=True)
                subprocess.run(['xcrun', 'ar', 'rcs', str(vpx / 'libvpx.a'), str(obj)], check=True)
            problems = '\n'.join(self.pack.built_problems(folder))
            for f in self.pack.carried_built():
                self.assertIn(f'built/{f}: not the files', problems)
            if HAS_CLANG:
                self.assertIn('no NEON code', problems)
                self.assertIn(f'{NEWER}  ', problems)

    def test_build_stops_on_other_files(self):
        with tempfile.TemporaryDirectory() as t:
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop(self.pack.REMAKE_ENV, None)
                with self.assertRaisesRegex(RuntimeError, 'not what every release carries'):
                    self.pack.build_built(Path(t), {})

    def test_the_kept_store_on_this_mac_is_launcher_1_1_4s(self):
        """The store the private release tool passes (keep/launcher-release): exactly the record, minos 26.0, NEON."""
        store = Path('/Volumes/开发/MacNativeGames-artifacts/keep/launcher-release/offline-store')
        if not store.is_dir():
            self.skipTest('not the release Mac')
        self.assertEqual(self.pack.built_problems(store / 'built'), [])


if __name__ == '__main__':
    unittest.main()

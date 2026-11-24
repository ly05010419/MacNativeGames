"""tools/prebuilt.py: a build from a release's source takes the programs the release compiled from the same commit, so a
player's Mac needs no Xcode command line tools (user 2026-11-17: "做 1"). A program is taken only when the folder is of
this source's commit and the file is the manifest's; otherwise the build compiles, as before."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402
from tools import prebuilt  # noqa: E402
# a release's source.tar.gz carrying .prebuilt (make_app.source_archive) is checked in MNG-Launcher, the Launcher's own
# repository since 2026-11-20 (tests/test_porting_contract.py SourceArchive)


def source_with(root, programs, commit='c0ffee', manifest_commit=None):
    """A release source folder: .mng-source-commit and .prebuilt with these {name: bytes}."""
    (root / '.mng-source-commit').write_text(commit + '\n')
    folder = root / prebuilt.FOLDER
    folder.mkdir(parents=True)
    files = {}
    for name, data in programs.items():
        (folder / name).write_bytes(data)
        files[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
    (folder / 'manifest.json').write_text(json.dumps({'schema': prebuilt.SCHEMA, 'commit': manifest_commit or commit,
                                                      'files': files}))
    return root


class Program(unittest.TestCase):
    def test_taken_only_for_this_source_and_its_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(Path(tmp), {'hactool': b'tool'})
            self.assertEqual(prebuilt.program(src, 'hactool'), src / '.prebuilt/hactool')
            self.assertIsNone(prebuilt.program(src, 'spirv-cross'))
            (src / '.prebuilt/hactool').write_bytes(b'changed')
            self.assertIsNone(prebuilt.program(src, 'hactool'))
        with tempfile.TemporaryDirectory() as tmp:   # the folder of another commit
            src = source_with(Path(tmp), {'hactool': b'tool'}, manifest_commit='other')
            self.assertIsNone(prebuilt.program(src, 'hactool'))
        with tempfile.TemporaryDirectory() as tmp:   # a checkout: no .mng-source-commit
            src = source_with(Path(tmp), {'hactool': b'tool'})
            (src / '.mng-source-commit').unlink()
            self.assertIsNone(prebuilt.program(src, 'hactool'))

    def test_complete_needs_every_program(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(Path(tmp), {n: n.encode() for n in prebuilt.NAMES})
            self.assertTrue(prebuilt.complete(src))
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(Path(tmp), {n: n.encode() for n in prebuilt.NAMES[1:]})
            self.assertFalse(prebuilt.complete(src))

    def test_place_copies_executable_and_only_mode_refuses_a_compile(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 's').mkdir()
            src = source_with(Path(tmp) / 's', {'hactool': b'tool'})
            dest = Path(tmp) / 'out/hactool'
            self.assertTrue(prebuilt.place(src, 'hactool', dest))
            self.assertEqual(dest.read_bytes(), b'tool')
            self.assertTrue(os.access(dest, os.X_OK))
            self.assertFalse(prebuilt.place(src, 'spirv-cross', Path(tmp) / 'x'))
            with mock.patch.dict(os.environ, {'MNG_PREBUILT_ONLY': '1'}):
                with self.assertRaises(prebuilt.Missing):
                    prebuilt.place(src, 'spirv-cross', Path(tmp) / 'x')
                with self.assertRaises(prebuilt.Missing):
                    prebuilt.compiler_needed('anything')


class BuildUsesThem(unittest.TestCase):
    def tree(self, root):
        tree = root / 'work/totk'
        (tree / 'build').mkdir(parents=True)
        return tree

    def test_runtime_from_the_release_runs_no_make(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / 'src'; src.mkdir()
            source_with(src, {prebuilt.runtime_name('pkp'): b'runtime'})
            tree = self.tree(root)
            with mock.patch.object(build_app, 'REPO', src), mock.patch.object(build_app, 'run') as run:
                build_app.make_runtime(tree, 'pkp', root / 'logs', {})
            self.assertEqual((tree / 'build/totk-native-pkp').read_bytes(), b'runtime')
            cmds = [[str(c) for c in call.args[0]] for call in run.call_args_list]
            self.assertFalse(any(c[0] == 'make' for c in cmds), cmds)
            self.assertIn(['export', '../pokopia/assets/shader_list.tsv'], [c[2:4] for c in cmds])   # Makefile SHADER_LIST ?=
            self.assertEqual([c[1].rsplit('/', 1)[-1] for c in cmds[1:]],
                             ['prepare_runtime.py', 'prepare_romfs_backend.py', 'prepare_shared_fonts.py'])

    def test_without_one_the_build_makes_as_before(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = self.tree(root)
            with mock.patch.object(build_app, 'REPO', root), mock.patch.object(build_app, 'run') as run:
                build_app.make_runtime(tree, 'v143', root / 'logs', {})
                build_app.make_runtime(tree, 'v0', root / 'logs', {}, data=False)
            self.assertEqual([call.args[0][-1] for call in run.call_args_list], ['runtime-v143', 'binary'])

    @staticmethod
    def makefile_game_data():
        """games/totk/Makefile, per version: the files game-data-<version> makes and the Python steps that make them."""
        text = (ROOT / 'games/totk/Makefile').read_text()
        define = text.split('define GAME_DATA\n', 1)[1].split('\nendef', 1)[0]
        recipe = r'^({target}): .*\n\t.*?"\$+\(PY\)" scripts/(\S+\.py) --version {version}(.*)$'
        found = {}
        for v in re.findall(r'^\$\(eval \$\(call GAME_DATA,(\w+),', text, re.M):
            files, steps = set(), set()
            for m in re.finditer(recipe.format(target=r'build/game-data/\S+', version=r'\S+'), define.replace('$(1)', v), re.M):
                files.add(m[1].rsplit('/', 1)[-1]); steps.add((m[2], *m[3].split()))
            for line in re.findall(rf'^game-data-{v}: (.*)$', text, re.M):   # a version's own rules (BotW's aoc-config.txt)
                for target in line.split():
                    m = re.search(recipe.format(target=re.escape(target), version=v), text, re.M)
                    files.add(target.rsplit('/', 1)[-1]); steps.add((m[2], *m[3].split()))
            found[v] = files, steps
        return found

    def test_game_data_steps_are_the_makefiles(self):
        """#2282, #2318: with the release's prebuilt runtime the Python steps stood in for make game-data-botw190 without
        its aoc-config.txt rule, and no Breath of the Wild build got past placing the game data."""
        found = self.makefile_game_data()
        self.assertIn(('prepare_romfs_backend.py', '--aoc'), found['botw190'][1])
        self.assertEqual(set(found), {'v0', 'v121', 'v143', 'botw160', 'botw190', 'dkb', 'pkp'})
        for version, (files, steps) in found.items():
            with self.subTest(version), mock.patch.object(build_app, 'run') as run:
                build_app.make_game_data(Path('/w'), version, Path('/logs'), {})
                ran = {(c.args[0][1].rsplit('/', 1)[-1], *c.args[0][4:]) for c in run.call_args_list}
                self.assertEqual(ran, steps)
                self.assertLessEqual(set(build_app.game_data_files(version)), files)   # what place_game_data wants

    def test_warm_tool_and_hactool_from_the_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / 'src'; src.mkdir()
            source_with(src, {'warm-metal-cache': b'warm', 'hactool': b'hac', 'spirv-cross': b'sc'})
            tree = self.tree(root)
            with mock.patch.object(build_app, 'REPO', src), mock.patch.object(build_app, 'run') as run, \
                    mock.patch.object(build_app, 'git_fetch') as fetch:
                build_app.build_warm_tool(tree, root / 'logs', {})
                hactool = build_app.build_hactool(tree, root / 'logs')
                build_app.build_spirv_cross(tree, root / 'logs')
            run.assert_not_called(); fetch.assert_not_called()
            self.assertEqual(hactool.read_bytes(), b'hac')
            self.assertEqual((tree / 'build/spirv-cross/spirv-cross').read_bytes(), b'sc')

    def test_a_complete_source_checks_no_developer_tools(self):
        """On a Mac without the command line tools xcrun, git and make each ask to install them: not one may run."""
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(Path(tmp), {n: n.encode() for n in prebuilt.NAMES})
            args = mock.Mock(platform='macos')
            with mock.patch.object(build_app, 'REPO', src), \
                    mock.patch.object(build_app.subprocess, 'run', side_effect=AssertionError('ran a developer tool')), \
                    mock.patch.object(build_app, 'codec_libraries', side_effect=AssertionError('made codec libraries')), \
                    mock.patch.object(build_app.shutil, 'which', return_value='/usr/local/bin/python3.14'):
                self.assertEqual(build_app.prerequisites(args), Path('/usr/local/bin/python3.14'))
            args = mock.Mock(platform='ios')   # an iPhone / iPad build compiles: it checks as before
            with mock.patch.object(build_app, 'REPO', src), mock.patch.object(build_app, 'developer_tools') as tools, \
                    mock.patch.object(build_app.shutil, 'which', return_value='/usr/local/bin/python3.14'):
                build_app.prerequisites(args)
            tools.assert_called_once()

    def test_no_make_left_outside_make_runtime(self):
        for name in ('update_runtime.py',):
            self.assertNotIn("run(['make'", (ROOT / name).read_text(), name)
        text = (ROOT / 'build_app.py').read_text()
        self.assertEqual(text.count("run(['make'"), 2)   # make_runtime's fallback and hactool's (after its prebuilt check)


class LauncherChecks(unittest.TestCase):
    """The Build page's first page (tools/builder_jobs/checks.py): with the release's programs it runs no xcrun or git,
    which open Apple's install dialog on a Mac without the command line tools, and says none are needed."""

    def test_a_release_source_runs_no_developer_tool(self):
        from tools.builder_jobs import checks
        with mock.patch.object(checks, 'compile_free', return_value=True), \
                mock.patch.object(checks, '_ok', side_effect=AssertionError('ran xcrun')), \
                mock.patch.object(checks, '_out', side_effect=AssertionError('ran xcrun')), \
                mock.patch.object(checks.shutil, 'which', side_effect=AssertionError('looked for git')):
            out = checks.checks('/tmp')
        ids = {c['id']: c['ok'] for c in out}
        self.assertTrue(ids['tools'])
        self.assertFalse({'clt', 'sdk', 'cxx', 'git'} & set(ids))

    def test_a_checkout_checks_them_as_before(self):
        from tools.builder_jobs import checks
        with mock.patch.object(checks, 'compile_free', return_value=False), \
                mock.patch.object(checks, '_ok', return_value=False), mock.patch.object(checks, '_out', return_value=''):
            ids = [c['id'] for c in checks.checks('/tmp')]
        self.assertEqual(ids[:5], ['mac', 'clt', 'sdk', 'cxx', 'git'])

    def test_compile_free_reads_the_launcher_source(self):
        from tools.builder_jobs import checks
        with mock.patch.object(prebuilt, 'complete', return_value=True) as complete:
            self.assertTrue(checks.compile_free())
        self.assertEqual(Path(complete.call_args.args[0]), ROOT)


class GamesTheProgramsDoNotCover(unittest.TestCase):
    """Page 1 says the developer tools are not needed; the games with their own games/<game>/build.py (ss, dread,
    worldend, tmnt, acnh) still compile on this Mac, so their build is held at page 3 with the installer, not left to fail
    halfway (2026-11-17, found when origin's Animal Crossing commit met this change)."""

    def plan(self, game):
        from tools.builder_jobs import classify
        return classify.plan([classify.Item('/tmp/none', 'ns2_dump' if game in ('dkb', 'pkp') else 'base', game)])

    def test_held_at_page_three_without_the_tools(self):
        from tools.builder_jobs import classify
        with mock.patch.object(classify, 'no_developer_tools', return_value=True), \
                mock.patch.object(classify, 'no_metal_compiler', return_value=False):
            for game in ('acnh', 'tmnt', 'dread'):
                with self.subTest(game=game):
                    p = self.plan(game)
                    self.assertTrue(any('command line tools' in b or '命令行工具' in b for b in p.blocks), p.blocks)
                    self.assertEqual(p.tool_needs, ['clt'])
                    self.assertEqual(len(p.tool_blocks), 1)
            for game in ('totk', 'botw', 'dkb', 'pkp'):
                with self.subTest(game=game):
                    self.assertEqual(self.plan(game).tool_needs, [])

    def test_with_the_tools_nothing_is_added(self):
        from tools.builder_jobs import classify
        with mock.patch.object(classify, 'no_developer_tools', return_value=False):
            self.assertEqual(self.plan('acnh').tool_needs, [])

    def test_the_check_reads_files_and_runs_no_xcrun(self):
        from tools.builder_jobs import classify
        with tempfile.TemporaryDirectory() as tmp:
            calls = []

            def fake(cmd, **kw):
                calls.append(cmd)
                return subprocess.CompletedProcess(cmd, 0, tmp + '\n', '')
            with mock.patch.object(classify.subprocess, 'run', side_effect=fake):
                self.assertTrue(classify.no_developer_tools())
                (Path(tmp) / 'usr/bin').mkdir(parents=True)
                (Path(tmp) / 'usr/bin/clang').write_text('')
                self.assertFalse(classify.no_developer_tools())
            self.assertEqual({c[0] for c in calls}, {'/usr/bin/xcode-select'})

    def test_a_full_xcode_counts(self):
        """xcode-select naming Xcode.app: clang is in its default toolchain, not in Developer/usr/bin (issue #2284)."""
        from tools.builder_jobs import classify
        with tempfile.TemporaryDirectory() as tmp:
            run = lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, tmp + '\n', '')
            with mock.patch.object(classify.subprocess, 'run', side_effect=run):
                (Path(tmp) / 'usr/bin').mkdir(parents=True)
                self.assertTrue(classify.no_developer_tools())
                clang = Path(tmp) / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
                clang.parent.mkdir(parents=True)
                clang.write_text('')
                self.assertFalse(classify.no_developer_tools())


class RuntimeSignature(unittest.TestCase):
    """A runtime maps guest code at start (mprotect to PROT_EXEC); signed with the hardened runtime without the
    entitlements that permit it, macOS kills it (SIGKILL, exit 137) and every update's runtime probe fails."""

    def _macho(self, root, *, runtime=False, entitlements=False):
        """A real Mach-O (the system's true) signed as asked; its bytes."""
        src = shutil.which('true')
        if not src or not shutil.which('codesign'):
            self.skipTest('codesign and a Mach-O are needed to test the signature')
        p = root / 'true'
        p.write_bytes(Path(src).read_bytes())
        args = ['codesign', '--force', '--sign', '-']
        if runtime:
            args += ['--options', 'runtime']
        if entitlements:
            ent = root / 'ent.plist'
            ent.write_text(prebuilt.RUNTIME_ENTITLEMENTS)
            args += ['--entitlements', str(ent)]
        args.append(str(p))
        subprocess.run(args, check=True, capture_output=True)
        return p

    def test_runtime_signature_ok_for_the_three_forms(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertTrue(prebuilt.runtime_signature_ok(self._macho(root)))
            self.assertFalse(prebuilt.runtime_signature_ok(self._macho(root, runtime=True)))
            self.assertTrue(prebuilt.runtime_signature_ok(self._macho(root, runtime=True, entitlements=True)))

    def test_program_takes_a_runtime_only_when_its_signature_permits_guest_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = self._macho(root, runtime=True)   # hardened runtime, no entitlements
            src = root / 'src'; src.mkdir()
            source_with(src, {'totk-native': bad.read_bytes()})
            self.assertIsNone(prebuilt.program(src, 'totk-native'))
            good = self._macho(root, runtime=True, entitlements=True)
            src2 = root / 'src2'; src2.mkdir()
            source_with(src2, {'totk-native': good.read_bytes()})
            self.assertEqual(prebuilt.program(src2, 'totk-native'), src2 / '.prebuilt/totk-native')

    def test_a_non_macho_stand_in_is_left_alone(self):
        """The tests' stand-in programs are a few bytes, not Mach-O: codesign cannot read them, so they are taken."""
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(Path(tmp), {'totk-native': b'runtime'})
            self.assertEqual(prebuilt.program(src, 'totk-native'), src / '.prebuilt/totk-native')


if __name__ == '__main__':
    unittest.main()

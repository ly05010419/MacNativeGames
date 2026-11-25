"""The Launcher's offline store (tools/deps/offline.py, offline_pack.py): a player packages any game with no network
(user 2026-11-20: "launcher 的目标 用户安装就能用 不需要 联网， 所有东西 我们打包的时候 都放进去"; 2026-11-19: "我的 用户 大多在
中国", where GitHub is blocked; 2026-11-21: "我一直要求 竟然今天才知道没有实现？？？").

Every download a build script can make must be in the store (offline_pack's lists, read from the code that downloads),
or be listed below with why a player's build never makes it. The proof that nothing is downloaded is offline_pack.py
check, which denies the network itself (tests/test_offline_check_network.py; the release runs it; until 2026-11-25 it ran
under the retired storage guard's --offline); these tests keep the lists honest without a network."""
import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.deps import core, offline, offline_pack  # noqa: E402

# The games the Launcher builds (tools/builder_jobs/repo.py GAMES) and the folder holding each one's build code
GAME_FOLDERS = {'totk': 'totk', 'botw': 'totk', 'dkb': 'dkb', 'pkp': 'pokopia', 'oot': 'oot', 'dread': 'dread',
                'tmnt': 'tmnt', 'acnh': 'acnh', 'eow': 'eow', 'odyssey': 'odyssey'}
HOSTS = re.compile(r'https?://(github\.com|raw\.githubusercontent\.com|[a-z0-9-]+\.googlesource\.com|pypi\.org|'
                   r'files\.pythonhosted\.org|api\.nuget\.org|[a-z0-9.-]*nintendo\.(net|com)|downloads\.[a-z.]+|builds\.dotnet)')
# Download addresses in build code that a player's build never fetches, and why
NOT_FETCHED = {
    'shader_update.py': 'the newest shared list: a released Launcher uses the list it carries (shader_update.newest)',
    'tools/mng.py': 'a message: where to download the Launcher',
    'tools/builder_jobs/migrate.py': 'a message: where to download the Launcher',
    'tools/builder/icons_env.py': 'PyPI only without a store; with one, pip_argv uses the store (tested below)',
    'games/oot/tools/shader_pins.py': 'build_shader_service.py, a development tool (a build uses the prebuilt translator)',
    'games/oot/tools/build_proof_tools.py': 'build_shader_service.py, a development tool',
}
NOT_FETCHED_PINS = {
    'DEPTH_MODEL_LICENSE': 'the Launcher itself carries the depth model (MNG-Launcher make_app.py)',
    'PYTHON_ENVIRONMENTS': 'tools/ui_pack, a development tool, not a game build',
}
# a command that runs the .NET SDK (['dotnet', 'build', …], [dotnet, 'restore', …], "$DOTNET" publish) or asks tools/deps
# for it (program('dotnet'), env('dotnet'), …): the store carries no SDK (offline.NOT_CARRIED)
SDK_COMMAND = re.compile(r"""['"]dotnet['"]\s*,\s*['"](?:build|restore|publish)['"]|\[\s*[\w.]*dotnet\s*,\s*['"](?:build|restore|publish)['"]"""
                         r"""|"\$DOTNET"\s+publish|(?:program|prefix|env|path)\([^)]*['"]dotnet['"]""")
# Code of the Launcher's games that runs the SDK but is never part of a player's build, and why
SDK_NOT_IN_A_BUILD = {
    'games/tmnt/tools/totk_xlate/publish_native.sh': 'rebuilds the prebuilt translator the repository holds (a development tool)',
    'games/tmnt/tools/totk_xlate/publish_ios_lib.sh': 'an iOS library, run by no build script (a development tool)',
    'games/totk/tools/totk_xlate/publish_native.sh': 'rebuilds the prebuilt translator the repository holds (a development tool)',
    'games/totk/tools/totk_xlate/publish_ios_lib.sh': 'rebuilds the prebuilt iOS translator library (a development tool)',
    'games/oot/tools/totk_xlate/publish_native.sh': 'rebuilds the prebuilt translator the repository holds (a development tool)',
    'games/oot/tools/totk_xlate/publish_ios_lib.sh': 'rebuilds the prebuilt iOS translator library (a development tool)',
    'games/oot/tools/build_shader_service.py': 'rebuilds the prebuilt translator (a development tool, NOT_FETCHED)',
}


def build_code():
    """The files a player's build runs: the shared entry points, tools/ and each Launcher game's own build code."""
    files = [REPO / 'build_app.py', REPO / 'update_runtime.py', REPO / 'shader_update.py', *REPO.glob('tools/**/*.py')]
    for folder in sorted(set(GAME_FOLDERS.values())):
        for p in (REPO / 'games' / folder).rglob('*'):
            parts = set(p.relative_to(REPO / 'games' / folder).parts)
            if p.is_file() and (p.suffix in ('.py', '.sh') or p.name == 'Makefile') and not (
                    {'ios', 'tests', 'third_party', 'runtime', 'analysis', '__pycache__'} & parts):
                files.append(p)
    return files


def addresses(path):
    """(line, address) of every download address in a file's code (string literals; a Makefile's uncommented lines)."""
    if path.suffix == '.py':
        for node in ast.walk(ast.parse(path.read_text())):
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and '\n' not in node.value:
                m = HOSTS.search(node.value)
                if m:
                    yield node.lineno, node.value[m.start():].split()[0]
    else:
        for i, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            m = HOSTS.search(line)
            if m and not line.lstrip().startswith('#'):
                yield i, line[m.start():].split()[0].strip('"\'')


class EveryDownloadIsCarried(unittest.TestCase):
    def test_every_launcher_game_is_known_here(self):
        from tools.builder_jobs import repo
        self.assertEqual(set(repo.GAMES), set(GAME_FOLDERS), 'a new game in the Launcher: add its build folder here')

    def test_every_requirements_file_goes_in(self):
        wanted = {f'games/{f}' for f in GAME_FOLDERS.values() if (REPO / 'games' / f / 'requirements.txt').is_file()}
        self.assertEqual(wanted, set(offline_pack.REQUIREMENTS))

    def test_every_download_address_in_build_code_is_in_the_store(self):
        git = {url for url, _ in offline_pack.git_pins()}
        files = {url for url, _ in offline_pack.files_wanted().values()}
        from tools.deps import pins
        skipped = {v[1] if isinstance(v, tuple) and len(v) == 2 else None for v in ()}
        skipped |= {pins.DEPTH_MODEL_LICENSE[0]}
        skipped |= {url for env in pins.PYTHON_ENVIRONMENTS.values() for _, url, _ in env}
        skipped |= {pins.SOURCES[n][1] for n in offline.NOT_CARRIED if n in pins.SOURCES}
        skipped |= {pins.SOURCES[n][1] for n in offline_pack.built_names()}   # carried made (built/), not as sources
        skipped |= {pins.PACKAGES[n][1] for n in offline.NOT_CARRIED if n in pins.PACKAGES}
        unexplained = []
        for path in build_code():
            rel = str(path.relative_to(REPO))
            if rel in NOT_FETCHED:
                continue
            for line, url in addresses(path):
                if url in git or url in files or url in skipped:
                    continue
                if url.startswith('https://raw.githubusercontent.com/google/fonts/'):   # open_fonts._url: every font is a file
                    continue
                unexplained.append(f'{rel}:{line} {url}')
        self.assertEqual(unexplained, [], 'a build downloads this without the offline store: add it to '
                                          'tools/deps/offline_pack.py (or say in NOT_FETCHED why a build never does)')

    def test_no_launcher_game_build_runs_the_dotnet_sdk(self):
        """The store carries no .NET SDK (user 2026-11-21: "去掉 .net sdk"): no build of a game the Launcher makes may
        run it. A .NET program a game needs goes in the repository prebuilt (TMNT's translator, 2026-11-22)."""
        found = {str(p.relative_to(REPO)) for p in build_code() if SDK_COMMAND.search(p.read_text(errors='replace'))}
        for rel in SDK_NOT_IN_A_BUILD:
            self.assertTrue((REPO / rel).is_file(), rel)
        self.assertEqual(found - set(SDK_NOT_IN_A_BUILD), set(), 'a Launcher game\'s build runs the .NET SDK, which '
                                                                 'the Launcher does not carry: use a prebuilt program')

    def test_what_is_not_carried_is_left_out(self):
        from tools.deps import pins
        files = offline_pack.files_wanted()
        self.assertEqual(offline.NOT_CARRIED, ('dotnet', 'libhac', 'cmake', 'glslang', 'xcodegen'))
        for name in ('dotnet', 'xcodegen'):
            self.assertNotIn(pins.SOURCES[name][2], files)
        self.assertNotIn(pins.PACKAGES['libhac'][2], files)
        self.assertNotIn(pins.SOURCES['glslang'][1], {url for url, _ in offline_pack.git_pins()})
        self.assertFalse(any(l.startswith('cmake==') for _, lines in offline_pack.requirement_sets() for l in lines))

    def test_no_launcher_game_mac_build_uses_cmake_glslang_or_xcodegen(self):
        """CMake (54 MB), glslang and XcodeGen are not carried (user 2026-11-22: "好"): a Mac build of a Launcher game
        uses none of them; spirv-cross is compiled with clang alone."""
        uses = re.compile(r"""(?:program|prefix|path|build_tools)\([^)]*['"](?:cmake|glslang|glslangValidator|xcodegen)['"]"""
                          r"""|CMAKE_SET|tools/deps program (?:glslangValidator|xcodegen)|_cmake_install\(""")
        allowed = {'tools/deps/__init__.py': 'defines them, for iPhone / iPad builds and glslang',
                   'build_app.py': 'cmake only for an iPhone / iPad build (codec_libraries target != macos)',
                   'games/dread/build.py': 'the iPhone / iPad build (ios_prerequisites)'}
        found = {str(p.relative_to(REPO)) for p in build_code() if uses.search(p.read_text(errors='replace'))}
        self.assertEqual(found - set(allowed), set())
        import inspect
        import tools.deps as deps
        self.assertNotIn('cmake', inspect.getsource(deps._spirv_cross))

    def test_spirv_cross_revision_is_cmakes(self):
        """--revision is the line SPIRV-Cross's CMake writes: nxnative keys its shader cache on it."""
        import tools.deps as deps
        with tempfile.TemporaryDirectory() as t:
            env = dict(os.environ, GIT_AUTHOR_DATE='2026-09-16T13:27:19', GIT_COMMITTER_DATE='2026-09-16T13:27:19',
                       GIT_AUTHOR_NAME='a', GIT_AUTHOR_EMAIL='a@b', GIT_COMMITTER_NAME='a', GIT_COMMITTER_EMAIL='a@b')
            subprocess.run(['git', 'init', '-q', t], check=True, env=env)
            (Path(t) / 'f').write_text('x')
            subprocess.run(['git', '-C', t, 'add', 'f'], check=True, env=env)
            subprocess.run(['git', '-C', t, 'commit', '-q', '-m', 'x'], check=True, env=env)
            short = subprocess.check_output(['git', '-C', t, 'rev-parse', '--short', 'HEAD'], text=True).strip()
            self.assertEqual(deps.spirv_cross_revision(t), f'Git commit: {short} Timestamp: 2026-09-16T13:27:19')

    def test_skyward_sword_and_worldend_are_not_launcher_games(self):
        """User 2026-11-22: "去掉 WORLDEND SYNDROME" (from the Launcher only), "去掉 塞尔达传说 御天之剑 HD"."""
        from tools.builder_jobs import repo
        self.assertNotIn('ss', repo.GAMES)
        self.assertNotIn('worldend', repo.GAMES)
        self.assertNotIn('games/ss', offline_pack.REQUIREMENTS)
        self.assertNotIn('games/worldend', offline_pack.REQUIREMENTS)
        from tools.builder_jobs import classify
        for title, folder in (('01002DA013484000', 'games/ss'), ('010048900CF64000', 'games/worldend')):
            self.assertIsNone(repo.game_of_title(title))   # never taken for a game the Launcher builds
            self.assertIn(f'{folder}/build.py', classify.not_in_launcher(title))

    def test_explained_exceptions_still_exist(self):
        for rel in NOT_FETCHED:
            self.assertTrue((REPO / rel).is_file(), rel)
        from tools.deps import pins
        for name in NOT_FETCHED_PINS:
            self.assertTrue(hasattr(pins, name), name)

    def test_every_git_clone_in_build_code_uses_a_pinned_address(self):
        """A git clone or fetch reads its address from a pin offline_pack knows, never one written only at the call."""
        git = {url for url, _ in offline_pack.git_pins()}
        for path in build_code():
            if path.name == 'Makefile':
                for line in path.read_text().splitlines():
                    if re.search(r'\bgit (clone|fetch)\b', line) and not line.lstrip().startswith('#'):
                        self.assertTrue(any(u in line for u in git), f'{path}: {line.strip()}')


class Environment(unittest.TestCase):
    def setUp(self):
        self.store = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.store)
        (self.store / 'manifest.json').write_text(json.dumps({'git': {'a-b': ['https://github.com/a/b.git']}}))

    def test_pip_and_git_point_at_the_store(self):
        env = offline.environment(self.store, {'PATH': '/usr/bin'})
        self.assertEqual(env['PIP_NO_INDEX'], '1')
        self.assertEqual(env['PIP_FIND_LINKS'], (self.store / 'wheels').resolve().as_uri())
        self.assertEqual(env['GIT_CONFIG_COUNT'], '1')
        self.assertEqual(env['GIT_CONFIG_KEY_0'], f'url.{self.store / "git/a-b.git"}.insteadOf')
        self.assertEqual(env['GIT_CONFIG_VALUE_0'], 'https://github.com/a/b.git')
        self.assertNotIn('RestoreSources', env)   # nothing is restored: no SDK, the programs come built
        self.assertEqual(env['MNG_OFFLINE_STORE'], str(self.store))

    def test_a_store_path_with_a_space_is_one_pip_location(self):
        # pip splits PIP_FIND_LINKS at spaces: "…/Application Support/…/wheels" as a path was two locations, both ignored
        store = self.store / 'Application Support' / 'offline'
        store.mkdir(parents=True)
        shutil.copy(self.store / 'manifest.json', store)
        links = offline.environment(store, {})['PIP_FIND_LINKS']
        self.assertEqual(links.split(), [links])
        self.assertTrue(links.startswith('file:///') and links.endswith('Application%20Support/offline/wheels'), links)

    def test_no_store_named_changes_nothing(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('MNG_OFFLINE_STORE', None)
            self.assertIsNone(offline.store())
            self.assertIsNone(offline.activate())

    def test_a_folder_without_a_manifest_is_no_store(self):
        empty = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, empty)
        with mock.patch.dict(os.environ, {'MNG_OFFLINE_STORE': str(empty)}):
            self.assertIsNone(offline.store())


class Fetch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.store = self.tmp / 'store'
        (self.store / 'files').mkdir(parents=True)
        (self.store / 'manifest.json').write_text('{}')
        self.data = b'pinned bytes'
        import hashlib
        self.want = hashlib.sha256(self.data).hexdigest()
        env = {'MNG_OFFLINE_STORE': str(self.store), 'MNG_DEPS_CACHE': str(self.tmp / 'cache')}
        patcher = mock.patch.dict(os.environ, env)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_carried_file_is_taken_without_a_download(self):
        (self.store / 'files' / self.want).write_bytes(self.data)
        with mock.patch.object(core, 'run', side_effect=AssertionError('downloaded')):
            got = core.fetch('https://github.com/x/y.tar.gz', self.want, 'y.tar.gz')
        self.assertEqual(got.read_bytes(), self.data)
        self.assertEqual(got, self.tmp / 'cache/src/y.tar.gz')

    def test_a_missing_file_fails_without_a_download(self):
        with mock.patch.object(core, 'run', side_effect=AssertionError('downloaded')):
            with self.assertRaisesRegex(core.DepsError, 'not in the Launcher\'s offline store'):
                core.fetch('https://github.com/x/y.tar.gz', self.want, 'y.tar.gz')

    def test_a_damaged_copy_is_refused(self):
        (self.store / 'files' / self.want).write_bytes(b'other bytes')
        with self.assertRaisesRegex(core.DepsError, 'does not match'):
            core.fetch('https://github.com/x/y.tar.gz', self.want, 'y.tar.gz')


class GitFromTheStore(unittest.TestCase):
    """A pinned commit from the store's bare repository, through git's own url.<…>.insteadOf: the clone-then-checkout
    and the fetch-one-commit styles the build scripts use, with the original repository gone."""

    def test_both_styles_get_the_commit_with_the_source_gone(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        git = ['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'init.defaultBranch=main']
        source = tmp / 'origin'
        subprocess.run([*git, 'init', '-q', source], check=True)
        (source / 'a.txt').write_text('a\n')
        subprocess.run([*git, '-C', source, 'add', '.'], check=True)
        subprocess.run([*git, '-C', source, 'commit', '-q', '-m', 'a'], check=True)
        commit = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
        url = source.as_uri()
        store = tmp / 'store'
        with mock.patch.object(offline_pack, 'git_pins', return_value=[(url, commit)]):
            repos = offline_pack.build_git(store)
        (store / 'manifest.json').write_text(json.dumps({'git': repos}))
        shutil.rmtree(source)
        env = offline.environment(store, {'PATH': os.environ['PATH'], 'HOME': str(tmp)})
        a, b = tmp / 'a', tmp / 'b'
        subprocess.run(['git', 'init', '-q', a], check=True, env=env)
        subprocess.run(['git', '-C', a, 'fetch', '-q', '--depth', '1', url, commit], check=True, env=env)
        subprocess.run(['git', '-C', a, 'checkout', '-q', 'FETCH_HEAD'], check=True, env=env)
        subprocess.run(['git', 'clone', '-q', url, b], check=True, env=env, capture_output=True)
        subprocess.run(['git', '-C', b, 'checkout', '-q', '--detach', commit], check=True, env=env)
        for folder in (a, b):
            self.assertEqual(subprocess.check_output(['git', '-C', folder, 'rev-parse', 'HEAD'], text=True).strip(), commit)
            self.assertEqual((folder / 'a.txt').read_text(), 'a\n')


class LaunchersOwnDownloads(unittest.TestCase):
    def setUp(self):
        self.store = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.store)
        (self.store / 'manifest.json').write_text('{}')

    def test_the_icons_page_installs_from_the_store(self):
        from tools.builder import icons_env
        with mock.patch.dict(os.environ, {'MNG_OFFLINE_STORE': str(self.store)}):
            argv = [str(a) for a in icons_env.pip_argv(Path('/v'), 'req.txt')]
        self.assertIn('--no-index', argv)
        self.assertEqual(argv[argv.index('--find-links') + 1], str(self.store / 'wheels'))
        self.assertNotIn(icons_env.INDEX, argv)
        self.assertIn('--require-hashes', argv)

    def test_the_shader_list_is_the_carried_one_and_nothing_is_downloaded(self):
        import shader_update as su
        with mock.patch.dict(os.environ, {'MNG_OFFLINE_STORE': str(self.store)}), \
             mock.patch.object(su, 'download', side_effect=AssertionError('downloaded')):
            path, where = su.newest('totk', self.store)
        self.assertEqual(path, REPO / su.LISTS['totk'])
        self.assertIn('carries', where)

    def test_a_blocked_github_fails_fast(self):
        import shader_update as su
        source = Path(su.__file__).read_text()
        self.assertIn("'--connect-timeout', '15'", source)
        self.assertIn("'--speed-time', '30'", source)

    def test_the_app_icons_come_from_the_store(self):
        for script in ('games/totk/scripts/extract_app_icon.py', 'games/oot/scripts/extract_app_icon.py'):
            text = (REPO / script).read_text()
            self.assertIn("os.environ.get('MNG_OFFLINE_STORE')", text, script)
            self.assertIn("Path(store, 'files', source['sha256'])", text, script)


class BuiltDependencies(unittest.TestCase):
    """The tools/deps libraries and programs a released Launcher carries made at the release (user 2026-11-22: "做")."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.store = self.tmp / 'store'
        from tools.deps import _folder
        self.name = _folder('zstd')
        made = self.store / 'built' / self.name
        (made / 'bin').mkdir(parents=True)
        (made / 'bin/zstd').write_bytes(b'made at the release')
        (made / 'licences').mkdir()
        (made / 'licences/LICENSE').write_text('BSD')
        (self.store / 'manifest.json').write_text(json.dumps({'built': {self.name: offline.tree_files(made)}}))
        patcher = mock.patch.dict(os.environ, {'MNG_OFFLINE_STORE': str(self.store), 'MNG_DEPS_CACHE': str(self.tmp / 'cache')})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_the_carried_copy_is_installed_and_nothing_is_compiled(self):
        import tools.deps as deps
        with mock.patch.dict(deps.RECIPES, {'zstd': mock.Mock(side_effect=AssertionError('compiled'))}):
            folder = deps.prefix('zstd')
        self.assertEqual((folder / 'bin/zstd').read_bytes(), b'made at the release')
        self.assertEqual(folder, self.tmp / 'cache' / self.name)

    def test_a_damaged_copy_is_refused(self):
        import tools.deps as deps
        (self.store / 'built' / self.name / 'bin/zstd').write_bytes(b'changed')
        with self.assertRaisesRegex(deps.DepsError, 'damaged'):
            deps.prefix('zstd')

    def test_every_carried_source_is_carried_made_and_not_as_source(self):
        from tools.deps import pins
        names = offline_pack.built_names()
        self.assertEqual(set(names), set(pins.SOURCES) - set(offline.NOT_CARRIED))
        self.assertIn('spirv-cross', names)
        files, git = offline_pack.files_wanted(), {url for url, _ in offline_pack.git_pins()}
        for name in names:
            version, url, pin = pins.SOURCES[name]
            self.assertNotIn(pin, files, name)
            self.assertFalse(url in git and (url, pin) in offline_pack.git_pins(), name)


class PrebuiltTranslator(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def test_tmnt_uses_its_prebuilt_translator_and_refuses_another(self):
        """TMNT's translator is prebuilt in the repository (user 2026-11-22: "对 做吧"), so no build of it needs .NET."""
        tmnt = offline_pack.module('tmnt_build_shader_tools', 'games/tmnt/scripts/build_shader_tools.py')
        self.assertNotIn('subprocess', vars(tmnt))   # nothing is compiled
        cross = self.tmp / 'spirv-cross'
        cross.write_bytes(b'cross')
        with mock.patch.object(tmnt, 'program', return_value=cross):
            _, service = tmnt.build(self.tmp / 'tmnt')
            self.assertEqual(service.read_bytes(), (tmnt.PREBUILT / 'totk_xlate').read_bytes())
            other = self.tmp / 'prebuilt'
            other.mkdir()
            (other / 'totk_xlate').write_bytes(b'another build')
            shutil.copy2(tmnt.PREBUILT / 'totk_xlate.sha256', other / 'totk_xlate.sha256')
            with mock.patch.object(tmnt, 'PREBUILT', other), self.assertRaisesRegex(ValueError, 'not the pinned translator'):
                tmnt.build(self.tmp / 'tmnt-other')


if __name__ == '__main__':
    unittest.main()

"""Every test runs in the environment MNG Launcher carries (user 2026-11-25: "写测试，只用我们的 launcher里的 环境"): its
Python, and the packages a build installs from its offline store (games/totk/requirements.txt; CI installs the same
file). No second environment is set up to make a test pass.

A test that needs any other package asks whether it is installed and skips, and never imports it while its file is
being loaded: one such import stops every test of that file wherever the package is missing. The forms the scan below
takes as asking: importlib.util.find_spec('<package>') (also as `HAVE = …find_spec…` with `if HAVE:` or a skipUnless),
try / except ImportError that does not raise again, and contextlib.suppress(ImportError).

The tests themselves keep this, not a note (user 2026-11-25: "用测试保证 不是记忆和规则"): on a Mac that has MNG Launcher,
a run on any other Python, or in an environment holding a package the Launcher's offline store does not carry, fails
here. Where there is no Launcher (CI) the Python version and the package list stand in.

Not seen by the scan: a package a repository module imports on a test's behalf (the run in the Launcher's environment
shows it), and an import whose name is computed."""
import ast
import importlib.metadata
import os
import plistlib
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.deps import offline, offline_pack  # noqa: E402

# The Python the Launcher carries. The build already needs it (compression.zstd is in its standard library).
PYTHON = (3, 14)
REQUIREMENTS = 'games/totk/requirements.txt'
WORKFLOW = '.github/workflows/tests.yml'
# A package whose importable name is not its PyPI name
MODULE_OF = {'pycryptodome': 'Crypto', 'fonttools': 'fontTools', 'pillow': 'PIL'}
# Where a Launcher is looked for: the one running this job (MNG-Launcher bundled_job.py sets MNG_LAUNCHER_APP), else an installed one
LAUNCHERS = ('/Applications/MNG Launcher.app', '~/Applications/MNG Launcher.app')
BUNDLED_PYTHON = 'Contents/Resources/python'
STORES = '~/Library/Application Support/MacNativeGames/Builder/offline'   # bundled_job.py prepare_offline
WITH_PYTHON = {'pip'}   # comes with the Launcher's Python (ensurepip), not from the store
INTERPRETER = 'bin/python3.14'
HOW = ('Run the tests in the Launcher\'s environment: a venv made by a copy of its Python (cp -R "MNG Launcher.app/' +
       BUNDLED_PYTHON + '" <copy>; <copy>/bin/python3 -B -m venv <folder>; a copy, because a run without -B writes bytecode '
       'beside the standard library, and inside the signed app that breaks its signature), holding only wheels of its '
       'offline store (<folder>/bin/python -m pip install --no-index --find-links "<store>/wheels" -r ' + REQUIREMENTS +
       '; <store> is the folder in ' + STORES + ').')
MISSING = {'ImportError', 'ModuleNotFoundError', 'Exception', 'BaseException'}
TRIES = tuple(t for t in (ast.Try, getattr(ast, 'TryStar', None)) if t)
SCOPES = (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
A_PATH = re.compile(r'[\w.@+-]+(/[\w.@+ -]+)*')
FORMS = ('ask importlib.util.find_spec(\'<package>\') and skip, and import the package inside the test (or under `if` '
         'on that answer, in try / except ImportError, or in contextlib.suppress(ImportError))')


def installed_launcher(environ=None, places=LAUNCHERS):
    """The MNG Launcher of this Mac, or None."""
    named = (os.environ if environ is None else environ).get('MNG_LAUNCHER_APP')
    for place in ([named] if named else []) + list(places):
        app = Path(place).expanduser()
        if (app / BUNDLED_PYTHON / 'bin/python3').exists():
            return app
    return None


def launcher_store(app, stores=STORES):
    """The Launcher's unpacked offline store, or None."""
    try:
        digest = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes()).get('MNGOfflineStoreSHA256', '')
    except (OSError, ValueError):
        return None
    folder = Path(stores).expanduser() / digest
    return folder if re.fullmatch(r'[0-9a-f]{64}', digest) and (folder / offline.MANIFEST).is_file() else None


def normal(name):
    return re.sub(r'[-_.]+', '-', name or '(a package without a name)').lower()   # how pip compares package names


def made_by_launcher(base_prefix, app):
    """Whether the Python that runs (or made the venv that runs) the tests is the Launcher's own: the same interpreter
    file as the one the app carries, wherever it was copied to."""
    try:
        return (Path(base_prefix) / INTERPRETER).read_bytes() == (Path(app) / BUNDLED_PYTHON / INTERPRETER).read_bytes()
    except OSError:
        return False


def inside_app(base_prefix, app):
    """Whether that Python is the one inside the app itself. It must not be: 2026-11-25, runs of the app's own Python
    left 256 bytecode files in the installed Launcher and rewrote three it ships; codesign --verify --strict failed."""
    try:
        prefix, bundle = Path(base_prefix).resolve(), Path(app).resolve()
    except OSError:
        return False
    return prefix == bundle or bundle in prefix.parents


def not_carried(installed, wheel_names):
    """The installed (name, version) pairs that are no wheel of the store (wheel files: <name>-<version>-<tags>.whl)."""
    carried_wheels = {(normal(w.split('-')[0]), w.split('-')[1]) for w in wheel_names}
    return sorted((normal(n), v) for n, v in installed if normal(n) not in WITH_PYTHON and (normal(n), v) not in carried_wheels)


def carried():
    """The importable names of what games/totk/requirements.txt installs."""
    names = set()
    for line in (REPO / REQUIREMENTS).read_text().splitlines():
        package = re.split(r'[=<>!~\[; ]', line.split('#')[0].strip(), maxsplit=1)[0].lower()
        if package:
            names.add(MODULE_OF.get(package, package.replace('-', '_')))
    return names


def called(node):
    """The name a call or an exception type is written with: find_spec for importlib.util.find_spec(…)."""
    return getattr(node, 'attr', getattr(node, 'id', None))


def path_parts(node):
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        return path_parts(node.left) + path_parts(node.right)
    return [node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None]


def named_paths(tree):
    """What the file may name as a path: its string constants and the constant runs of its `a / 'b' / 'c'` chains and
    join(…) calls."""
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            runs = [[node.value]]
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
            runs = [path_parts(node)]
        elif isinstance(node, ast.Call) and called(node.func) in ('join', 'joinpath', 'Path'):
            runs = [[a.value if isinstance(a, ast.Constant) and isinstance(a.value, str) else None for a in node.args]]
        else:
            continue
        for parts in runs:
            run = []
            for part in parts:
                run = run + [part.strip('/')] if part else []
                if run:
                    names.add('/'.join(run))
    return {n for n in names if len(n) < 160 and '..' not in n and A_PATH.fullmatch(n)}


def import_folders(tree, repo=REPO):
    """The folders of this repository the file can import from: the repository, tests/, and every folder (or the folder
    of every file) it names, also below one of those (`GAME / 'scripts'`). A folder the file never names is not one: a
    folder that merely shares a package's name (games/unicorn) must not pass for that package."""
    found, names = {repo, repo / 'tests'}, named_paths(tree)
    for _ in range(4):
        more = set()
        for base in found:
            for name in names:
                path = base / name
                more.add(path if path.is_dir() else path.parent if path.is_file() else base)
        if more <= found:
            break
        found |= more
    return found


def reachable_folders(tree, repo=REPO, seen=None):
    """import_folders of the file and of the tests/ modules it imports: a helper (test_game_containers) puts its script
    folder on sys.path, and the files importing it import from there too."""
    seen = set() if seen is None else seen
    found = import_folders(tree, repo)
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            names = [node.module]
        else:
            continue
        for name in names:
            helper = repo / 'tests' / (name.split('.')[0] + '.py')
            if helper.is_file() and helper not in seen:
                seen.add(helper)
                found |= reachable_folders(ast.parse(helper.read_text(encoding='utf-8')), repo, seen)
    return found


def own_module(name, folders):
    return any((folder / (name + '.py')).is_file() or (folder / name).is_dir() for folder in folders)


def gives_up(handler):
    """An except block that ends the file's loading after all: raises (other than SkipTest) or exits."""
    for statement in handler.body:
        if isinstance(statement, ast.Raise) and 'SkipTest' not in ast.dump(statement):
            return True
        if isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call) and called(statement.value.func) in ('exit', '_exit', 'quit'):
            return True
    return False


def catches_missing(handler):
    kinds = handler.type.elts if isinstance(handler.type, ast.Tuple) else [handler.type]
    return (handler.type is None or any(called(k) in MISSING for k in kinds)) and not gives_up(handler)


def suppresses_missing(block):
    return any(isinstance(i.context_expr, ast.Call) and called(i.context_expr.func) == 'suppress'
               and any(called(a) in MISSING for a in i.context_expr.args) for i in block.items)


def asking(tree):
    """(the packages the file asks find_spec about, the names it keeps an answer in). A question covers the strings of
    its own statement or decorator: `HAVE = all(find_spec(n) for n in ('numpy', 'PIL'))` asks about both."""
    parents = {child: parent for parent in ast.walk(tree) for child in ast.iter_child_nodes(parent)}
    packages, answers = set(), set()
    for call in ast.walk(tree):
        if not (isinstance(call, ast.Call) and called(call.func) == 'find_spec'):
            continue
        part = call
        while part in parents and not isinstance(parents[part], SCOPES):
            part = parents[part]
        packages.update(c.value.split('.')[0] for c in ast.walk(part) if isinstance(c, ast.Constant) and isinstance(c.value, str))
        if isinstance(part, ast.Assign):
            answers.update(t.id for t in part.targets if isinstance(t, ast.Name))
    return packages, answers


def asks(test, answers):
    return any((isinstance(n, ast.Call) and called(n.func) == 'find_spec') or (isinstance(n, ast.Name) and n.id in answers)
               for n in ast.walk(test))


def imports(node, answers, loading=True, tried=False):
    """(line, top-level name, imported while the file loads, inside a guard against its absence) of every import below
    node; __import__('x') and import_module('x') count."""
    def below(parts, loading=loading, tried=tried):
        for part in parts:
            yield from imports(ast.Module(body=[part], type_ignores=[]), answers, loading, tried)

    for child in ast.iter_child_nodes(node):
        if isinstance(child, ast.Import):
            for alias in child.names:
                yield child.lineno, alias.name.split('.')[0], loading, tried
        elif isinstance(child, ast.ImportFrom):
            if child.level == 0 and child.module:
                yield child.lineno, child.module.split('.')[0], loading, tried
        elif isinstance(child, TRIES):
            guarded = tried or any(catches_missing(h) for h in child.handlers)
            yield from below(child.body, loading and not guarded, guarded)
            yield from below((*child.handlers, *child.orelse, *child.finalbody))
        elif isinstance(child, (ast.With, ast.AsyncWith)) and suppresses_missing(child):
            yield from imports(child, answers, False, True)
        elif isinstance(child, ast.If) and called(child.test) == 'TYPE_CHECKING':
            yield from below(child.orelse)   # the body never runs
        elif isinstance(child, ast.If):
            answered = asks(child.test, answers)   # any other condition (sys.platform, …) still imports while loading
            yield from below((*child.body, *child.orelse), loading and not answered)
        elif isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            yield from imports(child, answers, False, tried)
        else:
            if (isinstance(child, ast.Call) and called(child.func) in ('__import__', 'import_module') and child.args
                    and isinstance(child.args[0], ast.Constant) and isinstance(child.args[0].value, str)):
                yield child.lineno, child.args[0].value.split('.')[0], loading, tried
            yield from imports(child, answers, loading, tried)


def outside(source, known, repo=REPO):
    """[(line, package, what is wrong)] for the packages `source` uses that are neither in `known` nor its own modules."""
    tree = ast.parse(source)
    (asked, answers), folders, found = asking(tree), reachable_folders(tree, repo), []
    for line, name, loading, tried in imports(tree, answers):
        if name in known or own_module(name, folders):
            continue
        if loading:
            found.append((line, name, 'imported while the file loads'))
        elif not tried and name not in asked:
            found.append((line, name, 'used without asking whether it is installed'))
    return found


class TestsNeedOnlyTheLauncherEnvironment(unittest.TestCase):
    def standard_library(self):
        names = getattr(sys, 'stdlib_module_names', None)   # Python 3.10 and later
        if names is None:
            self.fail(f'these tests are running on Python {sys.version.split()[0]} ({sys.executable}). {HOW}')
        return set(names)

    def test_the_tests_run_on_the_python_the_launcher_carries(self):
        self.assertEqual(sys.version_info[:2], PYTHON,
                         f'these tests are running on Python {sys.version.split()[0]} ({sys.executable}). {HOW}')
        text = (REPO / WORKFLOW).read_text()
        versions = re.findall(r'^\s*python-version:\s*(.*?)\s*$', text, re.M)
        self.assertTrue(versions, f'{WORKFLOW} names no Python version')
        self.assertEqual({v.strip('\'"') for v in versions}, {'.'.join(map(str, PYTHON))}, f'{WORKFLOW} must test on the Launcher\'s Python')
        self.assertNotIn('python-version-file', text, f'{WORKFLOW} must name the Launcher\'s Python version itself')

    def test_on_a_mac_with_the_launcher_the_tests_run_in_its_environment(self):
        app = installed_launcher()
        if app is None:
            self.skipTest('no MNG Launcher on this machine (CI, a checkout-only Mac): the Python version and the package list stand in')
        self.assertTrue(made_by_launcher(sys.base_prefix, app),
                        f'these tests are running on {sys.executable} (Python of {sys.base_prefix}), not on the Python of {app}. {HOW}')
        self.assertFalse(inside_app(sys.base_prefix, app),
                         f'these tests are running on the Python inside the signed app ({sys.base_prefix}). {HOW}')
        store = launcher_store(app)
        self.assertIsNotNone(store, f'the offline store of {app} is not unpacked in {STORES}: open MNG Launcher once')
        installed = [(d.metadata['Name'], d.version) for d in importlib.metadata.distributions()]
        self.assertEqual(not_carried(installed, offline.manifest(store)['wheels']), [],
                         f'this environment ({sys.prefix}) holds packages, or versions, that {store}/wheels does not carry '
                         f'(after a Launcher update: make the environment again from the new store). {HOW}')

    def test_the_environment_check_tells_another_python_and_an_outside_package(self):
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            app, copy, other = tmp / 'MNG Launcher.app', tmp / 'deps/launcher-python', tmp / 'python.org/3.14'
            for prefix, interpreter in ((app / BUNDLED_PYTHON, b'the launcher\'s'), (copy, b'the launcher\'s'), (other, b'another build')):
                (prefix / 'bin').mkdir(parents=True)
                (prefix / INTERPRETER).write_bytes(interpreter)
            self.assertTrue(made_by_launcher(copy, app))    # the same interpreter, copied out of the app
            self.assertFalse(inside_app(copy, app))
            self.assertTrue(made_by_launcher(app / BUNDLED_PYTHON, app))
            self.assertTrue(inside_app(app / BUNDLED_PYTHON, app))   # the app's own: it would write into the signed app
            self.assertFalse(made_by_launcher(other, app))   # python.org's, Homebrew's, Xcode's
            self.assertFalse(made_by_launcher(tmp / 'nowhere', app))
            self.assertFalse(inside_app(other, app))
        wheels = ['capstone-5.0.9-py3-none-macosx_11_0_arm64.whl', 'ruamel.yaml-0.18.6-py3-none-any.whl',
                  'zbic-1.0.1.post1-cp314-cp314-macosx_11_0_arm64.whl']
        self.assertEqual(not_carried([('capstone', '5.0.9'), ('ruamel_yaml', '0.18.6'), ('Zbic', '1.0.1.post1'), ('pip', '99')], wheels), [])
        self.assertEqual(not_carried([('capstone', '5.0.9'), ('numpy', '2.3.4')], wheels), [('numpy', '2.3.4')])
        self.assertEqual(not_carried([('capstone', '5.0.8')], wheels), [('capstone', '5.0.8')])   # another version
        self.assertEqual(not_carried([(None, '1')], wheels), [('(a package without a name)', '1')])   # a broken install is named, not a TypeError
        self.assertIsNone(installed_launcher({}, ()))
        self.assertIsNone(installed_launcher({'MNG_LAUNCHER_APP': '/nonexistent/MNG Launcher.app'}, ()))
        self.assertIsNone(launcher_store('/nonexistent/MNG Launcher.app'))
        self.assertIn('--no-index', HOW)   # following the advice must not fetch newer dependencies than the store's

    def test_no_test_needs_a_package_the_launcher_does_not_carry(self):
        known, wrong = self.standard_library() | carried(), []
        for path in sorted((REPO / 'tests').glob('*.py')):
            for line, name, why in outside(path.read_text(encoding='utf-8'), known):
                wrong.append(f'tests/{path.name}:{line}: {name}: {why}')
        self.assertEqual(wrong, [], f'a test may use only the Launcher\'s Python and what {REQUIREMENTS} installs; for anything '
                         f'else {FORMS}:\n' + '\n'.join(wrong))

    def test_what_the_tests_install_is_in_the_launcher_offline_store(self):
        sets = dict(offline_pack.requirement_sets())
        self.assertIn(REQUIREMENTS, sets, 'the offline store no longer carries the wheels of ' + REQUIREMENTS)
        self.assertTrue(sets[REQUIREMENTS], REQUIREMENTS + ' lists nothing')
        self.assertEqual(carried(), {MODULE_OF.get(r.split('==')[0].lower(), r.split('==')[0].lower().replace('-', '_'))
                                     for r in sets[REQUIREMENTS]})

    def test_the_scan_tells_each_way_of_needing_an_outside_package(self):
        known = self.standard_library() | {'capstone'}
        loads, unasked = 'imported while the file loads', 'used without asking whether it is installed'
        ask = 'import importlib.util\nHAVE = importlib.util.find_spec("numpy")\n'
        wrong = {
            'import numpy\n': [(1, 'numpy', loads)],
            'from scipy import ndimage\n': [(1, 'scipy', loads)],
            ask + 'import numpy\n': [(3, 'numpy', loads)],   # asking does not help an import made while the file loads
            'class T:\n    import numpy\n': [(2, 'numpy', loads)],
            'def test():\n    import numpy\n': [(2, 'numpy', unasked)],
            'import importlib.util\nHAVE = importlib.util.find_spec("scipy")\ndef test():\n    import numpy\n': [(4, 'numpy', unasked)],
            'NAME = "numpy"\nimport importlib.util\nHAVE = importlib.util.find_spec("scipy")\ndef test():\n    import numpy\n':
                [(5, 'numpy', unasked)],   # a string elsewhere in the file is not a question
            'try:\n    import numpy\nexcept KeyError:\n    numpy = None\n': [(2, 'numpy', loads)],
            'try:\n    pass\nexcept ImportError:\n    import numpy\n': [(4, 'numpy', loads)],
            'try:\n    import numpy\nexcept ImportError:\n    raise\n': [(2, 'numpy', loads)],   # still stops the file
            'import sys\ntry:\n    import numpy\nexcept Exception:\n    sys.exit("no numpy")\n': [(3, 'numpy', loads)],
            ask + 'import sys\nif sys.platform == "darwin":\n    import numpy\n': [(5, 'numpy', loads)],   # not the answer's `if`
            'numpy = __import__("numpy")\n': [(1, 'numpy', loads)],
            'import importlib\ndef test():\n    importlib.import_module("numpy.linalg")\n': [(3, 'numpy', unasked)],
            'import unicorn\n': [(1, 'unicorn', loads)],   # the repository's games/unicorn folder is not the unicorn package
            'import build\nimport lldb\n': [(1, 'build', loads), (2, 'lldb', loads)],
        }
        for source, expected in wrong.items():
            self.assertEqual(outside(source, known), expected, source)
        right = (
            'import os, json\nfrom pathlib import Path\n',
            'import capstone\nfrom capstone.arm64 import ARM64_OP_IMM\n',
            'from tools.deps import offline\nfrom . import helper\nimport switch_packages\n',   # the repository, tests/
            'import sys\nfrom pathlib import Path\nGAME = Path(__file__).resolve().parents[1] / "games/oot"\n'
            'sys.path.insert(0, str(GAME / "scripts"))\nimport prepare_ns2_seed\n',   # a folder the file names
            'import importlib.util\nHAVE = all(importlib.util.find_spec(n) for n in ("numpy", "PIL"))\n'
            'def test():\n    import numpy\n    from PIL import Image\n',
            ask + 'if HAVE:\n    import numpy\n',
            'import importlib.util\nif importlib.util.find_spec("numpy.linalg"):\n    import numpy\n',
            'import importlib.util, unittest\n@unittest.skipUnless(importlib.util.find_spec("numpy"), "needs numpy")\n'
            'def test():\n    import numpy\n',
            'try:\n    import numpy\nexcept ImportError:\n    numpy = None\n',
            'import builtins\ntry:\n    import numpy\nexcept builtins.ImportError:\n    numpy = None\n',
            'import unittest\ntry:\n    import numpy\nexcept ImportError:\n    raise unittest.SkipTest("needs numpy")\n',
            'def test():\n    try:\n        import numpy\n    except ModuleNotFoundError:\n        return\n',
            'import contextlib\nwith contextlib.suppress(ImportError):\n    import numpy\n',
            'from typing import TYPE_CHECKING\nif TYPE_CHECKING:\n    import numpy\n',
        )
        for source in right:
            self.assertEqual(outside(source, known), [], source)
        if hasattr(ast, 'TryStar'):
            self.assertEqual(outside('try:\n    import numpy\nexcept* ImportError:\n    pass\n', known), [])


if __name__ == '__main__':
    unittest.main()

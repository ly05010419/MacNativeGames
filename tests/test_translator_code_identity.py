"""A shader translator is its code, not its file: another signature is the same translator (2026-11-18, user: "好").

MNG Launcher ships this repository's translator signed with a Developer ID, whose signature carries a time stamp, so each
release is another file of the same code. Named by the whole file, the translation context made a Launcher build and a
command-line build of one work folder translate all 42,848 Ocarina of Time shaders again for each other, refused the
express update after a Launcher build, and set the device's cache folder aside at every switch. These tests sign one
program two ways and check that games/oot/scripts/prepare_ns2_seed.py's code_sha256, the runtime's totk_code_sha256
(totk_rt_translator.h) and games/oot/build.py's seed_context_matches treat both as one translator, and another program
as another; and that a translation cache named the old way is taken over without translating again."""
import contextlib
import hashlib
import importlib.util
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@contextlib.contextmanager
def clean_imports(folder=None):
    """sys.path and sys.modules as they were afterwards: Ocarina of Time and Tears of the Kingdom both have a
    prepare_shader_seed.py and an ns2_shader_corpus.py, and other tests import the other game's
    (tests/test_ns2_shader_seed.py); games/oot/build.py puts its scripts on the path when it is called."""
    shared = {n: sys.modules.pop(n) for n in ('prepare_shader_seed', 'prepare_ns2_seed', 'ns2_shader_corpus', 'shader_list')
              if n in sys.modules}   # another game's of the same name, imported by another test module
    path_before, modules_before = list(sys.path), set(sys.modules)
    if folder:
        sys.path.insert(0, str(folder))
    try:
        yield
    finally:
        sys.path[:] = path_before
        for loaded in set(sys.modules) - modules_before:
            del sys.modules[loaded]
        sys.modules.update(shared)


def isolated(name, path, folder):
    """A script loaded under its own module name, its folder on the path only while it loads."""
    with clean_imports(folder):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


seed = isolated('oot_prepare_ns2_seed_code', GAME / 'scripts/prepare_ns2_seed.py', GAME / 'scripts')

RUNTIME = r'''#import <Foundation/Foundation.h>
#include "totk_rt_translator.h"
int main(int argc, char **argv) { @autoreleasepool { (void)argc; NSString *h = totk_code_sha256(@(argv[1])); printf("%s\n", h ? h.UTF8String : "nil"); return 0; } }
'''


def whole(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_build():
    return isolated('oot_build_code_identity', GAME / 'build.py', GAME)


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun') and shutil.which('codesign'), 'requires macOS and Xcode')
class CodeIdentity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def program(self, name, text):
        source = self.base / (name + '.c')
        source.write_text(f'#include <stdio.h>\nint main(void) {{ puts("{text}"); return 0; }}\n')
        out = self.base / name
        built = subprocess.run(['xcrun', 'clang', '-O1', str(source), '-o', str(out)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        return out

    def signed(self, program, name, *options):
        """A copy of `program` signed ad hoc with these codesign options (another identifier and entitlements give another
        signature of another size, as the Developer ID one is beside this repository's)."""
        copy = self.base / name
        shutil.copy2(program, copy)
        done = subprocess.run(['codesign', '--force', '--sign', '-', *options, str(copy)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        return copy

    def two_signatures(self):
        program = self.program('xlate', 'translator')
        entitlements = self.base / 'e.plist'
        entitlements.write_text('<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                                '"http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict>'
                                '<key>com.apple.security.cs.allow-jit</key><true/></dict></plist>')
        a = self.signed(program, 'a', '--identifier', 'totk_xlate')
        b = self.signed(program, 'b', '--identifier', 'org.example.another.longer.identifier', '--options', 'runtime',
                        '--entitlements', str(entitlements))
        self.assertNotEqual(whole(a), whole(b))                         # two files
        self.assertNotEqual(a.stat().st_size, b.stat().st_size)          # of different signature sizes
        return a, b

    def test_two_signatures_of_one_program_are_one_code(self):
        a, b = self.two_signatures()
        self.assertEqual(seed.code_sha256(a), seed.code_sha256(b))
        other = self.signed(self.program('other', 'another translator'), 'c', '--identifier', 'totk_xlate')
        self.assertNotEqual(seed.code_sha256(other), seed.code_sha256(a))
        self.assertEqual(seed.translation_context(a, 'minus-w'), seed.translation_context(b, 'minus-w'))
        self.assertNotEqual(seed.legacy_translation_context(a, 'minus-w'), seed.legacy_translation_context(b, 'minus-w'))

    def test_anything_but_a_signed_mach_o_is_hashed_whole(self):
        text = self.base / 'script'
        text.write_bytes(b'#!/bin/sh\necho translator\n')
        self.assertEqual(seed.code_sha256(text), whole(text))

    def test_the_runtime_gives_the_same_code_hash(self):
        a, b = self.two_signatures()
        source = self.base / 'code.m'
        source.write_text(RUNTIME)
        exe = self.base / 'code'
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Werror', '-I', str(NVN), '-framework', 'Foundation',
                                str(source), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        for program in (a, b):
            out = subprocess.run([str(exe), str(program)], capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(out, seed.code_sha256(program))

    def test_a_cache_named_the_old_way_is_taken_over(self):
        a, b = self.two_signatures()
        root = self.base / 'ns2-seed-cache/oot'
        old = root / seed.legacy_translation_context(b, 'minus-w')   # made by the other signature, named by its file
        (old / 'tool/build/shader-service').mkdir(parents=True)
        shutil.copy2(b, old / 'tool/build/shader-service/totk_xlate')
        (old / 'abc.variants').write_text('translations')
        foreign = root / seed.legacy_translation_context(self.program('other', 'another translator'), 'minus-w')
        (foreign / 'tool/build/shader-service').mkdir(parents=True)
        shutil.copy2(self.base / 'other', foreign / 'tool/build/shader-service/totk_xlate')
        taken = seed.adopt_legacy_cache(root, a, 'minus-w')
        self.assertEqual(taken, old)
        now = root / seed.translation_context(a, 'minus-w')
        self.assertEqual((now / 'abc.variants').read_text(), 'translations')   # nothing translated again
        self.assertTrue(foreign.exists())                                       # another program's cache stays as it is
        self.assertIsNone(seed.adopt_legacy_cache(root, a, 'minus-w'))          # once there, nothing more to take


def load_totk_seed():
    return isolated('totk_prepare_shader_seed_code', ROOT / 'games/totk/scripts/prepare_shader_seed.py', ROOT / 'games/totk/scripts')


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'requires macOS')
class TearsOfTheKingdom(unittest.TestCase):
    """games/totk/scripts/prepare_shader_seed.py (Tears of the Kingdom, Breath of the Wild): the translation cache and the
    seed name the translator by its code; a cache or seed named by this very file before is kept."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.totk = load_totk_seed()

    def tearDown(self):
        self.temp.cleanup()

    def test_the_same_code_hash_as_the_other_copies(self):
        repo = GAME / 'prebuilt/macos-arm64/totk_xlate'
        resigned = self.base / 'totk_xlate'
        shutil.copy2(repo, resigned)
        subprocess.run(['codesign', '--force', '--sign', '-', '--identifier', 'org.example.launcher.copy', str(resigned)],
                       capture_output=True, check=True)
        self.assertNotEqual(whole(resigned), whole(repo))
        self.assertEqual(self.totk.code_digest(resigned), self.totk.code_digest(repo))
        self.assertEqual(self.totk.code_digest(repo), seed.code_sha256(repo))

    def test_seed_and_cache_name_the_translator_by_its_code(self):   # never by the whole file again
        source = (ROOT / 'games/totk/scripts/prepare_shader_seed.py').read_text()
        body = source[source.index('def prepare('):]
        self.assertIn('translator_code_sha256=code_digest(translator)', body)
        self.assertIn("translator_code_sha256=identity['translator_code_sha256']", body)
        self.assertNotIn('translator_sha256=digest(translator.read_bytes())', body)

    def test_a_seed_named_by_this_file_before_is_reused(self):
        output = self.base / 'seed'
        output.mkdir()
        identity = dict(schema=1, version='v', translator_code_sha256='code')
        old = dict(schema=1, version='v', translator_sha256='this-file', shaders=0, files={'shader-keys.json': 'x'})
        (output / 'seed-manifest.json').write_text(json.dumps(old))
        said = []
        for name in ('this-file', 'another-file'):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertIsNone(self.totk.reusable_seed(output, identity, min_shaders=1, translator_file=name))
            said.append(out.getvalue())
        self.assertIn('fewer than', said[0])                 # the same file: past the translator, stopped by the next check
        self.assertIn('translator_code_sha256', said[1])     # another file: prepared again for it

    def cache(self, recorded, identity, legacy):
        folder = self.base / 'seed.translations'
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir()
        (folder / 'cache.json').write_text(json.dumps(recorded))
        (folder / 'abc-0123456789ab_b.metal').write_text('translated before')
        self.totk.TranslationCache(folder, identity, legacy=legacy)
        return folder

    def test_a_cache_named_by_this_file_before_is_kept(self):
        identity = dict(schema=1, chain='c', translator_code_sha256='code', spirv_cross_sha256='s', request_sha256='r')
        legacy = dict(schema=1, chain='c', translator_sha256='this-file', spirv_cross_sha256='s', request_sha256='r')
        kept = self.cache(legacy, identity, legacy)
        self.assertTrue((kept / 'abc-0123456789ab_b.metal').exists())
        self.assertEqual(json.loads((kept / 'cache.json').read_text()), identity)   # named by its code from now on
        emptied = self.cache(dict(legacy, translator_sha256='another-file'), identity, legacy)
        self.assertFalse((emptied / 'abc-0123456789ab_b.metal').exists())       # another translator's: made again


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'requires macOS')
class SeedContext(unittest.TestCase):
    """games/oot/build.py seed_context_matches: the express update after a Launcher build (2026-11-18) is not refused."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.tree = Path(self.temp.name) / 'oot'
        self.build = load_build()
        self.repo = GAME / 'prebuilt/macos-arm64/totk_xlate'

    def tearDown(self):
        self.temp.cleanup()

    def matches(self, meta):
        with clean_imports():
            return self.build.seed_context_matches(self.tree, meta)

    def service(self, program):
        target = self.tree / 'build/shader-service/totk_xlate'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(program, target)
        return target

    def test_a_seed_of_the_repository_translator_matches(self):
        self.assertTrue(self.matches(dict(
            nvn_depth_mode='minus-w', translation_context_sha256=seed.translation_context(self.repo, 'minus-w'))))

    def test_a_seed_named_by_another_signature_of_the_same_code_matches(self):
        resigned = self.service(self.repo)
        done = subprocess.run(['codesign', '--force', '--sign', '-', '--identifier', 'org.example.launcher.copy', str(resigned)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotEqual(whole(resigned), whole(self.repo))
        meta = dict(nvn_depth_mode='minus-w', translator_sha256=whole(resigned),
                    translation_context_sha256=seed.legacy_translation_context(resigned, 'minus-w'))
        self.assertTrue(self.matches(meta))
        self.assertFalse(self.matches(dict(meta, nvn_depth_mode='zero')))   # another depth mode

    def test_a_seed_of_another_translator_does_not_match(self):
        other = Path(self.temp.name) / 'other'
        other.write_bytes(b'not the translator')
        program = self.service(other)
        self.assertFalse(self.matches(dict(
            nvn_depth_mode='minus-w', translator_sha256=whole(program),
            translation_context_sha256=seed.legacy_translation_context(program, 'minus-w'))))


# Every game's packaging script that names the translator in a cache or seed identity (user 2026-11-18: "做" the
# community games and Bananza / Pokopia too), and the function in it that hashes the translator's code.
CODE_HASHES = {
    'games/oot/scripts/prepare_ns2_seed.py': 'code_sha256', 'games/totk/scripts/prepare_ns2_seed.py': 'code_sha256',
    'games/totk/scripts/prepare_shader_seed.py': 'code_digest', 'games/oot/scripts/prepare_shader_seed.py': 'code_digest',
    'games/kirby/third_party/totk-scripts/prepare_shader_seed.py': 'code_digest',
    'games/odyssey/third_party/totk-scripts/prepare_shader_seed.py': 'code_digest',
    'games/pla/scripts/prepare_shader_seed.py': 'code_digest', 'games/ssbu/scripts/prepare_shader_seed.py': 'code_digest',
    'games/dread/scripts/prepare_shader_seed.py': 'code_digest', 'games/acnh/tools/prepare_shader_seed.py': 'code_digest',
    'games/eow/scripts/prepare_shader_seed.py': 'code_digest', 'games/kirby/scripts/prepare_shader_seed.py': 'code_digest',
    'games/tmnt/scripts/tmnt_shaders.py': 'code_digest', 'games/xc3/scripts/translate_shaders.py': 'code_digest',
}
# how a translator was named by its whole file in a cache or seed identity before 2026-11-18
WHOLE_FILE = re.compile(r"translator_sha256=(?:digest|sha256|file_digest)\((?:translator|work /)|'translator': digests\.sha256\("
                        r"|\btranslator=digest\(translator\)|translator_sha256=identity\['translator_sha256'\]")


def function_only(path, name):
    """`name` from the script at `path`, run on its own (hashlib, struct and Path given): the script's other imports
    (another game's modules, ROM tools) are not needed to check the hash."""
    import ast
    tree = ast.parse((ROOT / path).read_text())
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == name]
    consts = [n for n in tree.body if isinstance(n, ast.Assign) and any(
        getattr(t, 'id', '') == 'MH_MAGIC_64' or any(getattr(e, 'id', '') == 'MH_MAGIC_64' for e in getattr(t, 'elts', []))
        for t in n.targets)]
    scope = dict(hashlib=hashlib, struct=__import__('struct'), Path=Path)
    exec(compile(ast.Module(body=consts + nodes, type_ignores=[]), path, 'exec'), scope)
    return scope[name]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'requires macOS')
class EveryGame(unittest.TestCase):
    def test_no_identity_names_the_translator_by_its_file(self):
        found = []
        for path in sorted(ROOT.glob('games/*/**/*.py')):
            if '/tests/' in str(path) or path.name == 'verify_vertex_counter.py':
                continue
            function = ''
            for n, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
                if line.startswith('def '):
                    function = line[4:].split('(')[0]
                # allowed: the manifest keeps the whole file's hash as information (the runtime's check of the App's
                # in-game translator), and legacy_translation_context recognises the caches named that way before
                if function == 'legacy_translation_context' or re.match(r'\s*(report|manifest) = dict\(', line):
                    continue
                if WHOLE_FILE.search(line):
                    found.append(f'{path.relative_to(ROOT)}:{n}: {line.strip()[:140]}')
        self.assertEqual(found, [], 'a cache or seed identity names the translator by its whole file again: every '
                                    'Launcher release would translate everything again; use the code hash\n' + '\n'.join(found))

    def test_every_copy_hashes_the_code_as_the_shared_function_does(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            import translator_code
        finally:
            sys.path.remove(str(ROOT / 'tools'))
        oot = GAME / 'prebuilt/macos-arm64/totk_xlate'
        with tempfile.TemporaryDirectory() as temp:
            resigned = Path(temp) / 'totk_xlate'
            shutil.copy2(oot, resigned)
            subprocess.run(['codesign', '--force', '--sign', '-', '--identifier', 'org.example.launcher.copy', str(resigned)],
                           capture_output=True, check=True)
            want = translator_code.code_sha256(oot)
            self.assertEqual(translator_code.code_sha256(resigned), want)
            for path, name in CODE_HASHES.items():
                with self.subTest(script=path):
                    code = function_only(path, name)
                    self.assertEqual(code(oot), want)
                    self.assertEqual(code(resigned), want)   # another signature: the same translator


if __name__ == '__main__':
    unittest.main()

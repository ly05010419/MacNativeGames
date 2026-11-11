"""Exercise the actual runtime patch on authored data, without a game dump or GPU."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
START, SITE, NAME = 0xec1f60, 0xec1f78, 0x3710a05
TITLE = b'AreaMergeTarRenderer\0'


def function(text, name):
    match = re.search(r'static void ' + name + r'\([^)]*\)\s*\{', text)
    if not match:
        raise AssertionError('Missing source helper: ' + name)
    end, depth = match.end(), 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[match.start():end]


@unittest.skipUnless(shutil.which('clang'), 'clang required')
class AreaMergeHeap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.tmp.name)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.source = (GAME / 'runtime/game_patches.c').read_text()
        cls.helper = function(cls.source, 'patch_area_merge_heap')
        (cls.directory / 'heap_helper.h').write_text(cls.helper)
        (cls.directory / 'patch_helper.h').write_text(function(cls.source, 'put_word'))
        words = re.search(r'uint32_t creation\[\] = \{(.*?)\};', cls.helper, re.S).group(1)
        creation = struct.pack('<12I', *[int(x, 16) for x in re.findall(r'0x[0-9a-f]+', words)])
        # Pin the fingerprint independently of the fixture generated from it.
        if hashlib.sha256(creation).hexdigest() != 'dcdf27632881573a1951869f662756d8030ef62d70e0a74a7adc3ee90ffb104b':
            raise AssertionError('Unexpected verified 1.0.0 creation fingerprint')
        cls.fixture = cls.directory / 'synthetic-image'
        with cls.fixture.open('wb') as f:
            f.truncate(NAME + len(TITLE))
            f.seek(START)
            f.write(creation)
            f.seek(NAME)
            f.write(TITLE)
        cls.executables = {}

    def probe(self, version, sanitize=False):
        key = version, sanitize
        if key not in self.executables:
            executable = self.directory / ('probe-' + version + ('-ubsan' if sanitize else ''))
            command = ['clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-I' + str(self.directory)]
            if version == 'v121':
                command += ['-DTOTK_GAME_V121=1']
            if version == 'ios':
                command += ['-UTARGET_OS_IPHONE', '-DTARGET_OS_IPHONE=1']
            if sanitize:
                command += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            command += [str(GAME / 'tests/area_merge_heap/probe.c'), '-o', str(executable)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            self.executables[key] = executable
        return self.executables[key]

    def run_probe(self, version, sanitize=False, disabled=False, export=False):
        env = dict(os.environ)
        for key in ('TOTK_AREA_MERGE_HEAP', 'TEST_HEAP_OFF', 'TOTK_EXPORT_IOS'):
            env.pop(key, None)
        if disabled:
            env.update(TOTK_AREA_MERGE_HEAP='0', TEST_HEAP_OFF='1')
        if export:
            env.update(TOTK_EXPORT_IOS='fixture-only', TEST_HEAP_OFF='1')
        run = subprocess.run([str(self.probe(version, sanitize)), str(self.fixture)],
                             env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('PASS', run.stdout)

    def test_single_word_change_guards_and_excluded_version(self):
        for version in ('v0', 'v121', 'ios'):
            for sanitize in (False, True):
                with self.subTest(version=version, ubsan=sanitize):
                    self.run_probe(version, sanitize)

    def test_diagnostic_disable_keeps_original_heap(self):
        self.run_probe('v0', disabled=True)

    def test_ios_export_keeps_original_heap(self):
        self.run_probe('v0', export=True)

    def test_creation_instructions_reserved_only_for_v0(self):
        spec = importlib.util.spec_from_file_location('area_merge_patch_sites', GAME / 'scripts/patch_sites.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        expected = {START + 4 * i for i in range(12)}
        for version in ('v0', 'v121', 'v143'):
            actual = {site.offset for site in module.sites(version)}
            if version == 'v0':
                self.assertTrue(expected <= actual)
            else:
                self.assertFalse(expected & actual)

    def test_call_follows_embedded_early_return(self):
        body = self.source[self.source.index('void runtime_game_patches(Module *m)'):]
        self.assertLess(body.index('return;'), body.index('patch_area_merge_heap(m);'))
        self.assertIn('runtime/game_patches_v143.c', (GAME / 'Makefile').read_text())


if __name__ == '__main__':
    unittest.main()

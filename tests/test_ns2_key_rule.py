"""The Switch 2 shader key (2026-11-12): control +0x08..+0x0b, +0x20..+0x27 and +0x30 hashed as zero, the same in the runtime
(nvn_ctl.h ctl_key_update) and the tools (ns2_shader_corpus.py shader_key); its name, which a seed records and the update
checks (an express update kept a seed of the old keys); and ns2_rekey.py, which renames a list's rows."""
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(SCRIPTS))


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


corpus, rekey = load('ns2_shader_corpus'), load('ns2_rekey')
_spec = importlib.util.spec_from_file_location('oot_ns2_shader_corpus', ROOT / 'games/oot/scripts/ns2_shader_corpus.py')
oot_corpus = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(oot_corpus)
from shader_list import ShaderList  # noqa: E402
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402
import update_runtime  # noqa: E402


def control(**fields):
    c = bytearray(0xe0)
    struct.pack_into('<II', c, 0, 0x98761234, 0xc9)
    struct.pack_into('<III', c, 0x38, 0x100, 0, 0)
    struct.pack_into('<II', c, 0x40, 0x100, 0x100)
    for offset, value in fields.items():
        c[int(offset[1:], 16)] = value
    return bytes(c)


CODE = bytes(range(256))
PROBE = r'''
#include <stdio.h>
#include "nvn_ctl.h"
int main(int argc, char **argv) {
    unsigned char c[0xe0]; FILE *f = fopen(argv[1], "rb"); fread(c, 1, sizeof c, f); fclose(f);
    unsigned char code[256]; for (int i = 0; i < 256; i++) code[i] = (unsigned char)i;
    unsigned char md[32]; CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx); ctl_key_update(&ctx, c); CC_SHA256_Update(&ctx, code, 256);
    CC_SHA256_Final(md, &ctx); for (int i = 0; i < 32; i++) printf("%02x", md[i]); printf("\n"); return 0;
}
'''


class KeyRule(unittest.TestCase):
    def test_the_fields_a_game_writes_again_do_not_change_the_key(self):
        base = corpus.shader_key(control(), CODE)
        for field in ('x08', 'x0b', 'x20', 'x27', 'x30'):
            self.assertEqual(corpus.shader_key(control(**{field: 0x55}), CODE), base, field)
        for field in ('x38', 'x44', 'x18'):   # instructions, code area, a field the key keeps (+0x56 is past the 0x48 it hashes)
            self.assertNotEqual(corpus.shader_key(control(**{field: 0x55}), CODE), base, field)

    def test_the_old_rule_saw_them(self):
        self.assertNotEqual(rekey.old_key(control(x20=1), CODE), rekey.old_key(control(), CODE))
        self.assertEqual(rekey.old_key(control(x30=1), CODE), rekey.old_key(control(), CODE))

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
    def test_the_runtime_hashes_as_the_tools_do(self):
        # Each game's runtime (the shared layer built with its defines) and that game's own tools: Ocarina of Time keeps
        # its rule (2026-11-13: on the shared layer it took the one above, and its seed named none of its list's shaders).
        for defines, tools in (([], corpus), (['-DGAME_NS2=1', '-DGAME_OOT=1'], oot_corpus)):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as tmp:
                tmp = Path(tmp)
                (tmp / 'p.c').write_text(PROBE)
                build = subprocess.run(['xcrun', 'clang', '-Wall', '-Werror', *defines, f'-I{NVN}', str(tmp / 'p.c'), '-o', str(tmp / 'p')],
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for fields in ({}, {'x08': 17, 'x20': 9, 'x26': 200, 'x30': 7}, {'x38': 1}):
                    c = control(**fields)
                    (tmp / 'c.bin').write_bytes(c)
                    out = subprocess.run([str(tmp / 'p'), str(tmp / 'c.bin')], capture_output=True, text=True).stdout.strip()
                    self.assertEqual(out, tools.shader_key(c, CODE), fields)

    def test_ocarina_keeps_its_own_rule(self):
        base = oot_corpus.shader_key(control(), CODE)
        self.assertEqual(oot_corpus.shader_key(control(x30=0x55), CODE), base)
        for field in ('x08', 'x20'):   # its list and seeds were made with these in the key
            self.assertNotEqual(oot_corpus.shader_key(control(**{field: 0x55}), CODE), base, field)


class KeyRuleName(unittest.TestCase):
    def test_the_runtime_and_the_tools_name_the_same_rule(self):
        text = (NVN / 'nvn_ctl.h').read_text()
        rules = dict((g, r) for r, g in re.findall(r'#define NVN2_KEY_RULE "([^"]+)"\s*/\* game: (\w+)', text))
        self.assertEqual(rules, {'totk': corpus.KEY_RULE, 'oot': oot_corpus.KEY_RULE})
        self.assertEqual(update_runtime.runtime_key_rule(), corpus.KEY_RULE)

    def test_the_seed_records_it(self):
        text = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertIn('from ns2_shader_corpus import KEY_RULE', text)
        self.assertEqual(text.count('shader_key_rule=KEY_RULE'), 2, 'in the manifest and in the inputs seed_unchanged compares')

    def seed(self, root, **manifest):
        (root / 'build/shader-seed').mkdir(parents=True, exist_ok=True)
        (root / 'build/shader-seed/seed-manifest.json').write_text(json.dumps(manifest))
        return root

    def test_an_update_makes_a_seed_of_another_rule_again(self):
        chain, rule = update_runtime.runtime_chain(), update_runtime.runtime_key_rule()
        ns2 = next(iter(build_app.NS2.values()))['version']
        with tempfile.TemporaryDirectory() as tmp:
            for recorded in (None, 'nvn2-an-older-rule'):
                res = self.seed(Path(tmp), chain=chain, version=ns2, **({'shader_key_rule': recorded} if recorded else {}))
                with self.assertRaisesRegex(build_app.BuildError, 'shader key rule.*Pass --seed'):
                    update_runtime.check_seed_chain(res, chain)
                self.assertTrue(update_runtime.check_seed_chain(res, chain, remake=True), recorded)
            res = self.seed(Path(tmp), chain=chain, version=ns2, shader_key_rule=rule)
            self.assertFalse(update_runtime.check_seed_chain(res, chain, remake=True))
            update_runtime.check_seed_chain(res, chain)

    def test_a_switch_1_seed_has_no_key_rule(self):
        chain = update_runtime.runtime_chain()
        with tempfile.TemporaryDirectory() as tmp:
            res = self.seed(Path(tmp), chain=chain, version='v143')
            self.assertFalse(update_runtime.check_seed_chain(res, chain, remake=True))


class ListKeyRule(unittest.TestCase):
    def test_the_lists_say_the_runtimes_rule(self):
        for game in ('dkb', 'pokopia'):
            self.assertEqual(corpus.list_key_rule(ROOT / f'games/{game}/assets/shader_list.tsv'), corpus.KEY_RULE, game)

    def test_a_rule_line_only_in_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'l.tsv'
            path.write_text('# a list\nV\t' + 'a' * 64 + '\t{}\n# Shader key rule: late\n')
            self.assertIsNone(corpus.list_key_rule(path))
            path.write_text('# a list\n# Shader key rule: r1\nV\t' + 'a' * 64 + '\t{}\n')
            self.assertEqual(corpus.list_key_rule(path), 'r1')

    def test_the_seed_and_the_merge_refuse_a_list_of_another_rule(self):
        seed = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertIn('if list_key_rule(a.list) != KEY_RULE', seed)
        merge = (SCRIPTS / 'collect_shaders.py').read_text()
        self.assertIn('list_key_rule(GAMES[game]) != KEY_RULE', merge)

    def test_rekey_writes_the_rule_into_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            listed = tmp / 'l.tsv'
            listed.write_text('# a list\n# Shader key rule: old\nV\t' + 'a' * 64 + '\t{}\n')
            with unittest.mock.patch.object(rekey, 'scan', lambda *a: []), \
                    unittest.mock.patch.object(sys, 'argv', ['ns2_rekey.py', '--romfs', str(tmp), '--modules', str(tmp / 'm'),
                                                             '--list', str(listed), '--apply']):
                rekey.main()
            self.assertEqual(listed.read_text().splitlines()[:2], ['# a list', f'# Shader key rule: {corpus.KEY_RULE}'])


class Rekey(unittest.TestCase):
    def test_rows_are_renamed_and_unknown_ones_left_out(self):
        a_old, b_old, gone = 'a' * 64, 'b' * 64, 'c' * 64
        renames = {a_old: '1' * 64, b_old: '2' * 64}
        name = lambda k: f'{k}-0123456789ab_b'
        key = lambda vs, fs: (name(vs).encode().ljust(80, b'\0') + name(fs).encode().ljust(80, b'\0') + bytes(16)).hex()
        shared = ShaderList()
        shared.variants = {(a_old, '{}'), (gone, '{}')}
        shared.warm = {('S', name(a_old), '5'): 'm', ('R', key(a_old, b_old)): 'tm', ('R', key(a_old, gone)): 'm'}
        shared.locators = {(b_old, 'module:main', 1, 2, 3), (gone, 'module:main', 4, 5, 6)}
        lost = rekey.rekey(shared, renames)
        self.assertEqual(shared.variants, {('1' * 64, '{}')})
        self.assertEqual(shared.warm, {('S', name('1' * 64), '5'): 'm', ('R', key('1' * 64, '2' * 64)): 'tm'})
        self.assertEqual(shared.locators, {('2' * 64, 'module:main', 1, 2, 3)})
        self.assertEqual(lost, {'V': 1, 'L': 1, 'R': 1})

    def test_a_dump_must_hash_to_its_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = control(x20=3)
            good = Path(tmp) / f'{rekey.old_key(c, CODE)}.ctl'
            good.write_bytes(c); good.with_suffix('.code').write_bytes(CODE)
            self.assertEqual(rekey.dumped(tmp), [(c, CODE)])
            (Path(tmp) / ('f' * 64 + '.ctl')).write_bytes(c); (Path(tmp) / ('f' * 64 + '.code')).write_bytes(CODE)
            with self.assertRaises(SystemExit):
                rekey.dumped(tmp)


if __name__ == '__main__':
    unittest.main()

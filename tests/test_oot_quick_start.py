"""Ocarina of Time's quick build (MNG Launcher 快速启动; user 2026-11-13: "就带上 菜单的 着色器就行"): the App carries only
the shaders the shared list's title rows name, the game translates the rest at first use, and 补全着色器 (update_shaders)
makes it a full App again. Checked here: the list's title shaders and its cut to them (shader_list.py title_keys / only),
the corpus cut (prepare_ns2_seed.py corpus_subset / only_keys), the seed's scope still naming every shader of the game,
the shader mode a build and an update take (games/oot/build.py shader_mode) and the capture switch that records the
title rows (nvn_mtl.m TOTK_PSO_CAPTURE)."""
import importlib.util
import os
import plistlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
SCRIPTS = GAME / 'scripts'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def variant(n):
    return f'{n:064x}-{n:012x}_b'


def render_key(vs, fs):
    name = lambda v: v.encode().ljust(80, b'\0')
    return (name(vs) + name(fs) + bytes(544 - 160)).hex()


class QuickList(unittest.TestCase):
    """shader_list.py: the shaders of the title rows, and the list cut to some shaders."""

    def setUp(self):
        sys.path.insert(0, str(SCRIPTS))
        self.shader_list = load('oot_shader_list_quick', SCRIPTS / 'shader_list.py')
        self.lst = self.shader_list.ShaderList()
        self.lst.add_warm(('R', render_key(variant(1), variant(2))), 't')
        self.lst.add_warm(('R', render_key(variant(3), variant(4))), 'm')
        self.lst.add_warm(('C', variant(5)), 'tm')
        self.lst.add_warm(('R', render_key(variant(1), variant(4))), 'm')   # one of its shaders is not a title one

    def tearDown(self):
        sys.path.remove(str(SCRIPTS))

    def test_the_title_rows_name_the_quick_build_s_shaders(self):
        self.assertEqual(self.lst.title_keys(), {variant(n)[:64] for n in (1, 2, 5)})

    def test_a_list_without_title_rows_names_none(self):
        lst = self.shader_list.ShaderList()
        lst.add_warm(('R', render_key(variant(3), variant(4))), 'm')
        self.assertEqual(lst.title_keys(), set())

    def test_the_cut_keeps_only_rows_whose_every_shader_is_kept(self):
        keys = self.lst.title_keys()
        self.lst.add_variant(variant(1)[:64], '{"attr":{},"fout":{},"img":{},"tex":{}}')
        self.lst.add_variant(variant(3)[:64], '{"attr":{},"fout":{},"img":{},"tex":{}}')
        part = self.lst.only(keys)
        self.assertEqual(set(part.warm), {('R', render_key(variant(1), variant(2))), ('C', variant(5))})
        self.assertEqual({key for key, _ in part.variants}, {variant(1)[:64]})
        self.assertEqual(part.warm[('C', variant(5))], 'tm')   # the flags stay as listed
        self.assertEqual(len(self.lst.warm), 4)   # the list itself is not changed


class QuickCorpus(unittest.TestCase):
    """prepare_ns2_seed.py: the corpus cut to some shaders, in the corpus's own form."""

    def setUp(self):
        sys.path.insert(0, str(SCRIPTS))
        self.seed = load('oot_ns2_seed_quick', SCRIPTS / 'prepare_ns2_seed.py')
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()
        sys.path.remove(str(SCRIPTS))

    def corpus(self, n):
        records, keys = [], []
        for i in range(n):
            control, code, label = bytes([i]) * 0x48, bytes([i + 100]) * 32, f'shader{i}'.encode()
            records.append(struct.pack('<IIIH', i % 6, len(control), len(code), len(label)) + label + control + code)
            keys.append(self.seed.shader_key(control, code))
        path = self.dir / 'corpus.bin'
        path.write_bytes(self.seed.CORPUS_MAGIC + struct.pack('<I', n) + b''.join(records))
        return path, keys

    def test_the_cut_holds_the_asked_shaders_and_reads_as_a_corpus(self):
        path, keys = self.corpus(6)
        out = self.dir / 'only.bin'
        self.assertEqual(self.seed.corpus_subset(path, {keys[1], keys[4], '0' * 64}, out), 2)
        self.assertEqual(set(self.seed.corpus_inventory(out)), {keys[1], keys[4]})
        self.assertEqual(len(self.seed.corpus_inventory(path)), 6)   # the full corpus is left as it is

    def test_a_cut_with_none_of_the_corpus_shaders_stops(self):
        path, _ = self.corpus(3)
        with self.assertRaises(ValueError):
            self.seed.corpus_subset(path, {'0' * 64}, self.dir / 'only.bin')

    def test_the_only_file_holds_shader_keys(self):
        good = self.dir / 'good.txt'
        good.write_text('a' * 64 + '\n' + 'b' * 64 + '\n\n')
        self.assertEqual(self.seed.only_keys(good), {'a' * 64, 'b' * 64})
        for text in ('', 'xyz\n', 'a' * 63 + '\n'):
            bad = self.dir / 'bad.txt'
            bad.write_text(text)
            with self.assertRaises(ValueError, msg=repr(text)):
                self.seed.only_keys(bad)

    def test_a_quick_seed_s_scope_is_every_shader_of_the_game(self):
        # totk_rt_note_set writes a shader outside the scope into the player's contribution file as new: a quick seed's
        # scope of its few shaders would have sent thousands the list holds
        text = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertIn("shader_key_scope(sorted(corpus_inventory(full_corpus)) if only else keys", text)


class ShaderMode(unittest.TestCase):
    """games/oot/build.py shader_mode: what a build takes from OOT_SHADERS, what an update takes from the App."""

    def setUp(self):
        self.build = load('oot_build_quick', GAME / 'build.py')
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def app(self, shaders=None):
        app = Path(self.tmp.name) / 'Ocarina of Time.app'
        (app / 'Contents').mkdir(parents=True, exist_ok=True)
        info = {'CFBundleExecutable': 'OcarinaOfTime'}
        if shaders:
            info['MNGShaders'] = shaders
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        return app

    def mode(self, env, app=None):
        clean = {k: v for k, v in os.environ.items() if k not in ('OOT_SHADERS', 'OOT_BUNDLE_SHADERS')}
        with mock.patch.dict(os.environ, {**clean, **env}, clear=True):
            return self.build.shader_mode(app)

    def test_a_build_carries_every_shader_unless_told(self):
        self.assertEqual(self.mode({}), 'full')
        self.assertEqual(self.mode({'OOT_SHADERS': 'title'}), 'title')
        self.assertEqual(self.mode({'OOT_BUNDLE_SHADERS': '0'}), 'none')   # the older test build's switch

    def test_an_update_keeps_what_the_app_records(self):
        self.assertEqual(self.mode({}, self.app('title')), 'title')
        self.assertEqual(self.mode({}, self.app()), 'full')   # an App from before 2026-11-13 records nothing
        self.assertEqual(self.mode({'OOT_SHADERS': 'full'}, self.app('title')), 'full')

    def test_an_unknown_mode_stops(self):
        with self.assertRaises(ValueError):
            self.mode({'OOT_SHADERS': 'some'})

    def test_a_list_without_title_rows_stops_a_quick_build(self):
        shared = Path(self.tmp.name) / 'shader_list.tsv'
        shared.write_text('# list\n')
        with self.assertRaises(ValueError) as e:
            self.build.title_shader_file(Path(self.tmp.name) / 'tree', shared)
        self.assertIn('no title rows', str(e.exception))

    def test_the_app_records_its_mode_and_shader_fill_makes_it_full(self):
        text = (GAME / 'build.py').read_text()
        self.assertIn("info['MNGShaders']=mode", text)
        self.assertIn("package_app(work,tree,app,'library' if folder else 'app',folder,shared=listfile,mode='full')", text)


class CaptureSwitch(unittest.TestCase):
    def test_the_capture_records_before_the_lists_leave_rows_out(self):
        text = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        body = text[text.index('static void pso_user_record(const char *fmt, ...) {'):]
        body = body[:body.index('\n}\n')]
        self.assertLess(body.index('pso_capture(line);'), body.index('if (!f && !g_pso_shared) return;'))
        self.assertLess(body.index('pso_capture(line);'), body.index('if ([g_pso_user_seen containsObject:k]) return;'))
        self.assertIn('getenv("TOTK_PSO_CAPTURE")', text)


if __name__ == '__main__':
    unittest.main()

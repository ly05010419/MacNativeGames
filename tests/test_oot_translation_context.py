"""Ocarina of Time's translation cache belongs only to the code that decides a translation (games/oot/scripts/prepare_ns2_seed.py
request_sources; user 2026-11-10): a change elsewhere in totk_shader_rt.m or nvn_mtl.m keeps the translations, a change to
a request function makes them again."""
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/oot/third_party/d2r_gfx/gfx/nvn'


def load_seed():
    spec = importlib.util.spec_from_file_location('oot_prepare_ns2_seed', ROOT / 'games/oot/scripts/prepare_ns2_seed.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RequestSources(unittest.TestCase):
    def setUp(self):
        self.seed = load_seed()
        self.tmp = tempfile.TemporaryDirectory()
        self.nvn = Path(self.tmp.name)
        for name in ('totk_shader_rt.m', 'nvn_mtl.m', 'nvn_ctl.h'):
            shutil.copy2(NVN / name, self.nvn / name)
        self.base = self.seed.request_sources(self.nvn)

    def tearDown(self):
        self.tmp.cleanup()

    def edit(self, name, old, new):
        path = self.nvn / name
        text = path.read_text(errors='replace')
        self.assertEqual(text.count(old), 1, old)
        path.write_text(text.replace(old, new))

    def test_the_checked_out_files_give_the_same(self):
        self.assertEqual(self.base, self.seed.request_sources())

    def test_other_code_keeps_the_translations(self):
        # the cache folder's setup (main 0d4e68ec released its import temporaries there) and a function no request reaches
        self.edit('totk_shader_rt.m', 'NSString *totk_rt_dir(void) {', 'NSString *totk_rt_dir(void) {   // changed')
        self.edit('totk_shader_rt.m', 'static int rt_bg_workers(void) {', 'static int rt_bg_workers(void) {   // changed')
        self.edit('nvn_mtl.m', 'static inline int registry_lock(void) {', 'static inline int registry_lock(void) {   // changed')
        self.assertEqual(self.seed.request_sources(self.nvn), self.base)

    def test_each_request_function_counts(self):
        for name, heads in self.seed.REQUEST_FUNCTIONS.items():
            for head in heads:
                with self.subTest(head=head):
                    text = (self.nvn / name).read_text(errors='replace')
                    body = self.seed.function_text(text, head, name)
                    last = body.rstrip().splitlines()[-1]
                    self.edit(name, last, last + '   // changed')
                    self.assertNotEqual(self.seed.request_sources(self.nvn), self.base)
                    self.edit(name, last + '   // changed', last)
        path = self.nvn / 'nvn_ctl.h'
        path.write_bytes(path.read_bytes() + b'\n// changed\n')
        self.assertNotEqual(self.seed.request_sources(self.nvn), self.base)

    def test_a_missing_function_is_an_error(self):
        self.edit('totk_shader_rt.m', 'static NSMutableDictionary *variant_request(', 'static NSMutableDictionary *variant_request_renamed(')
        with self.assertRaises(ValueError):
            self.seed.request_sources(self.nvn)


if __name__ == '__main__':
    unittest.main()

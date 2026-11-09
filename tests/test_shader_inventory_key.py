"""The shader inventory (prepare_shader_seed.py cached_inventory) is keyed by the code that reads it, not by the whole
script: a comment or an unrelated change there no longer reads the whole RomFS again (49 s), a change to that code does.
The keys are taken from prepare() itself, on copies of the script loaded as modules of their own."""
import ast
import contextlib
import io
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

import test_shader_seed as base

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/prepare_shader_seed.py'
SOURCE = SCRIPT.read_text()
# Module-level names inventory_sha256 hashes by value rather than by source, each with a change to its definition.
CONSTANTS = {'ZSTD': ("ZSTD = b'\\x28\\xb5\\x2f\\xfd'", "ZSTD = b'\\x28\\xb5\\x2f\\xfe'"),
             'YAZ0': ("YAZ0 = b'Yaz0'", "YAZ0 = b'Yaz1'"),
             'GAMES': ("folders=('Pack/', 'Effect/', 'Event/', 'System/')", "folders=('Pack/', 'Effect/', 'Event/', 'System/', 'Model/')")}


class Stop(Exception):
    """Raised when prepare() starts the translations, once the inventory is written."""


class InventoryKey(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.copies = self.keys = 0
        path = list(sys.path)
        self.addCleanup(sys.path.__setitem__, slice(None), path)   # each copy puts its folder first in sys.path

    def load(self, text):
        """The script's text as a module of its own, from a file of its own (inspect.getsource reads that file)."""
        self.copies += 1
        folder = self.root/f'copy{self.copies}/games/totk/scripts'
        folder.mkdir(parents=True)
        (folder/'prepare_shader_seed.py').write_text(text)
        sys.path.insert(0, str(SCRIPT.parent))   # shader_list, which the copy imports
        spec = importlib.util.spec_from_file_location(f'inventory_key_copy{self.copies}', folder/'prepare_shader_seed.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def key(self, module):
        """The identity prepare() writes into a new inventory (seed.inventory/inventory.json): a synthetic Breath of the
        Wild RomFS (no Zstandard dictionaries) holding one BNSH file in a startup package. Nothing named in INVENTORY_CODE
        is replaced; prepare() stops when the translations start."""
        self.keys += 1
        work = self.root/f'work{self.keys}'
        romfs = work/'romfs'
        romfs.mkdir(parents=True)
        data = bytes(base.bnsh())
        (romfs/'romfs.bin').write_bytes(data)
        (romfs/'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256='c'*64, files=[
            dict(path='Pack/Bootup.pack', offset=0, size=len(data))])))
        for name in ('translator', 'spirv-cross', 'list.tsv'):
            (work/name).write_text('')
        try:
            with mock.patch.object(module, 'translations_folder', side_effect=Stop), \
                    contextlib.redirect_stdout(io.StringIO()):
                module.prepare(romfs, work/'seed', work/'translator', work/'spirv-cross', '1.6.0', work/'list.tsv',
                               game='botw')
        except Stop:
            pass
        return json.loads((work/'seed.inventory'/'inventory.json').read_text())['identity']

    @staticmethod
    def mutated(text, name):
        """text with a statement added at the start of function name's body (before its docstring)."""
        node = next(n for n in ast.parse(text).body if isinstance(n, ast.FunctionDef) and n.name == name)
        lines = text.splitlines(keepends=True)
        first = node.body[0].lineno - 1
        indent = re.match(r'\s*', lines[first]).group()
        return ''.join(lines[:first] + [f'{indent}_mutated = None\n'] + lines[first:])

    def test_a_comment_or_an_unrelated_function_keeps_the_key(self):
        original = self.key(self.load(SOURCE))
        text = SOURCE.replace('\ndef main():', '\n# a comment-only edit\ndef main():', 1)
        self.assertNotEqual(text, SOURCE)
        self.assertEqual(self.key(self.load(text)), original)
        # what the inventory does not read of GAMES; each copy is in another folder, so its list paths differ too
        text = SOURCE.replace('min_shaders=2000', 'min_shaders=1999', 1)
        self.assertNotEqual(text, SOURCE)
        self.assertEqual(self.key(self.load(text)), original)
        for name in ('plan', 'located', 'translation_request'):   # code the inventory does not run
            self.assertEqual(self.key(self.load(self.mutated(SOURCE, name))), original, name)

    def test_the_key_changes_with_each_function_it_names(self):
        module = self.load(SOURCE)
        original = self.key(module)
        self.assertEqual(len(module.INVENTORY_CODE), len(set(module.INVENTORY_CODE)))
        for name in module.INVENTORY_CODE:
            changed = self.load(self.mutated(SOURCE, name))
            self.assertNotEqual(self.key(changed), original, name)
        # control: the copies load and key alike when nothing changes
        self.assertEqual(self.key(self.load(SOURCE)), original)

    def test_the_key_changes_with_each_constant_the_inventory_reads(self):
        original = self.key(self.load(SOURCE))
        for name, (old, new) in CONSTANTS.items():
            self.assertEqual(SOURCE.count(old), 1, name)
            self.assertNotEqual(self.key(self.load(SOURCE.replace(old, new))), original, name)

    def test_the_named_functions_are_what_the_inventory_reaches_in_the_script(self):
        tree = ast.parse(SOURCE)
        functions = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
        defined = set(functions) | {n.name for n in tree.body if isinstance(n, ast.ClassDef)}
        defined |= {t.id for n in tree.body if isinstance(n, ast.Assign) for t in n.targets if isinstance(t, ast.Name)}
        local_imports = {a.asname or a.name for n in tree.body if isinstance(n, ast.ImportFrom)
                         and n.module.split('.')[0] not in sys.stdlib_module_names for a in n.names}   # shader_list
        self.assertTrue(local_imports)
        reached, constants, pending = set(), set(), ['cached_inventory']
        while pending:
            name = pending.pop()
            if name in reached:
                continue
            reached.add(name)
            for node in ast.walk(functions[name]):
                if isinstance(node, ast.Name):
                    self.assertNotIn(node.id, local_imports, f'{name} uses {node.id}, from another module of the repo')
                    if node.id in functions:
                        pending.append(node.id)
                    elif node.id in defined:
                        constants.add(node.id)
                if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Subscript) and \
                        isinstance(node.value.value, ast.Name) and node.value.value.id == 'GAMES':
                    self.assertEqual(ast.literal_eval(node.slice), 'folders', name)   # only the folders are hashed
        self.assertEqual(reached, set(self.load(SOURCE).INVENTORY_CODE))
        self.assertEqual(constants, set(CONSTANTS))
        uses = [n for f in reached for n in ast.walk(functions[f]) if isinstance(n, ast.Name) and n.id == 'GAMES']
        subscripted = [n for f in reached for n in ast.walk(functions[f])
                       if isinstance(n, ast.Subscript) and isinstance(n.value, ast.Subscript)
                       and isinstance(n.value.value, ast.Name) and n.value.value.id == 'GAMES']
        self.assertEqual(len(uses), len(subscripted))   # every use of GAMES is GAMES[game]['folders']


class InventoryCacheRoundTrip(unittest.TestCase):
    """prepare() reads the RomFS again exactly when INVENTORY_SHA256 changes (test_shader_seed's synthetic service)."""
    def test_the_inventory_is_reused_with_the_same_key_and_read_again_with_another(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root, calls = Path(tmp.name), []
        rows = list(base.seed.bnsh_shaders(base.bnsh()))
        def listed(slot):   # a new listed row each time: not the whole-seed reuse
            return [(rows[0][0], json.dumps({'tex': {slot: 'cube'}}, sort_keys=True, separators=(',', ':')))]
        prepare = base.ShaderKeyScope.prepare_with_service
        prepare(None, root, root/'seed', inventory_calls=calls)
        saved = json.loads((root/'seed.inventory'/'inventory.json').read_text())['identity']
        self.assertEqual(saved['inventory_sha256'], base.seed.INVENTORY_SHA256)
        self.assertEqual(base.seed.INVENTORY_SHA256, base.seed.inventory_sha256())
        self.assertNotIn('preparation_sha256', saved)
        prepare(None, root, root/'seed', listed('8'), inventory_calls=calls)
        with mock.patch.object(base.seed, 'INVENTORY_SHA256', '0'*64):
            prepare(None, root, root/'seed', listed('9'), inventory_calls=calls)
        self.assertEqual(calls, [1, 0, 1])


if __name__ == '__main__':
    unittest.main()

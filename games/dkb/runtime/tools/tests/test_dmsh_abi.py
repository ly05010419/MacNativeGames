"""The shader library ABI (dm/gfx/nvn/dmsh_abi.h): the one definition of the NVN driver segment layout and the binding kinds that the shader build (dmsh_build.py, through
dm/gfx/tools/dmsh_abi.py) and the host (nvn_mtl.m, nvn_refl.m) share. The header parses, its tables fit together, the build takes its values from it, the host handles every kind
it names, the translator's own copy of the c2 layout (Program.cs) agrees, and a ratchet keeps the numbers and kind names from coming back as literals in the files that use them."""
import ast
import collections
import io
from pathlib import Path
import re
import sys
import tokenize
from typing import Any
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_abi as abi   # noqa: E402

# Literals of the ABI still written in each file (outside comments), counted as the ratchet below does: the value, or the kind string (repr). These stay for other reasons:
# a json key spelled like a kind ('c1', 'geometry', 'tex', 'img' in the facts and reflection json), an NVN stage name ("geometry"), a "?" placeholder in a log line, and 0x100
# where it is not the push constant block's size (DMSH_PUSH_SIZE): a viewport swizzle value in nvn_mtl.m, the start of compute code in a package record in dmsh_build.py,
# a single texture bind record's flag bit in nvn_obj.h (BIND_TEX1_NOBASE).
# 0x1000 where it is not the pool table's entry count (DMSH_GMEM_MAP_ENTRIES): the 4 KB page step of nvn_mtl.m's sparse residency map.
# A count may only go down; when it does, lower it here.
BASELINE = {
    'dm/gfx/nvn/nvn_mtl.m': {"'?'": 2, "'geometry'": 3, '0x100': 2, '0x1000': 2},
    'dm/gfx/nvn/nvn_refl.m': {"'c1'": 1},
    'dm/gfx/nvn/nvn_refl.h': {},
    'dm/gfx/nvn/nvn_obj.h': {'0x100': 1},
    'dm/gfx/tools/dmsh_build.py': {"'c1'": 4, "'geometry'": 3, "'img'": 1, "'tex'": 1, '0x100': 2},
    'dm/gfx/tools/dmsh_fetch.py': {},
    'dm/gfx/tools/cb_extent.py': {},
}
NUMBERS = {v for v in abi.VALUES.values() if isinstance(v, int) and v >= 0x100}   # the offsets and sizes distinctive enough to find as literals (not 8, 16, 0x20 or 128)
C_TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/|\b0[xX][0-9A-Fa-f]+\b', re.S)


def c_literals(text):   # (kind, value) of the string literals and hex numbers of C / Objective-C source outside comments
    for m in C_TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith(('//', '/*', "'")): continue
        yield ('str', t[1:-1]) if t.startswith('"') else ('int', int(t, 16))


def py_literals(text):   # the same for Python source
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type == tokenize.NUMBER and tok.string.lower().startswith('0x'): yield ('int', int(tok.string, 16))
        elif tok.type == tokenize.STRING:
            try: v = ast.literal_eval(tok.string)
            except (ValueError, SyntaxError): continue
            if isinstance(v, str): yield ('str', v)


def abi_literals(path):
    text = (REPO / path).read_text()
    lits = py_literals(text) if path.endswith('.py') else c_literals(text)
    return collections.Counter(f'{v:#x}' if k == 'int' else repr(v) for k, v in lits if (k == 'int' and v in NUMBERS) or (k == 'str' and v in abi.KINDS))


class Header(unittest.TestCase):
    def test_parses(self):
        self.assertEqual(abi.load(abi.HEADER), abi.VALUES)
        for name in ('C2_TEX', 'C2_SAMP', 'C0_SSBO_PS', 'DRV_SIZE', 'KIND_CBUF', 'KIND_UNKNOWN'): self.assertIn(name, abi.VALUES)
        kinds = [v for k, v in abi.VALUES.items() if k.startswith('KIND_')]
        self.assertEqual(len(kinds), len(set(kinds)), 'two binding kinds with the same name')

    def test_reader_is_strict(self):
        good = '// comment\n#pragma once\n\n#define DMSH_A 0x20   // a\n#define DMSH_B "b"\n#define DMSH_C 0\n'
        self.assertEqual(abi.parse(good), {'A': 0x20, 'B': 'b', 'C': 0})
        for bad in ('#define DMSH_A (0x20 + 8)', '#define DMSH_A 010', '#define A 1', 'enum { DMSH_A = 1 };', '#define DMSH_A 1 /* a */', '#define DMSH_A 1\n#define DMSH_A 2', '// only a comment'):
            with self.assertRaises(ValueError, msg=bad): abi.parse(bad)

    def test_tables_fit(self):   # the same as nvn_mtl.m's _Static_asserts, and what dmsh_build.py's slot decoding relies on
        a = abi
        self.assertEqual(a.C2_TEX + a.C2_ROW * a.C2_TEX_SLOTS, a.C2_SEPTEX)
        self.assertEqual(a.C2_TEX + a.C2_ROW * 32, a.C2_IMG, 'combined slot 32 shares its word with image slot 0 (nvn_mtl.m tex_slot_h)')
        self.assertLessEqual(a.C2_IMG + a.C2_ROW * a.C2_IMG_SLOTS, a.C2_SEPTEX)
        self.assertEqual(a.C2_SEPTEX + a.C2_ROW * a.C2_SEP_SLOTS, a.C2_SAMP)
        self.assertLessEqual(a.C2_SAMP + a.C2_ROW * a.C2_SEP_SLOTS, a.DRV_SIZE)
        self.assertEqual(a.SEPTEX_SLOT0 + a.C2_SEP_SLOTS, 256, 'the host has 256 texture slots a stage (nvn_mtl.m NTEX)')
        self.assertEqual(a.C0_CS_UBO + a.C0_ROW * a.C0_CS_UBO_SLOTS, a.C0_SSBO_CS)
        self.assertLessEqual(a.C0_DISPATCH + 12, a.C0_CS_UBO)
        bases = sorted((a.C0_SSBO_VS, a.C0_SSBO_CS, a.C0_SSBO_GS, a.C0_SSBO_PS))
        for lo, hi in zip(bases, bases[1:]): self.assertLessEqual(lo + a.C0_ROW * a.C0_SSBO_SLOTS, hi, 'storage buffer tables overlap')
        self.assertLessEqual(bases[-1] + a.C0_ROW * a.C0_SSBO_SLOTS, a.C0_POINT_SIZE)
        self.assertLessEqual(a.C0_BASE_INSTANCE + 4, a.DRV_SIZE)


class Build(unittest.TestCase):
    def test_values_come_from_the_header(self):
        # dmsh_build.py is a command-line program whose top level loads a game profile: run only its layout assignments, with the reader
        tree = ast.parse((REPO / 'dm/gfx/tools/dmsh_build.py').read_text())
        names = {'TEX_BASE', 'SSBO', 'DRV_FILL'}
        body: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.Assign) and any(getattr(e, 'id', None) in names for t in n.targets for e in (t.elts if isinstance(t, ast.Tuple) else [t]))]
        ns: dict[str, Any] = {'abi': abi}
        exec(compile(ast.Module(body=body, type_ignores=[]), 'dmsh_build.py', 'exec'), ns)
        self.assertEqual((ns['TEX_BASE'], ns['IMG_BASE'], ns['IMG_SLOTS'], ns['SEPTEX_BASE'], ns['SEPTEX_SLOT0'], ns['SAMP_BASE']),
                         (abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP))
        self.assertEqual(ns['SSBO'], {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'gs': abi.C0_SSBO_GS, 'ps': abi.C0_SSBO_PS})
        self.assertEqual(ns['DRV_FILL'][2][-1], (abi.C2_SAMP, abi.C2_SAMP + abi.C2_ROW * abi.C2_SEP_SLOTS))

    def test_every_kind_is_written_by_the_build(self):
        src = (REPO / 'dm/gfx/tools/dmsh_build.py').read_text()
        self.assertRegex(src, r'(?m)^import .*\bdmsh_abi as abi\b')
        unused = [k for k in abi.VALUES if k.startswith('KIND_') and f'abi.{k}' not in src]
        self.assertEqual(unused, [], 'binding kinds in dmsh_abi.h that dmsh_build.py never writes')

    def test_chain_covers_the_abi(self):   # the header and its reader decide the build output: they are part of the chain version
        sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
        import dmsh_chain
        self.assertIn('nvn/dmsh_abi.h', dmsh_chain.FILES)
        self.assertIn('tools/dmsh_abi.py', dmsh_chain.FILES)


class Host(unittest.TestCase):
    def test_host_includes_the_header(self):
        self.assertIn('#include "dmsh_abi.h"', (REPO / 'dm/gfx/nvn/nvn_mtl.m').read_text())
        self.assertIn('#include "dmsh_abi.h"', (REPO / 'dm/gfx/nvn/nvn_refl.m').read_text())
        self.assertRegex((REPO / 'dm/gfx/nvn/nvn_obj.h').read_text(), r'(?m)^#define NVN_SEPTEX_SLOT0 DMSH_SEPTEX_SLOT0$')

    def test_host_reads_every_kind(self):   # a kind the build may write that the host does not know fails the shader at load time
        src = (REPO / 'dm/gfx/nvn/nvn_refl.m').read_text()
        missing = [k for k in abi.VALUES if k.startswith('KIND_') and k != 'KIND_UNKNOWN' and f'@DMSH_{k}]' not in src]
        self.assertEqual(missing, [], 'binding kinds nvn_refl.m does not convert')

    def test_translator_agrees(self):   # Program.cs keeps its own copy of the c2 layout (a C# file cannot include the header)
        m = re.search(r'const int TexBase = (\w+), ImgBase = (\w+), ImgSlots = (\w+), SepTexBase = (\w+), SepTexSlot0 = (\w+), SampBase = (\w+);',
                      (REPO / 'dm/gfx/tools/dmsh_xlate/Program.cs').read_text())
        assert m, 'Program.cs c2 layout line not found'
        self.assertEqual([int(x, 0) for x in m.groups()], [abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP])
        m = re.search(r'const int C2HandleWords = \(SampBase \+ 8 \* (\d+)\) / 8;', (REPO / 'dm/gfx/tools/dmsh_xlate/Program.cs').read_text())
        assert m, 'Program.cs C2HandleWords line not found'
        self.assertEqual(int(m.group(1)), abi.C2_SEP_SLOTS)
        self.assertEqual(abi.C2_ROW, 8)


class Ratchet(unittest.TestCase):
    def test_no_new_abi_literals(self):
        worse = {}
        for path, base in BASELINE.items():
            now = abi_literals(path)
            more = {lit: f'{base.get(lit, 0)} -> {n}' for lit, n in now.items() if n > base.get(lit, 0)}
            if more: worse[path] = more
        self.assertEqual(worse, {}, 'ABI values written as literals: use the DMSH_* names of dm/gfx/nvn/dmsh_abi.h (dmsh_abi.<NAME> in Python)')


if __name__ == '__main__':
    unittest.main()

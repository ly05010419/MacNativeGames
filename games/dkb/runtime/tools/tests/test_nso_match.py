"""Function patch recognition through real AArch64 code and synthetic unwind tables.
No game files. Relocation is accepted; changed logic, callees, ambiguity and wrong
profile/module selection are refused through the rewriter's actual registry code.
A patch of several functions applies whole or not at all; the rewriter lists what an
unregistered build kept, and the NSP script reports it.
"""
import ast
import contextlib
import copy
import glob
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, B, NOP
from nso_match import Image, MatchError, resolve, validate

MOV = 0xaa1703e0  # mov x0, x23
ADD = 0x914012e0  # add x0, x23, #0x4000
RET = 0xd65f03c0


def image(functions):
    """A MOD0 and .eh_frame_hdr describing the supplied functions."""
    out = bytearray(0x4000)
    struct.pack_into('<I', out, 4, 0x20)
    out[0x20:0x24] = b'MOD0'
    struct.pack_into('<ii', out, 0x30, 0x3000 - 0x20, 0x300c + len(functions) * 8 - 0x20)
    out[0x3000:0x3004] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', out, 0x3004, 0x3200 - 0x3004, len(functions))
    # A zR CIE with pcrel/sdata4 PCs and four-byte lengths.
    struct.pack_into('<II', out, 0x3200, 16, 0)
    out[0x3208:0x3211] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, words) in enumerate(sorted(functions.items())):
        struct.pack_into('<' + 'I' * len(words), out, start, *words)
        fde = 0x3240 + i * 24
        struct.pack_into('<ii', out, 0x300c + i * 8, start - 0x3000, fde - 0x3000)
        struct.pack_into('<IIiI', out, fde, 16, fde + 4 - 0x3200, start - (fde + 8), len(words) * 4)
    return out


def caller(start, target, data=0x2200):
    return [ADRP(8, start, data), ADDI(8, 8, data & 0xfff), MOV,
            B(start + 12, target) | 0x80000000, 0x54000040, NOP, RET]  # bl; b.eq +8


def imported(name, plt=0x1800, slot=0x2500):
    out = image({0x100: caller(0x100, plt)})
    struct.pack_into('<i', out, 0x24, 0x2100 - 0x20)
    for i, (tag, value) in enumerate(((20, 7), (23, 0x2200), (2, 24), (6, 0x2300), (5, 0x2400), (0, 0))):
        struct.pack_into('<qQ', out, 0x2100 + i * 16, tag, value)
    struct.pack_into('<QQq', out, 0x2200, slot, (1 << 32) | 1026, 0)
    struct.pack_into('<I', out, 0x2318, 1)
    data = b'\0' + name.encode() + b'\0'; out[0x2400:0x2400 + len(data)] = data
    lo = slot & 0xfff
    struct.pack_into('<4I', out, plt, ADRP(16, plt, slot), 0xf9400211 | (lo // 8) << 10, ADDI(16, 16, lo), 0xd61f0220)
    return out


class Matching(unittest.TestCase):
    def setUp(self):
        self.original = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET]})
        self.rule = Image(self.original, 0x2000).recipe(0x108, 'e00317aa', 'e0124091', 'heap')

    def test_relocated_function_callee_and_data_addresses(self):
        new = image({0x800: caller(0x800, 0x1200, 0x2980), 0x1200: [0xd2800020, RET]})
        before = bytes(new)
        cuts = resolve(new, 0x2000, [self.rule], lambda _: None)
        self.assertEqual(cuts[0][:3], (0x808, 'e00317aa', 'e0124091'))
        self.assertEqual(bytes(new), before)  # selection never writes partially verified patches

    def test_changed_logic_and_callee_are_not_matches(self):
        for off, word in ((0x108, 0xaa1603e0), (0x110, 0x54000020), (0x500, 0xd2800040)):
            with self.subTest(off=hex(off)):
                changed = bytearray(self.original); struct.pack_into('<I', changed, off, word)
                with self.assertRaisesRegex(MatchError, '0 matching'):
                    Image(changed, 0x2000).match(self.rule)

    def test_call_redirected_to_different_function(self):
        changed = image({0x100: caller(0x100, 0x600), 0x500: [0xd2800020, RET], 0x600: [0xd2800040, RET]})
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(changed, 0x2000).match(self.rule)

    def test_import_identity_survives_relocation_but_not_redirection(self):
        old = Image(imported('allocate'), 0x2000)
        rule = old.recipe(0x108, 'e00317aa', 'e0124091', 'heap')
        moved = Image(imported('allocate', plt=0x1900, slot=0x2800), 0x2000)
        self.assertEqual(moved.match(rule)[0][0], 0x108)
        with self.assertRaisesRegex(MatchError, '0 matching'):
            Image(imported('free'), 0x2000).match(rule)

    def test_system_call_wrapper_is_identified_by_its_number(self):
        # the SDK's svc wrappers have no unwind entry: `svc #n; ret` stands for call n, and another number is another callee
        def module(svc, at=0x500):
            out = image({0x100: caller(0x100, at)})
            struct.pack_into('<2I', out, at, 0xd4000001 | svc << 5, RET)
            return out
        rule = Image(module(0x2c), 0x2000).recipe(0x108, 'e00317aa', 'e0124091', 'heap')
        self.assertEqual(Image(module(0x2c, 0x700), 0x2000).match(rule)[0][0], 0x108)
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(module(0x2d), 0x2000).match(rule)
        longer = module(0x2c); struct.pack_into('<I', longer, 0x504, NOP)   # not a bare wrapper: no identity, no match
        with self.assertRaisesRegex(MatchError, 'no verified boundary'): Image(longer, 0x2000).fingerprint(0x100)

    def test_address_register_clobbers_preserve_member_offsets(self):
        for reg, move in ((8, 0x2a0003e8), (29, 0xaa0003fd)):
            words = [ADRP(reg, 0x100, 0x2000), move, 0xf9400000 | (reg << 5) | (8 << 10), MOV, RET]
            old = image({0x100: words})
            rule = Image(old, 0x2000).recipe(0x10c, 'e00317aa', 'e0124091', 'heap')
            words[2] += 1 << 10  # changed field offset after the address was overwritten
            with self.assertRaisesRegex(MatchError, '0 matching'):
                Image(image({0x100: words}), 0x2000).match(rule)

    def test_internal_adr_target_is_not_normalized(self):
        old = image({0x100: [MOV, 0x10000040, NOP, RET]})  # adr x0, function+12
        rule = Image(old, 0x2000).recipe(0x100, 'e00317aa', 'e0124091', 'heap')
        new = image({0x100: [MOV, 0x10000020, NOP, RET]})  # adr x0, function+8
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(new, 0x2000).match(rule)

    def test_ambiguous_function_and_missing_callee_boundary(self):
        duplicate = image({0x100: caller(0x100, 0x500), 0x800: caller(0x800, 0x500), 0x500: [0xd2800020, RET]})
        with self.assertRaisesRegex(MatchError, '2 matching'): Image(duplicate, 0x2000).match(self.rule)
        missing = image({0x100: caller(0x100, 0x500)})
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(missing, 0x2000).match(self.rule)

    def test_original_bytes_still_required(self):
        rule = copy.deepcopy(self.rule); rule['cuts'][0] = (8, '00000000', 'e0124091', 'heap')
        with self.assertRaisesRegex(MatchError, 'original bytes differ'): Image(self.original, 0x2000).match(rule)

    def test_relocations_in_replacement_are_rejected(self):
        for w in (B(0, 4), ADRP(0, 0, 0x1000), 0x58000020):
            rule = copy.deepcopy(self.rule); rule['cuts'][0] = (8, 'e00317aa', struct.pack('<I', w).hex(), 'heap')
            with self.assertRaisesRegex(ValueError, 'PC-relative'): validate(rule)

    def test_no_boundaries_and_ambiguous_match_are_reported(self):
        for img in (bytearray(0x40), image({0x100: [RET]})):
            messages = []
            self.assertEqual(resolve(img, len(img), [self.rule], messages.append), [])
            self.assertTrue(messages[0].startswith('[RED] '))

    def test_overlapping_rules_are_rejected(self):
        other = dict(self.rule, name='another patch')
        with self.assertRaisesRegex(MatchError, 'overlap'):
            resolve(self.original, 0x2000, [self.rule, other], lambda _: None)

    def test_same_name_rules_are_alternative_implementations(self):
        # A later build changed the function (another size, the cut elsewhere in it): one rule per reviewed implementation, under one name
        def changed(start):  # caller() with one more instruction before the cut
            return [ADRP(8, start, 0x2200), ADDI(8, 8, 0x200), NOP, MOV, B(start + 16, 0x500) | 0x80000000, 0x54000040, NOP, RET]
        later = image({0x100: changed(0x100), 0x500: [0xd2800020, RET]})
        alt = Image(later, 0x2000).recipe(0x10c, 'e00317aa', 'e0124091', 'heap')
        self.assertNotEqual(alt['size'], self.rule['size'])
        for img, off in ((self.original, 0x108), (later, 0x10c)):
            with self.subTest(off=hex(off)):
                messages = []
                cuts = resolve(img, 0x2000, [self.rule, alt], messages.append)
                self.assertEqual([c[0] for c in cuts], [off])
                self.assertFalse([m for m in messages if m.startswith('[RED]')], messages)
        messages = []
        self.assertEqual(resolve(image({0x100: [RET]}), 0x2000, [self.rule, alt], messages.append), [])
        self.assertEqual(len(messages), 1); self.assertTrue(messages[0].startswith('[RED] '))
        both = image({0x100: caller(0x100, 0x500), 0x800: changed(0x800), 0x500: [0xd2800020, RET]})
        messages = []
        self.assertEqual(resolve(both, 0x2000, [self.rule, alt], messages.append), [])
        self.assertIn('2 implementations match', messages[0])

    def test_registry_scopes_rules_and_preserves_known_builds(self):
        source = TOOLS / 'patch_nso.py'; tree = ast.parse(source.read_text())
        start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'PATCH_FILES')
        end = next(i for i, n in enumerate(tree.body[start:], start) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'KEYS')
        code = compile(ast.Module(body=tree.body[start:end], type_ignores=[]), str(source), 'exec')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'tools').mkdir(); (root / 'tools/sdk_patches.py').write_text('')
            for gid in ('0100000000000001', '0100000000000002'):
                p = root / 'games' / gid / 'patches.py'; p.parent.mkdir(parents=True)
                rule = copy.deepcopy(self.rule)
                replacement = 'e0124091' if gid.endswith('1') else '1f2003d5'  # different game, different fix
                rule['cuts'][0] = (8, 'e00317aa', replacement, 'heap')
                p.write_text(f'MATCH_CUTS = {{"main": [{rule!r}]}}\n' +
                             ("CUTS = {'" + 'ab' * 16 + "': [(0x108, 'e00317aa', '1f2003d5', 'known')]}\n" if gid.endswith('1') else ''))
            for gid, name, bid, expected in (
                    ('0100000000000001', 'main', '00' * 16, ADD),
                    ('0100000000000002', 'main', '00' * 16, NOP),
                    ('0100000000000003', 'main', '00' * 16, MOV),
                    ('0100000000000001', 'sdk', '00' * 16, MOV),
                    ('0100000000000001', 'main', 'ab' * 16, NOP)):
                with self.subTest(game=gid, module=name, build=bid):
                    img = bytearray(self.original)
                    out = root / 'out' / gid / name / bid; out.mkdir(parents=True)
                    ns = dict(__file__=str(root / 'tools/patch_nso.py'), os=os, glob=glob, importlib=importlib,
                              MOD=SimpleNamespace(build_id=bytes.fromhex(bid)), IMG=img, TEXT_END=0x2000, NAME=name, outdir=str(out))
                    with patch.dict(os.environ, DM_GAME=gid, DM_GAMES_DIR='/derived/profile', DM_GAMES_FIRST='1'):
                        exec(code, ns)
                    self.assertEqual(struct.unpack_from('<I', img, 0x108)[0], expected)
                    # an unregistered build with rules lists their results; a registered build and a module without rules write nothing
                    tsv = out / f'{name}.match.tsv'
                    self.assertEqual(tsv.is_file(), expected != MOV and bid == '00' * 16)
                    if tsv.is_file(): self.assertEqual([l.split('\t')[:3] for l in tsv.read_text().splitlines()[1:]], [['patch', 'heap', '0x108']])

    def test_registry_reports_what_an_unregistered_build_lost(self):
        # the repository profile's build rows name the registered builds of the module, whose CUTS the unregistered build did not get
        source = TOOLS / 'patch_nso.py'; tree = ast.parse(source.read_text())
        start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'PATCH_FILES')
        end = next(i for i, n in enumerate(tree.body[start:], start) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'KEYS')
        code = compile(ast.Module(body=tree.body[start:end], type_ignores=[]), str(source), 'exec')
        gid, known = '0100000000000001', 'ab' * 16
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'tools').mkdir(); (root / 'tools/sdk_patches.py').write_text('')
            g = root / 'games' / gid; g.mkdir(parents=True)
            (g / 'patches.py').write_text("CUTS = {'" + known + "': [(0x108, 'e00317aa', '1f2003d5', 'a'), (0x10c, '1f2003d5', '1f2003d5', 'b')]}\n")
            (g / 'game.tsv').write_text(f'build\tv65536\t1.0.1\tmain={known}\tsdk={"cd" * 16}\n')
            img = bytearray(self.original)
            ns = dict(__file__=str(root / 'tools/patch_nso.py'), os=os, glob=glob, importlib=importlib,
                      MOD=SimpleNamespace(build_id=bytes(16)), IMG=img, TEXT_END=0x2000, NAME='main', outdir=str(root / 'out'))
            with patch.dict(os.environ, DM_GAME=gid): exec(code, ns)
            self.assertEqual(bytes(img), bytes(self.original))
            rows = [l.split('\t') for l in (root / 'out/main.match.tsv').read_text().splitlines()[1:]]
            self.assertEqual(rows, [['registered', known, '1.0.1', '2']])
            # the game's own build (a build row without CUTS) is no loss: no file
            ns.update(MOD=SimpleNamespace(build_id=bytes.fromhex('cd' * 16)), NAME='sdk', outdir=str(root / 'own'))
            with patch.dict(os.environ, DM_GAME=gid): exec(code, ns)
            self.assertFalse((root / 'own').exists())

    def test_sdk_wide_rules_apply_to_every_build_without_cuts(self):
        # sdk_patches.py's MATCH_CUTS apply to every game's sdk of a build with no CUTS, the selected profile's own builds too (without a match.tsv: they lost nothing); an empty
        # CUTS list keeps Nintendo's code; a miss is not red
        source = TOOLS / 'patch_nso.py'; tree = ast.parse(source.read_text())
        start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'PATCH_FILES')
        end = next(i for i, n in enumerate(tree.body[start:], start) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'KEYS')
        code = compile(ast.Module(body=tree.body[start:end], type_ignores=[]), str(source), 'exec')
        gid, own, kept = '0100000000000001', 'cd' * 16, 'ef' * 16
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'tools').mkdir()
            (root / 'tools/sdk_patches.py').write_text(f'CUTS = {{{kept!r}: []}}\nMATCH_CUTS = {{"sdk": [{self.rule!r}]}}\n')
            g = root / 'games' / gid; g.mkdir(parents=True)
            (g / 'game.tsv').write_text(f'build\tv0\t1.0.0\tmain={"ab" * 16}\tsdk={own}\n')
            for name, bid, gamed, expected in (('sdk', '00' * 16, gid, ADD), ('sdk', own, gid, ADD), ('sdk', own, '0100000000000002', ADD), ('sdk', kept, gid, MOV),
                                               ('main', '00' * 16, gid, MOV)):
                with self.subTest(module=name, build=bid, game=gamed):
                    img, out = bytearray(self.original), root / 'out' / f'{name}{bid}{gamed}'
                    ns = dict(__file__=str(root / 'tools/patch_nso.py'), os=os, glob=glob, importlib=importlib,
                              MOD=SimpleNamespace(build_id=bytes.fromhex(bid)), IMG=img, TEXT_END=0x2000, NAME=name, outdir=str(out))
                    with patch.dict(os.environ, DM_GAME=gamed): exec(code, ns)
                    self.assertEqual(struct.unpack_from('<I', img, 0x108)[0], expected)
                    if expected == ADD and (bid, gamed) != (own, gid):
                        self.assertEqual([l.split('\t')[:3] for l in (out / 'sdk.match.tsv').read_text().splitlines()[1:]], [['sdk', 'heap', '0x108']])
                    else: self.assertFalse((out / 'sdk.match.tsv').exists())
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                ns = dict(__file__=str(root / 'tools/patch_nso.py'), os=os, glob=glob, importlib=importlib,
                          MOD=SimpleNamespace(build_id=bytes(16)), IMG=bytearray(image({0x100: [RET]})), TEXT_END=0x2000, NAME='sdk', outdir=str(root / 'miss'))
                with patch.dict(os.environ, DM_GAME=gid): exec(code, ns)
            self.assertIn("function patch 'heap' skipped", printed.getvalue())
            self.assertNotIn('[RED]', printed.getvalue())


class RegionGates(unittest.TestCase):
    """The address region gates' sites (patch_nso.py "Address regions"): the SDK's symbols and the one store through the out pointer"""
    PRO, MOV_OUT, RETW = [0xa9bd7bfd, 0x910003fd], 0xaa0103f4, RET   # stp x29, x30, [sp, #-0x30]!; mov x29, sp; mov x20, x1; ret

    def test_the_one_store_through_the_out_pointer(self):
        from nso_code import out_store
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        def fn(*words):
            img = bytearray(0x200); struct.pack_into('<' + 'I' * len(words), img, 0x100, *words); return img, 4 * len(words)
        img, n = fn(*self.PRO, self.MOV_OUT, NOP, 0xf9000296, NOP, self.RETW)   # str x22, [x20]
        self.assertEqual(out_store(img, 0x100, n, md), (0x110, 22))
        img, n = fn(*self.PRO, self.MOV_OUT, 0xf9000295, 0xf9000296, self.RETW)   # two stores: no single answer
        with self.assertRaisesRegex(ValueError, '2 stores'): out_store(img, 0x100, n, md)
        img, n = fn(*self.PRO, NOP, 0xf9000296, self.RETW)   # no copy of x1
        with self.assertRaisesRegex(ValueError, 'out pointer'): out_store(img, 0x100, n, md)
        img, n = fn(*self.PRO, self.MOV_OUT, 0xf9000696, self.RETW)   # str x22, [x20, #8]: a field, not the out value
        with self.assertRaisesRegex(ValueError, '0 stores'): out_store(img, 0x100, n, md)

    def test_defined_symbols(self):
        from nso_syms import defined
        img = imported('_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm')
        self.assertEqual(defined(img), {})   # imported, not defined
        struct.pack_into('<HQQ', img, 0x2318 + 6, 1, 0x500, 0x90)   # st_shndx, st_value, st_size
        self.assertEqual(defined(img), {'_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm': (0x500, 0x90)})
        self.assertEqual(defined(bytearray(0x100)), {})   # no MOD0


class Patches(unittest.TestCase):
    """A patch of several functions: one constant whose uses lie in different functions applies whole or not at all"""
    def setUp(self):
        self.original = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [MOV, NOP, RET]})
        self.rule = Image(self.original, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size'), (0x800, 'e00317aa', 'e0124091', 'bound')], 'region')

    def test_recipe_spans_functions(self):
        self.assertEqual(set(self.rule), {'name', 'functions'})
        self.assertEqual([(f['size'], [c[0] for c in f['cuts']]) for f in self.rule['functions']], [(28, [8]), (12, [0])])
        one = Image(self.original, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size')], 'region')
        self.assertEqual(set(one), {'name', 'size', 'fingerprints', 'cuts'})  # one function: the single form
        self.assertEqual(one['cuts'], [(8, 'e00317aa', 'e0124091', 'size')])

    def test_relocated_functions_match_together(self):
        moved = image({0x900: caller(0x900, 0x1200), 0x1200: [0xd2800020, RET], 0x300: [MOV, NOP, RET]})
        messages = []
        self.assertEqual([c[0] for c in resolve(moved, 0x2000, [self.rule], messages.append)], [0x908, 0x300])
        self.assertFalse([m for m in messages if m.startswith('[RED]')], messages)

    def test_one_changed_function_applies_none(self):
        changed = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [MOV, MOV, RET]})
        before, messages, outcomes = bytes(changed), [], []
        self.assertEqual(resolve(changed, 0x2000, [self.rule], messages.append, outcomes), [])
        self.assertEqual(bytes(changed), before)
        self.assertTrue(messages[0].startswith('[RED] ') and 'function 2 of 2' in messages[0] and 'the other 1 matched' in messages[0], messages)
        self.assertEqual(outcomes[0][:2], ('region', None))

    def test_alternative_implementations_of_a_patch(self):
        # a later build changed the bound's function: a second implementation of the same patch, which shares the first function
        later = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [NOP, MOV, NOP, RET]})
        alt = Image(later, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size'), (0x804, 'e00317aa', 'e0124091', 'bound')], 'region')
        for img, offs in ((self.original, [0x108, 0x800]), (later, [0x108, 0x804])):
            with self.subTest(offs=offs):
                outcomes = []
                self.assertEqual([c[0] for c in resolve(img, 0x2000, [self.rule, alt], lambda _: None, outcomes)], offs)
                self.assertEqual(outcomes[0][1], offs)

    def test_two_functions_cannot_match_one(self):
        rule = {'name': 'region', 'functions': [self.rule['functions'][1], dict(self.rule['functions'][1], cuts=[(4, '1f2003d5', '1f2003d5', 'again')])]}
        with self.assertRaisesRegex(MatchError, 'another one did'): Image(self.original, 0x2000).match_patch(rule)

    def test_malformed_patches_are_rejected(self):
        f = self.rule['functions'][0]
        for bad in ({'name': 'region', 'functions': [f]}, {'name': 'region', 'functions': [f, f], 'size': 4},
                    {'name': 'region', 'functions': [f, dict(f, name='other')]}, {'name': 'region', 'functions': [f, dict(f, cuts=[])]}):
            with self.assertRaises(ValueError): validate(bad)

    def test_pack_report(self):
        # the NSP script's summary of what modules of unregistered builds kept (report_fix_coverage)
        import importlib.util as iu
        repo = TOOLS.parents[1]
        sys.path.insert(0, str(repo / 'dm/games'))
        spec = iu.spec_from_file_location('build_ipa_from_nsp', repo / 'scripts/build_ipa_from_nsp.py'); assert spec and spec.loader
        nsp = iu.module_from_spec(spec); spec.loader.exec_module(nsp)
        reg = type('Reg', (), {'dir': Path('/repo/dm/games/0100000000000001')})()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'main.match.tsv').write_text('# main\nregistered\t' + 'ab' * 16 + '\t13.0.5\t11\n')
            (out / 'sdk.match.tsv').write_text('# sdk\npatch\tnode area\t0x10,0x20\timplementation 1 of 1\npatch\tregion\t-\tfunction 1 of 4: 0 matching functions\n')
            (out / 'subsdk0.match.tsv').write_text('# sdk\nregistered\t' + 'cd' * 16 + '\t1.0\t2\nsdk\tnode area\t0x10,0x20\timplementation 1 of 1\nsdk\talignment\t-\t0 matching\n')
            said = []
            with patch.object(nsp, 'say', said.append): nsp.report_fix_coverage(out, ['main', 'sdk', 'subsdk0'], reg)
        # an SDK-wide patch that applied carries the registered fixes: no red line, only what applied and what did not
        self.assertEqual(said[3:], ["subsdk0: SDK-wide patches applied: 'node area';  with no reviewed implementation in this build, so not applied: 'alignment'"])
        said = said[:3]
        self.assertEqual(len(said), 3, said)
        self.assertTrue(said[0].startswith('[RED] main: no function patch') and '13.0.5 (abababab…) has 11 cuts' in said[0], said[0])
        self.assertTrue(said[1].startswith('[RED] sdk: function patches not applied') and "'region' (function 1 of 4" in said[1], said[1])
        self.assertEqual(said[2], "sdk: function patches applied: 'node area'")


if __name__ == '__main__': unittest.main()

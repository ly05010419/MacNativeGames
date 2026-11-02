"""Shader preparation uses local game containers; seed import preserves user variants."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/prepare_shader_seed.py'
spec = importlib.util.spec_from_file_location('prepare_shader_seed', SCRIPT)
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def bnsh():
    b = bytearray(0x3000)
    b[:4] = b'BNSH'; b[12:14] = b'\xff\xfe'; b[96:100] = b'grsc'
    struct.pack_into('<I', b, 28, len(b))
    struct.pack_into('<IQ', b, 124, 1, 0xc0)
    struct.pack_into('<Q', b, 0xc0+16, 0x100)
    for i, (stage, rel) in enumerate([(0, 8), (1, 40), (5, 48)]):
        header, control, code = 0x200+i*64, 0x400+i*0x880, 0x2000+i*0x100
        struct.pack_into('<Q', b, 0x100+rel, header)
        struct.pack_into('<QQII', b, header+8, control, code, 0x100, 0x880)
        struct.pack_into('<II', b, control+0x700, 0x80, 0x100)
        struct.pack_into('<I', b, control+0x714, stage)
        struct.pack_into('<III', b, control+0x748, 8, 8, 1)
        b[code:code+0x100] = bytes([i+1])*0x100
    return b


def bnsh_botw():
    """Breath of the Wild's layout: control sections of 0x7d1 bytes that say so at +0x7c8, each followed by other data."""
    b = bnsh()
    for i in range(3):
        control = 0x400+i*0x880
        struct.pack_into('<I', b, control+0x20, 0x7d1)
        b[control+0x7d1:control+0x880] = bytes([0xee])*(0x880-0x7d1)   # what follows: not the shader's
        struct.pack_into('<I', b, 0x200+i*64+8+20, 0x7d1)   # the program table's control size
    return b


def twin(row):
    """A shader differing from row's only in a control byte no translation reads (+0x7d0, as 3,416 of TOTK 1.4.3's)."""
    _, stage, control, code = row
    control = bytearray(control); control[0x7d0] ^= 1
    return seed.digest(bytes(control)+code), stage, bytes(control), code


class ShaderInventory(unittest.TestCase):
    def test_the_key_hashes_the_control_sections_own_size_as_the_runtime_does(self):
        self.assertEqual(seed.control_key_len(bytes(0x880)), 0x880)              # no size word: the NVN 55 section
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x88c)+bytes(0x860)), 0x880)   # 0x88c: hashed as 0x880
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x7d1)+bytes(0x860)), 0x7d1)
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x848)+bytes(0x860)), 0x848)
        def flagged(size, flag):   # a section with this size word and +0x718
            c = bytearray(0x880); c[0x20:0x24] = struct.pack('<I', size); c[0x718] = flag; return bytes(c)
        self.assertTrue(seed.early_fragment_tests('ps', flagged(0x880, 1)))
        self.assertTrue(seed.early_fragment_tests('ps', flagged(0, 1)))               # no size word: the 0x880 layout too
        self.assertFalse(seed.early_fragment_tests('ps', flagged(0x880, 0)))
        self.assertFalse(seed.early_fragment_tests('vs', flagged(0x880, 1)))          # a fragment flag only
        self.assertFalse(seed.early_fragment_tests('ps', flagged(0x848, 1)))          # Breath of the Wild's layout: not read
        with tempfile.TemporaryDirectory() as tmp:   # the seed's requests carry it
            def request(stage, flag):
                c = bytearray(flagged(0x880, flag)); struct.pack_into('<II', c, 0x700, 0x30, 0x40)
                return seed.translation_request('v', stage, bytes(c), bytes(0x40), {}, Path(tmp)/'v.code')
            self.assertIs(request('ps', 1).get('early_fragment_tests'), True)
            self.assertNotIn('early_fragment_tests', request('ps', 0))
            self.assertNotIn('early_fragment_tests', request('vs', 1))
        agl = bytes(0x14)+b'\x40\0\0\0'+bytes(8)+struct.pack('<I', 0x848)+bytes(0x848-0x24)
        self.assertEqual(seed.bound_control(agl), agl)                    # Tears of the Kingdom: as in the archive
        self.assertEqual(seed.bound_control(agl, 'botw')[0x14:0x16], bytes(2))
        self.assertEqual(seed.bound_control(agl, 'botw')[0x790], 0x1c)
        self.assertEqual(seed.bound_control(agl[:0x7d1], 'botw'), agl[:0x7d1])   # only the 0x848 sections are rewritten
        self.assertEqual(seed.bound_control(agl, 'botw', embedded=False), agl)    # an agl package's own record: bound as it is
        rows = list(seed.bnsh_shaders(bnsh_botw()))
        self.assertEqual([(r[1], len(r[2])) for r in rows], [('vs', 0x7d1), ('ps', 0x7d1), ('cs', 0x7d1)])
        for key, _, control, code in rows:
            self.assertEqual(key, hashlib.sha256(control+code).hexdigest())
            self.assertNotIn(0xee, control)

    def test_yaz0_round_trip_with_literals_and_overlapping_copies(self):
        plain = b'abcabcabcabcabcabcXYZ' + bytes(range(256))*3 + b'Q'*300
        # a reference encoder: greedy longest match, as the tools that wrote the game's files do
        out = bytearray(b'Yaz0' + struct.pack('>I', len(plain)) + bytes(8)); i = 0
        while i < len(plain):
            code, items = 0, bytearray()
            for bit in range(8):
                if i >= len(plain):
                    break
                best, at = 0, 0
                for dist in range(1, min(i, 0x1000)+1):
                    n = 0
                    while n < min(0x111, len(plain)-i) and plain[i+n] == plain[i-dist+n]:
                        n += 1
                    if n > best:
                        best, at = n, dist
                if best >= 3:
                    if best >= 0x12:
                        items += bytes([(at-1) >> 8, (at-1) & 0xff, best-0x12])
                    else:
                        items += bytes([(best-2) << 4 | (at-1) >> 8, (at-1) & 0xff])
                    i += best
                else:
                    code |= 0x80 >> bit; items.append(plain[i]); i += 1
            out += bytes([code]) + items
        self.assertEqual(seed.yaz0(bytes(out)), plain)
        with self.assertRaises(ValueError):
            seed.yaz0(bytes(out[:-5]))

    def test_sarc_members_named_with_a_leading_slash_are_accepted(self):
        name = b'/x.bgdata\0\0\0\0\0\0\0'
        b = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, 0, 0x100, 0x100, 0))   # header, BOM, size, data offset, version
        b += b'SFAT' + struct.pack('<HHI', 0xc, 1, 0x65) + struct.pack('<4I', 0, 0x01000000, 0, 4)
        b += b'SFNT' + struct.pack('<HH', 8, 0) + name
        b += bytes(0x100-len(b)) + b'DATA'
        struct.pack_into('<I', b, 8, len(b))
        self.assertEqual(seed.sarc(bytes(b)), {'x.bgdata': b'DATA'})

    def test_actual_stage_offsets_and_runtime_hash_contract(self):
        rows = list(seed.bnsh_shaders(bnsh()))
        self.assertEqual([r[1] for r in rows], ['vs', 'ps', 'cs'])
        for key, _, control, code in rows:
            self.assertEqual(key, hashlib.sha256(control+code).hexdigest())

    def test_agl_shader_packages_give_control_and_code_through_their_records(self):
        # BAHS header: start of the records 0x100, code pool at 0x2000 (size 0x400). A record is <u64 code offset in the
        # pool, u32 control size, i32 offset of the control from the record>; a later record may reuse an earlier control.
        b = bytearray(0x2400)
        b[:4] = b'BAHS'; struct.pack_into('<8I', b, 0, 0x53484142, 9, len(b), 5, 0, 0x100, 0x400, 0x2000)
        def control(stage, length):
            c = bytearray(0x880); struct.pack_into('<II', c, 0x700, 0x40, length); struct.pack_into('<I', c, 0x714, stage)
            return bytes(c)
        struct.pack_into('<QIi', b, 0x100, 0x000, 0x880, 0x60); b[0x160:0x160+0x880] = control(0, 0x100)
        struct.pack_into('<QIi', b, 0x1000, 0x100, 0x88c, 0x60); b[0x1060:0x1060+0x880] = control(1, 0x200)  # 0x88c: hashed as 0x880
        struct.pack_into('<QIi', b, 0x1a00, 0x300, 0x880, 0x160-0x1a00)   # reuses the first control
        for at, fill in ((0x2000, 1), (0x2100, 2), (0x2300, 3)):
            b[at:at+0x100] = bytes([fill])*0x100
        got = {k: (stage, len(code)) for k, stage, _, code in seed.sharcb_shaders(bytes(b))}
        want = {hashlib.sha256(control(0, 0x100)+bytes([1])*0x100).hexdigest(): ('vs', 0x100),
                hashlib.sha256(control(1, 0x200)+bytes([2])*0x100+bytes(0x100)).hexdigest(): ('ps', 0x200),
                hashlib.sha256(control(0, 0x100)+bytes([3])*0x100).hexdigest(): ('vs', 0x100)}
        self.assertEqual(got, want)

    def test_listed_bind_states_become_jobs_for_shaders_in_this_game(self):
        found = {'a'*64: ('vs', b'', b'', 'Shader/x'), 'b'*64: ('ps', b'', b'', 'Effect/y')}
        listed = {('a'*64, '{"attr":{"0":"sint"},"fout":{},"img":{},"tex":{}}'),
                  ('b'*64, '{"attr":{},"fout":{"0":"uint"},"img":{},"tex":{"1":"cube"}}'),
                  ('c'*64, '{"attr":{},"fout":{},"img":{},"tex":{}}')}   # a shader this game does not have
        jobs = seed.plan(found, startup={'a'*64}, listed=listed)
        # every startup shader gets the default bind state first; listed states follow, only for shaders found here
        self.assertEqual([(k[0], json.dumps(a, sort_keys=True), why) for k, a, _, why in jobs[:1]],
                         [('a', json.dumps(seed.ANSWERS, sort_keys=True), 'default')])
        self.assertEqual(sorted((k[0], why) for k, _, _, why in jobs), [('a', 'default'), ('a', 'listed'), ('b', 'listed')])
        for key, answers, variant, _ in jobs:
            self.assertEqual(set(answers), {'tex', 'img', 'attr', 'fout'})
            self.assertEqual(variant, key+'-'+seed.suffix(answers))
        self.assertEqual(seed.suffix(seed.ANSWERS), seed.SUFFIX)

    def test_located_shaders_are_read_from_the_named_place_and_checked_against_their_key(self):
        control = bytearray(0x880)
        struct.pack_into('<II', control, 0x700, 0x40, 0x100); struct.pack_into('<I', control, 0x714, 1)
        code = bytes(range(256))
        blob = bytes(0x200) + bytes(control) + bytes(0x80) + code
        key = hashlib.sha256(bytes(control)+code).hexdigest()
        sources = {'Lib/x.sarc.zs/y.sharcb': blob}
        good = seed.located({(key, 'Lib/x.sarc.zs/y.sharcb', 0x200, 0xb00, 0x100)}, sources.get)
        self.assertEqual(list(good), [key]); self.assertEqual(good[key][0], 'ps')
        wrong = seed.located({('f'*64, 'Lib/x.sarc.zs/y.sharcb', 0x200, 0xb00, 0x100)}, sources.get)
        self.assertEqual(wrong, {})   # bytes of another game version: not used
        self.assertEqual(seed.located({(key, 'Lib/missing', 0x200, 0xb00, 0x100)}, sources.get), {})

    def test_truncated_and_out_of_range_container_is_rejected(self):
        for data in [bnsh()[:-1], bnsh()]:
            if len(data) == 0x3000:
                struct.pack_into('<Q', data, 0xc0+16, 0x2fff)
            with self.assertRaises(ValueError):
                list(seed.bnsh_shaders(data))


class ShaderKeyScope(unittest.TestCase):
    def test_scope_has_sorted_unique_raw_keys_and_game_identity(self):
        scope = seed.shader_key_scope(['b'*64, 'a'*64, 'b'*64], '1.2.1', 'c'*64)
        self.assertEqual(scope, dict(schema=1, chain=seed.CHAIN, version='1.2.1',
                                     source_romfs_sha256='c'*64, keys=['a'*64, 'b'*64]))

    def test_invalid_identity_or_variant_names_cannot_become_raw_keys(self):
        for keys in [[], 'a'*64, ['a'*63], ['A'*64], ['g'*64], ['a'*64+'-123456789abc'], [None]]:
            with self.subTest(keys=keys), self.assertRaises(ValueError):
                seed.shader_key_scope(keys, '1.2.1', 'c'*64)
        for kwargs in [dict(chain='other'), dict(source_romfs_sha256='unknown'), dict(version='')]:
            values = dict(keys=['a'*64], version='1.2.1', source_romfs_sha256='c'*64)
            values.update(kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                seed.shader_key_scope(**values)

    def test_preparation_keeps_untranslated_and_located_keys_and_rebuilds_old_seed(self):
        # A tiny service prepares the shaders. One is a startup shader; the other inventory shader and the verified
        # locator are translated for the default bind state too (all_default) and belong to this game either way.
        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self

            def write(self, line):
                request = json.loads(line)
                base = Path(request['code']).with_suffix('')
                base.with_name(base.name+'_b.metal').write_text('// synthetic shader')
                base.with_name(base.name+'_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage=request['stage'], deps=seed.ANSWERS)))

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); romfs = root/'romfs'; romfs.mkdir()
            (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            translator, cross, shared = [root/name for name in ['translator', 'spirv-cross', 'list.tsv']]
            for path in [translator, cross, shared]: path.write_text('')
            rows = list(seed.bnsh_shaders(bnsh()))
            shaders = {key: (stage, control, code, 'Shader/test') for key, stage, control, code in rows[:2]}
            extra = {rows[2][0]: (*rows[2][1:], 'module:main')}
            output = root/'seed'
            with mock.patch.object(seed, 'MIN_SHADERS', 1), mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=({}, shaders, {rows[0][0]}, [])) as inventory, \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed, 'located', return_value=extra), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                manifest = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                scope = json.loads((output/'shader-keys.json').read_text())
                self.assertEqual(scope['keys'], sorted(row[0] for row in rows))
                self.assertEqual((manifest['shaders'], manifest['discovered'], manifest['located']), (3, 3, 1))
                self.assertEqual(manifest['startup_shaders'], 1)
                self.assertEqual(manifest['files']['shader-keys.json'], seed.digest((output/'shader-keys.json').read_bytes()))
                self.assertEqual(seed.prepare(romfs, output, translator, cross, '1.2.1', shared), manifest)
                self.assertEqual(inventory.call_count, 1)
                # A pre-metadata seed may have all successful translations and matching build hashes. It must still
                # regenerate its inventory instead of treating that smaller list as the game's full scope.
                # (The scan itself comes from the inventory cache now: the RomFS is read once.)
                del manifest['files']['shader-keys.json']
                (output/'seed-manifest.json').write_text(json.dumps(manifest))
                again = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                self.assertIn('shader-keys.json', again['files'])
                self.assertEqual(json.loads((output/'shader-keys.json').read_text())['keys'], scope['keys'])
                (output/'shader-keys.json').unlink()
                again = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                self.assertEqual(json.loads((output/'shader-keys.json').read_text())['keys'], scope['keys'])
                self.assertEqual(inventory.call_count, 1)

    def test_shaders_differing_only_in_control_bytes_no_translation_reads_share_one_translation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = list(seed.bnsh_shaders(bnsh()))
            first, second = sorted([rows[1][0], twin(rows[1])[0]])
            cube = self.deps(tex={'1': 'cube'})
            manifest, asked, _ = self.prepare_with_service(root, root/'seed', [(second, cube)], twins=(1,))
            answers = {**seed.ANSWERS, 'tex': {'1': 'cube'}}
            self.assertEqual(sorted(asked), sorted([rows[0][0]+'-'+seed.SUFFIX, first+'-'+seed.SUFFIX, rows[2][0]+'-'+seed.SUFFIX,
                                                    first+'-'+seed.suffix(answers)]))   # the twin's listed state too, as the first's
            out = root/'seed'
            self.assertEqual((out/(second+'.variants')).read_text(), (out/(first+'.variants')).read_text())
            self.assertFalse(any(p.name.startswith(second+'-') for p in out.iterdir()))
            self.assertEqual((manifest['served_by_another'], manifest['startup_shaders'], manifest['shaders']), (1, 3, 4))
            self.assertIn(second+'.variants', manifest['files'])
            self.assertFalse((root/'seed.aliases.tsv').exists())   # rows naming the twin's own variants: shader_list.py resolve
            self.assertTrue(seed.builtin_serves(out, second, {}))   # the shared list's prune reads its default the same way

    def prepare_with_service(self, root, output, listed=(), translator_text='', fail=(), inventory_calls=None, compare=(),
                             twins=()):
        """prepare() with a synthetic service; returns (manifest, the variants it was asked to translate). twins: indices of
        shaders that get a twin (startup too) differing only in a control byte no translation reads."""
        asked = []

        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self
                self.reply = ''

            def write(self, line):
                request = json.loads(line)
                asked.append(request['variant'])
                if request['variant'] in fail:
                    self.reply = '{"ok":false,"error":"synthetic failure"}\n'
                    return
                base = Path(request['code']).with_suffix('')
                deps = {k: request[k] for k in ('tex', 'img', 'attr', 'fout')}
                deps['tex'] = {'1': '2d', '2': '2d', **deps['tex']}   # as if the code queried the kind of slots 1 and 2
                key = request['variant'].split('-')[0]
                if key in compare:   # slot 3 is compare-sampled and not queried: only whether a depth texture is bound counts
                    deps['tex']['3'] = '*_depth' if deps['tex'].get('3', '').endswith('_depth') else '*'
                base.with_name(base.name+'_b.metal').write_text('// synthetic '+request['variant'])
                base.with_name(base.name+'_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage=request['stage'], deps=deps, compare_slots=[3] if key in compare else [])))
                self.reply = '{"ok":true}\n'

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return self.reply

        romfs = root/'romfs'; romfs.mkdir(exist_ok=True)
        (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
        translator, cross, shared = [root/name for name in ['translator', 'spirv-cross', 'list.tsv']]
        translator.write_text(translator_text); cross.write_text('')
        shared.write_text(''.join(f'V\t{key}\t{deps}\n' for key, deps in listed))
        rows = list(seed.bnsh_shaders(bnsh()))
        rows += [twin(rows[i]) for i in twins]
        shaders = {key: (stage, control, code, 'Shader/test') for key, stage, control, code in rows}
        startup = {rows[0][0], rows[1][0]} | {row[0] for row in rows[3:]}
        with mock.patch.object(seed, 'MIN_SHADERS', 1), mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                mock.patch.object(seed, 'inventory', return_value=({}, dict(shaders), set(startup), [])) as inventory, \
                mock.patch.object(seed, 'located', return_value={}), \
                mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                mock.patch.object(seed.ShaderList, 'load', return_value=mock.Mock(
                    variants={(key, deps) for key, deps in listed}, locators=set())), \
                mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
            manifest = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
        if inventory_calls is not None:
            inventory_calls.append(inventory.call_count)
        return manifest, asked, rows

    @staticmethod
    def tree(folder):
        return {p.name: p.read_bytes() for p in sorted(folder.iterdir()) if p.name != 'seed-manifest.json'}

    @staticmethod
    def deps(**answers):
        return json.dumps(answers, sort_keys=True, separators=(',', ':'))

    def test_a_longer_list_translates_only_its_new_bind_states_and_gives_the_same_seed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = list(seed.bnsh_shaders(bnsh()))
            first = [(rows[0][0], self.deps(tex={'1': 'cube'}))]
            more = first+[(rows[1][0], self.deps(tex={'2': '3d'}))]
            calls = []
            _, asked, _ = self.prepare_with_service(root, root/'seed', first, inventory_calls=calls)
            self.assertEqual(len(asked), 4)   # every shader's default bind state and one listed
            (root/'seed'/('f'*64+'-'+'0'*12+'_b.metal')).write_text('left from an older seed')   # removed, not kept
            manifest, asked, _ = self.prepare_with_service(root, root/'seed', more, inventory_calls=calls)
            self.assertEqual(asked, [rows[1][0]+'-'+seed.suffix({**seed.ANSWERS, 'tex': {'2': '3d'}})])
            self.assertEqual(calls, [1, 0])   # the RomFS was not scanned again
            self.assertEqual(manifest['shaders'], 5)
            # A fresh preparation of the longer list, with no caches, gives byte-identical files.
            fresh = root/'fresh'; fresh.mkdir()
            full, asked, _ = self.prepare_with_service(fresh, fresh/'seed', more)
            self.assertEqual(len(asked), 5)
            self.assertEqual(self.tree(root/'seed'), self.tree(fresh/'seed'))
            self.assertEqual(manifest['files'], full['files'])

    def test_a_listed_state_another_variant_serves_is_not_translated(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = list(seed.bnsh_shaders(bnsh()))
            # slot 5 was never queried: the default variant serves a 3D texture there; slot 1 was, so a cube needs its own
            listed = [(rows[0][0], self.deps(tex={'5': '3d'})), (rows[0][0], self.deps(tex={'1': 'cube'}))]
            manifest, asked, _ = self.prepare_with_service(root, root/'seed', listed)
            served = rows[0][0]+'-'+seed.suffix({**seed.ANSWERS, 'tex': {'5': '3d'}})
            self.assertNotIn(served, asked)
            self.assertIn(rows[0][0]+'-'+seed.suffix({**seed.ANSWERS, 'tex': {'1': 'cube'}}), asked)
            self.assertFalse((root/'seed.aliases.tsv').exists())   # shader_list.py resolve names such rows by their bind state
            self.assertEqual(manifest['shaders'], 4)

    def test_a_shader_sampling_shadow_maps_gets_its_depth_bound_variant_without_a_listed_row(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = list(seed.bnsh_shaders(bnsh()))
            shadow = rows[0][0]
            listed = [(shadow, self.deps(tex={'3': '2darray_depth'}))]   # what a player met: served by the derived variant
            manifest, asked, _ = self.prepare_with_service(root, root/'seed', listed, compare={shadow})
            depth = shadow+'-'+seed.suffix(seed.answers_for({'tex': {'3': '*_depth'}}))
            self.assertIn(depth, asked)
            self.assertNotIn(shadow+'-'+seed.suffix(seed.answers_for(json.loads(listed[0][1]))), asked)
            self.assertEqual(manifest['shaders'], 4)   # three defaults and the derived one
            self.assertIs(manifest['all_default'], True)   # the runtime contributes only other states then
            index = json.loads((root/'seed'/(shadow+'.variants')).read_text())
            self.assertEqual([row['deps']['tex']['3'] for row in index], ['*', '*_depth'])
            # the derived translation stays in the translation cache: a second preparation asks for nothing
            (root/'seed'/'seed-manifest.json').unlink()
            _, again, _ = self.prepare_with_service(root, root/'seed', listed, compare={shadow})
            self.assertEqual(again, [])

    def test_deps_match_reads_wildcard_texture_kinds_and_ignores_vertex_input_types_as_the_runtime_does(self):
        deps = {'tex': {'1': '*', '2': '*_depth', '3': '2d'}, 'attr': {'0': 'uint'}, 'fout': {}, 'img': {}}
        self.assertTrue(seed.deps_match(deps, {'tex': {'1': 'cube', '2': '2darray_depth'}, 'attr': {'0': 'uint'}}))
        self.assertTrue(seed.deps_match(deps, {'tex': {'2': '2d_depth'}}))              # unbound slot 1 and input 0
        self.assertFalse(seed.deps_match(deps, {'tex': {'1': '2d_depth', '2': '2d_depth'}}))
        self.assertFalse(seed.deps_match(deps, {'tex': {'2': '2d'}}))
        self.assertFalse(seed.deps_match(deps, {'tex': {'2': '2d_depth', '3': '3d'}}))   # a queried kind still counts
        # the pipeline redeclares an input with its bound format's type (nvn_mtl.m vs_vin): any variant serves it
        self.assertTrue(seed.deps_match(deps, {'tex': {'2': '2d_depth'}, 'attr': {'0': 'float'}}))
        self.assertEqual(seed.answers_for({'tex': {'1': '*', '2': '*_depth', '3': 'cube'}})['tex'],
                         {'1': '2d', '2': '2d_depth', '3': 'cube'})

    def test_a_shared_translation_cache_serves_another_work_folder_and_keeps_its_entries(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'a').mkdir(); (root/'b').mkdir()
            with mock.patch.dict(os.environ, {'TOTK_TRANSLATION_CACHE': str(root/'translations')}):
                _, first, _ = self.prepare_with_service(root/'a', root/'a'/'seed')
                _, again, _ = self.prepare_with_service(root/'b', root/'b'/'seed')   # another work folder (a clone)
            self.assertTrue(first)
            self.assertEqual(again, [])
            self.assertFalse((root/'a'/'seed.translations').exists())
            [shared] = [p for p in (root/'translations').iterdir() if p.is_dir()]
            self.assertTrue(any(p.name.endswith('_b.metal') for p in shared.iterdir()))
            # a new translator gets a folder of its own; the old one stays for whoever still uses it
            with mock.patch.dict(os.environ, {'TOTK_TRANSLATION_CACHE': str(root/'translations')}):
                _, other, _ = self.prepare_with_service(root/'b', root/'b'/'seed', translator_text='another build')
            self.assertEqual(sorted(other), sorted(first))
            self.assertEqual(len([p for p in (root/'translations').iterdir() if p.is_dir()]), 2)

    def test_builtin_serves_only_the_default_and_depth_bound_variants_not_listed_ones(self):
        key, other = 'a'*64, 'b'*64
        depth = seed.answers_for({'tex': {'3': '*_depth'}})
        rows = [{'variant': f'{key}-{seed.SUFFIX}', 'deps': {'tex': {'3': '*'}, 'attr': {'6': 'uint'}, 'fout': {'0': 'float'}}},
                {'variant': f'{key}-{seed.suffix(depth)}', 'deps': {'tex': {'3': '*_depth'}, 'attr': {'6': 'uint'}, 'fout': {'0': 'float'}}},
                {'variant': f'{key}-{"c"*12}', 'deps': {'fout': {'0': 'uint'}}}]   # a listed variant the seed also made
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/f'{key}.variants').write_text(json.dumps(rows))
            (root/f'{key}-{seed.SUFFIX}_b.json').write_text(json.dumps({'compare_slots': [3]}))
            self.assertTrue(seed.builtin_serves(root, key, '{"tex":{"3":"2darray"},"attr":{"6":"uint"}}'))
            self.assertTrue(seed.builtin_serves(root, key, {'tex': {'3': '2d_depth'}}))      # the depth-bound one
            self.assertTrue(seed.builtin_serves(root, key, {'attr': {'6': 'float'}}))       # a float input: the pipeline retypes it
            self.assertTrue(seed.builtin_serves(root, key, {'fout': {'0': 'uint'}}))       # an integer target: the pipeline retypes the output
            self.assertEqual(len(seed.builtin_variants(root, key)), 2)                     # the listed variant the seed also made is not built in
            self.assertFalse(seed.builtin_serves(root, other, {}))                          # not a shader of this seed
            lst = root/'list.tsv'
            lst.write_text(f'V\t{key}\t{{"attr":{{"6":"uint"}},"fout":{{}},"img":{{}},"tex":{{"3":"2d"}}}}\n'
                           f'V\t{key}\t{{"attr":{{"6":"float"}},"fout":{{}},"img":{{}},"tex":{{}}}}\n'
                           f'V\t{other}\t{{"attr":{{}},"fout":{{}},"img":{{}},"tex":{{}}}}\n')
            done = subprocess.run(['python3', '-I', str(ROOT/'games/totk/scripts/shader_list.py'), 'prune', str(lst), str(root)],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            kept = [l for l in lst.read_text().splitlines() if l.startswith('V')]
            self.assertEqual(kept, [f'V\t{other}\t{{"attr":{{}},"fout":{{}},"img":{{}},"tex":{{}}}}'])   # input types: served too

    def test_a_new_translator_empties_the_translation_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.prepare_with_service(root, root/'seed')
            _, asked, _ = self.prepare_with_service(root, root/'seed', translator_text='another build')
            self.assertEqual(len(asked), 3)

    def test_failed_translations_are_remembered_and_unused_entries_dropped(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = list(seed.bnsh_shaders(bnsh()))
            broken = rows[1][0]+'-'+seed.SUFFIX
            listed = [(rows[0][0], self.deps(tex={'1': 'cube'}))]
            manifest, asked, _ = self.prepare_with_service(root, root/'seed', listed, fail={broken})
            self.assertEqual([r['variant'] for r in manifest['skipped']], [broken])
            again, asked, _ = self.prepare_with_service(root, root/'seed', (), fail={broken})
            self.assertEqual(asked, [])
            self.assertEqual(again['skipped'], manifest['skipped'])
            dropped = listed[0][0]+'-'+seed.suffix({**seed.ANSWERS, 'tex': {'1': 'cube'}})
            self.assertFalse(any(p.name.startswith(dropped) for p in (root/'seed.translations').iterdir()))

    def test_a_changed_inventory_cache_is_scanned_again(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); calls = []
            self.prepare_with_service(root, root/'seed', inventory_calls=calls)
            blob = root/'seed.inventory'/'shaders.bin'
            data = bytearray(blob.read_bytes()); data[0] ^= 1; blob.write_bytes(bytes(data))
            listed = [(list(seed.bnsh_shaders(bnsh()))[0][0], self.deps(tex={'9': 'cube'}))]   # not the whole-seed reuse
            self.prepare_with_service(root, root/'seed', listed, inventory_calls=calls)
            self.assertEqual(calls, [1, 1])

    def test_reuse_takes_recorded_digests_of_unchanged_files_and_hashes_changed_ones(self):
        # A whole-seed reuse hashed all 65,436 files again (30 s, 2026-10-21); a file whose identity (inode, size,
        # modification and change times) is the one recorded after it was checked keeps its recorded digest.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); output = root/'seed'
            manifest, _, _ = self.prepare_with_service(root, output)
            identity = {k: manifest[k] for k in ('schema', 'chain', 'version', 'game', 'source_romfs_sha256', 'translator_sha256',
                                                 'spirv_cross_sha256', 'preparation_sha256', 'shader_list_sha256')}
            recorded = root/'seed.verified.json'
            self.assertEqual(seed.reusable_seed(output, identity, 1), manifest)
            name = next(n for n in manifest['files'] if n.endswith('_b.metal'))
            record = json.loads(recorded.read_text())
            good = record['files'][name][-1]
            record['files'][name][-1] = '0'*64   # a wrong recorded digest is believed: proof the file was not read
            recorded.write_text(json.dumps(record))
            self.assertIsNone(seed.reusable_seed(output, identity, 1))
            record['files'][name][-1] = good
            recorded.write_text(json.dumps(record))
            self.assertEqual(seed.reusable_seed(output, identity, 1), manifest)
            path = output/name; old = path.stat()
            path.write_text('// changed'); os.utime(path, ns=(old.st_atime_ns, old.st_mtime_ns))   # same size? no matter: ctime moved
            self.assertIsNone(seed.reusable_seed(output, identity, 1))

    def test_reuse_rejects_wrong_scope_identity_even_with_a_matching_file_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            identity = dict(schema=1, chain=seed.CHAIN, version='1.2.1', source_romfs_sha256='c'*64)
            good = seed.shader_key_scope(['a'*64], '1.2.1', 'c'*64)
            for changes in [{}, dict(chain='other'), dict(version='1.0.0'), dict(source_romfs_sha256='d'*64),
                            dict(keys=['a'*64+'-123456789abc']), dict(keys=[])]:
                scope = {**good, **changes}
                (root/'shader-keys.json').write_text(json.dumps(scope))
                manifest = dict(**identity, shaders=seed.MIN_SHADERS,
                                files={'shader-keys.json': seed.digest((root/'shader-keys.json').read_bytes())})
                (root/'seed-manifest.json').write_text(json.dumps(manifest))
                with self.subTest(changes=changes):
                    self.assertEqual(seed.reusable_seed(root, identity), None if changes else manifest)


@unittest.skipUnless(shutil.which('xcrun'), 'native Foundation importer is Mac-only')
class ShaderImport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        source = cls.root/'probe.m'
        source.write_text('''#import "totk_shader_seed.h"
int main(int argc, const char **argv) { @autoreleasepool {
 NSError *error = nil; NSDictionary *r = nil;
 if (argc == 4 && !strcmp(argv[1], "--fill")) r = totk_fill_import(@(argv[2]), @(argv[3]));
 else if (argc == 3) r = totk_seed_import(@(argv[1]), @(argv[2]), &error);
 else return 2;
 if (!r) { fprintf(stderr, "%s\\n", error.localizedDescription.UTF8String); return 1; }
 NSData *d = [NSJSONSerialization dataWithJSONObject:r options:0 error:nil];
 puts([[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding].UTF8String); return 0;
} }
''')
        cls.binary = cls.root/'probe'
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'),
                        str(source), '-framework', 'Foundation', '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixture(self, root):
        source, cache = root/'seed', root/'cache'
        source.mkdir(); cache.mkdir()
        key, variant = 'a'*64, 'a'*64+'-'+'b'*12
        files = {variant+'_b.metal': b'// synthetic shader', variant+'_b.json': b'{}',
                 key+'.variants': json.dumps([dict(variant=variant, deps={})]).encode(),
                 'pso_manifest.tsv': ('S\t'+variant+'_b\t0\n').encode()}
        for name, data in files.items():
            (source/name).write_bytes(data)
        manifest = dict(schema=1, chain=seed.CHAIN, shaders=1,
                        files={n: hashlib.sha256(d).hexdigest() for n, d in files.items()})
        (source/'seed-manifest.json').write_text(json.dumps(manifest))
        return source, cache, key, variant

    def run_probe(self, source, cache, good=True, fill=False):
        r = subprocess.run([str(self.binary)]+(['--fill'] if fill else [])+[str(source), str(cache)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0 if good else 1, r.stderr)
        return json.loads(r.stdout) if good else r.stderr

    def test_first_import_reuse_and_preservation_of_user_variants(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            user = dict(variant=key+'-'+'c'*12, deps={'tex': {'0': '2d_array'}})
            (cache/(key+'.variants')).write_text(json.dumps([user]))
            (cache/'unrelated.txt').write_text('keep')
            r = self.run_probe(source, cache)
            self.assertGreater(r['imported_files'], 0)
            self.assertEqual(json.loads((cache/(key+'.variants')).read_text()), [user, dict(variant=variant, deps={})])
            self.assertEqual(self.run_probe(source, cache)['imported_files'], 0)
            (cache/(variant+'_b.metal')).unlink()
            self.assertGreater(self.run_probe(source, cache)['imported_files'], 0)
            self.assertEqual((cache/'unrelated.txt').read_text(), 'keep')

    def test_an_empty_cache_gets_the_seed_as_one_folder_clone(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            (source/'pso_title.tsv').write_text('not in the manifest\n')   # the seed folder holds more than the manifest names
            r = self.run_probe(source, cache)
            self.assertTrue(r['cloned_folder'])
            names = set(json.loads((source/'seed-manifest.json').read_text())['files'])
            self.assertEqual({p.name for p in cache.iterdir()}, names | {'seed-installed.json'})
            for name in names:
                self.assertEqual((cache/name).read_bytes(), (source/name).read_bytes())
            (cache/'written-later.txt').write_text('the cache stays writable')
            self.assertEqual(self.run_probe(source, cache)['imported_files'], 0)

    def test_a_sealed_seed_is_read_in_place(self):
        # iOS: the seed inside the signed app is neither hashed nor copied (TOTK_SHADER_SEED_IN_PLACE=1 does the same on a Mac).
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            (source/(variant+'_b.metal')).write_text('changed after packaging')   # a check would refuse it; the seal covers it
            r = subprocess.run([str(self.binary), str(source), str(cache)], capture_output=True, text=True,
                               env=os.environ | {'TOTK_SHADER_SEED_IN_PLACE': '1'})
            self.assertEqual(r.returncode, 0, r.stderr)
            result = json.loads(r.stdout)
            self.assertEqual(result['imported_files'], 0)
            self.assertEqual(sorted(result['overlay']), sorted(json.loads((source/'seed-manifest.json').read_text())['files']))
            self.assertEqual(list(cache.iterdir()), [])

    def test_a_seed_inside_a_signed_mac_app_is_read_in_place(self):
        # The Mac app (build_app.py) carries its seed in the signed bundle, as iOS does: no hash, no copy (63 s on the
        # first start after a packaging, 2026-10-21). Outside a signed .app, or with TOTK_SHADER_SEED_IN_PLACE=0, the
        # seed is checked and imported as before.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            app = root/'Game.app'
            (app/'Contents/_CodeSignature').mkdir(parents=True)
            (app/'Contents/_CodeSignature/CodeResources').write_text('seal')
            inside = app/'Contents/Resources/totk/build'; inside.mkdir(parents=True)
            source, cache, key, variant = self.fixture(inside)
            env = {k: v for k, v in os.environ.items() if k != 'TOTK_SHADER_SEED_IN_PLACE'}
            probe = lambda extra={}: subprocess.run([str(self.binary), str(source), str(cache)], capture_output=True,
                                                    text=True, env=env | extra)
            r = probe(); self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(json.loads(r.stdout).get('sealed'))
            self.assertEqual(list(cache.iterdir()), [])
            r = probe({'TOTK_SHADER_SEED_IN_PLACE': '0'}); self.assertEqual(r.returncode, 0, r.stderr)
            self.assertGreater(json.loads(r.stdout)['imported_files'], 0)
            shutil.rmtree(cache); cache.mkdir()
            (app/'Contents/_CodeSignature/CodeResources').unlink()   # not signed: checked and imported
            r = probe(); self.assertEqual(r.returncode, 0, r.stderr)
            self.assertFalse(json.loads(r.stdout).get('sealed'))
            self.assertGreater(json.loads(r.stdout)['imported_files'], 0)

    def test_a_cache_with_files_is_merged_file_by_file(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            (cache/'unrelated.txt').write_text('keep')
            r = self.run_probe(source, cache)
            self.assertFalse(r.get('cloned_folder', False))
            self.assertEqual((cache/'unrelated.txt').read_text(), 'keep')

    def test_reprepared_seed_with_the_same_files_is_not_checked_again(self):
        # Packaging again writes a new preparation time and script hash into the manifest; the files stay the same.
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            self.run_probe(source, cache)
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            m['seconds'] = 12.5; m['preparation_sha256'] = 'f'*64
            p.write_text(json.dumps(m))
            (source/(variant+'_b.metal')).write_text('corrupt')   # only a check of the sources would notice
            self.assertEqual(self.run_probe(source, cache)['imported_files'], 0)

    def test_a_changed_file_table_is_checked_again(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            self.run_probe(source, cache)
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            extra = key+'-'+'e'*12+'_b.metal'
            (source/extra).write_bytes(b'// new shader')
            m['files'][extra] = hashlib.sha256(b'// new shader').hexdigest(); m['shaders'] = 2
            p.write_text(json.dumps(m))
            (source/(variant+'_b.metal')).write_text('corrupt')   # caught because every source is checked again
            self.run_probe(source, cache, good=False)

    def test_wrong_chain_corruption_and_path_traversal_are_rejected(self):
        for mutation in ['chain', 'hash', 'path']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as folder:
                source, cache, key, variant = self.fixture(Path(folder))
                p = source/'seed-manifest.json'; m = json.loads(p.read_text())
                if mutation == 'chain': m['chain'] = 'other-chain'
                elif mutation == 'hash': (source/(variant+'_b.metal')).write_text('corrupt')
                else: m['files']['../escape'] = '0'*64
                p.write_text(json.dumps(m))
                self.run_probe(source, cache, good=False)
                self.assertEqual(list(cache.iterdir()), [])

    def test_fill_packs_merge_without_touching_the_seed_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, cache, key, variant = self.fixture(root)
            self.run_probe(source, cache)
            receipt = (cache/'seed-installed.json').read_text()
            fills = root/'fills'; fills.mkdir()
            pack = fills/'20261018-1'; pack.mkdir()
            filled = key+'-'+'d'*12
            files = {filled+'_b.metal': b'// filled', filled+'_b.json': b'{}', filled+'_b.metallib': b'lib',
                     key+'.variants': json.dumps([dict(variant=filled, deps={'tex': {'1': 'cube'}})]).encode()}
            for name, data in files.items():
                (pack/name).write_bytes(data)
            (pack/'seed-manifest.json').write_text(json.dumps(dict(schema=1, chain=seed.CHAIN, shaders=1,
                files={n: hashlib.sha256(d).hexdigest() for n, d in files.items()})))
            r = self.run_probe(fills, cache, fill=True)
            self.assertEqual((r['packs'], r['rejected']), (1, []))
            self.assertEqual([v['variant'] for v in json.loads((cache/(key+'.variants')).read_text())], [variant, filled])
            self.assertEqual((cache/(filled+'_b.metallib')).read_bytes(), b'lib')
            self.assertEqual((cache/'seed-installed.json').read_text(), receipt)
            self.assertEqual(list(fills.iterdir()), [])

    def test_a_rebuilt_metallib_replaces_the_cached_one(self):
        # iOS metallibs once compiled with Metal's default fast math must be replaced by the safe-math rebuild; the
        # sources next to them are still kept as they are.
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            lib = variant+'_b.metallib'
            (cache/lib).write_bytes(b'fast-math lib'); (cache/(variant+'_b.metal')).write_bytes(b'// collected here')
            (source/lib).write_bytes(b'safe-math lib')
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            m['files'][lib] = hashlib.sha256(b'safe-math lib').hexdigest(); p.write_text(json.dumps(m))
            self.run_probe(source, cache)
            self.assertEqual((cache/lib).read_bytes(), b'safe-math lib')
            self.assertEqual((cache/(variant+'_b.metal')).read_bytes(), b'// collected here')

    def test_incomplete_fill_pack_is_dropped_without_changing_the_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, cache, key, variant = self.fixture(root)
            fills = root/'fills'
            shutil.copytree(source, fills/'partial')
            (fills/'partial'/(variant+'_b.json')).unlink()   # an interrupted push
            r = self.run_probe(fills, cache, fill=True)
            self.assertEqual((r['packs'], len(r['rejected'])), (0, 1))
            self.assertEqual(list(cache.iterdir()), [])
            self.assertEqual(list(fills.iterdir()), [])


if __name__ == '__main__':
    unittest.main()

"""Shader preparation uses local game containers; seed import preserves user variants."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

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


class ShaderInventory(unittest.TestCase):
    def test_the_key_hashes_the_control_sections_own_size_as_the_runtime_does(self):
        self.assertEqual(seed.control_key_len(bytes(0x880)), 0x880)              # no size word: the NVN 55 section
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x88c)+bytes(0x860)), 0x880)   # 0x88c: hashed as 0x880
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x7d1)+bytes(0x860)), 0x7d1)
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x848)+bytes(0x860)), 0x848)
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

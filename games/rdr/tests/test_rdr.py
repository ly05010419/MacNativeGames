"""Red Dead Redemption on the Zelda-line runtime: the pieces this port added that run without the game.

- the rewriter's leading patch area (games/dread's layout, third_party/d2r/patch_nso.py): a module whose .bss puts a
  patch area after the image out of branch reach gets it before the module; a small module keeps the old layout
  (tests/synthetic_nso.py builds both, no game code);
- scripts/merge_card_romfs.py: the base RomFS section read straight from the card (CardBase) and the RomFS section's
  FS header check;
- the host answers this game needed (the SDK 15.3.2 forms) and the loader's rtld symbol order;
- that the port carries no nxnative code and is built on the Zelda line (games/mk8dx's runtime and shared/);
- the shader seed's reader (scripts/rpf6.py: RPF6's Zstandard files, RAGE's NVN program records) and the seed tool's
  game side (scripts/prepare_rdr_shader_seed.py), on synthetic archives and caches (no game data)."""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE / 'tests'))
import synthetic_nso  # noqa: E402


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rewriter_deps():
    try:
        import capstone  # noqa: F401
        import lz4.block  # noqa: F401
        return True
    except ImportError:
        return False


def fixups(folder, name):
    out = {}
    for line in (folder / f'{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        out[key] = int(value, 0)
    return out


def branch_target(word, at):
    assert word >> 26 == 0x05, f'not a B at {at:#x}: {word:#010x}'
    imm = word & 0x3ffffff
    return at + ((imm - (1 << 26)) if imm >> 25 else imm) * 4


@unittest.skipUnless(rewriter_deps(), 'capstone and lz4 (the rewriter\'s modules)')
class LeadingPatchArea(unittest.TestCase):
    def rewrite(self, bss):
        tmp = Path(self.tmp.name)
        src, out = tmp / f'nso-{bss:x}', tmp / f'out-{bss:x}'
        src.write_bytes(synthetic_nso.build(bss))
        out.mkdir()
        subprocess.run([sys.executable, str(HERE / 'third_party/d2r/patch_nso.py'), str(src), str(out), 'main'], check=True,
                       capture_output=True, cwd=HERE / 'third_party/d2r', env={**os.environ, 'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'})
        return out, fixups(out, 'main'), (out / 'main.patched.bin').read_bytes()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_large_bss_puts_the_patch_area_before_the_module(self):
        bss = 0xC000000   # 192 MiB: an after-image patch area would be out of B's 128 MiB reach
        _, f, data = self.rewrite(bss)
        lead = f['PATCH_LEAD']
        self.assertEqual(f['PATCH_OFF'], -lead)
        self.assertGreater(lead, 0)
        self.assertEqual(f['IMAGE_SIZE'], f['KEY_PAGE_OFF'] + 0x4000)
        self.assertGreaterEqual(f['KEY_PAGE_OFF'], synthetic_nso.DATA + 0x1000 + bss)
        # the file: the patch area, then the module up to the end of data only (.bss and the key page are zero fill)
        self.assertEqual(len(data), lead + 0x4000 * -(-(synthetic_nso.DATA + 0x1000) // 0x4000))
        self.assertLessEqual(f['PATCH_SIZE'], lead)
        self.assertTrue(any(data[:f['PATCH_SIZE']]))
        self.assertFalse(any(data[f['PATCH_SIZE']:lead]))
        # the rewritten site branches back into the patch area, inside B's reach
        site = synthetic_nso.FUNC
        target = branch_target(struct.unpack_from('<I', data, lead + site)[0], site)
        self.assertTrue(-lead <= target < -lead + f['PATCH_SIZE'], hex(target))
        # the module's other bytes are the original ones
        original = synthetic_nso.build(bss)
        self.assertEqual(data[lead + synthetic_nso.RO:lead + synthetic_nso.RO + 0x1000], original[0x1100:0x2100])

    def test_small_bss_keeps_the_patch_area_after_the_image(self):
        _, f, data = self.rewrite(0x100000)
        self.assertEqual(f['PATCH_LEAD'], 0)
        self.assertGreater(f['PATCH_OFF'], f['KEY_PAGE_OFF'])
        self.assertEqual(len(data), f['PATCH_OFF'] + f['PATCH_SIZE'])
        site = synthetic_nso.FUNC
        target = branch_target(struct.unpack_from('<I', data, site)[0], site)
        self.assertTrue(f['PATCH_OFF'] <= target < f['PATCH_OFF'] + f['PATCH_SIZE'], hex(target))

    def test_prepare_runtime_and_loader_know_the_layout(self):
        prepare = (HERE / 'scripts/prepare_runtime.py').read_text()
        self.assertIn("image = data[layout['PATCH_LEAD']:]", prepare)
        self.assertIn('.patch_lead={l["PATCH_LEAD"]}ULL', prepare)
        loader = (HERE / 'runtime/loader.c').read_text()
        self.assertIn('m->base=reserved+m->patch_lead;', loader)
        self.assertIn('fseeko(f,(off_t)m->patch_lead,SEEK_SET)', loader)
        self.assertIn('int64_t patch_off; uint64_t patch_lead, patch_size, image_size;', (HERE / 'runtime/runtime.h').read_text())


class MergeCardRomfs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.merge = load(HERE / 'scripts/merge_card_romfs.py', 'merge_card_romfs')
        except ImportError as e:
            raise unittest.SkipTest(f'games/pla/scripts needs {e.name}')

    def test_card_base_reads_the_base_section_with_its_iv(self):
        calls = []

        class Ctr:
            def read(self, absolute, size, upper):
                calls.append((absolute, size, upper))
                return bytes(size)

        fs = bytearray(0x200)
        fs[3], fs[4] = 3, 3
        fs[0x140:0x148] = bytes(range(8))
        base = self.merge.CardBase(Ctr(), 0x4000, 0x10000, bytes(fs))
        self.assertEqual(base.read(0x100, 0x20), bytes(0x20))
        self.assertEqual(calls, [(0x4100, 0x20, bytes(range(8))[::-1])])
        with self.assertRaises(ValueError):
            base.read(0xfff0, 0x20)

    def test_card_base_refuses_a_patch_section(self):
        fs = bytearray(0x200)
        fs[3], fs[4] = 3, 4   # AES-CTR-Ex: an update's BKTR section, not a base
        with self.assertRaises(ValueError):
            self.merge.CardBase(None, 0, 0x1000, bytes(fs))

    def test_romfs_section_checks_its_fs_header(self):
        hdr = bytearray(0xc00)
        fs = bytearray(0x200)
        fs[3] = 3
        struct.pack_into('<II', hdr, 0x240 + 16, 0x10, 0x20)        # section 1
        hdr[0x400 + 512:0x600 + 512] = fs
        hdr[0x280 + 32:0x2a0 + 32] = hashlib.sha256(fs).digest()
        start, end, got = self.merge.romfs_section(bytes(hdr))
        self.assertEqual((start, end, got), (0x10 * 512, 0x20 * 512, bytes(fs)))
        hdr[0x280 + 32] ^= 1
        with self.assertRaises(ValueError):
            self.merge.romfs_section(bytes(hdr))


class HostAnswers(unittest.TestCase):
    def test_sdk15_entry_points(self):
        loader = (HERE / 'runtime/loader.c').read_text()
        self.assertIn('"_ZN2nn4init5StartEmmPFvvES2_S2_"', loader)   # SDK 15.3.2's five-argument nn::init::Start
        fs = (HERE / 'runtime/fs_host.c').read_text()
        for name in ('_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE', '_ZN2nn2fs32EnableDefaultGlobalFileDataCacheEPvm',
                     '_ZN2nn2fs15DeleteDirectoryEPKc', '_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE',
                     '_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE'):
            self.assertIn(f'"{name}"', fs)
        front = (HERE / 'runtime/frontend.m').read_text()
        for name in ('_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii', '_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii',
                     '_ZN2nn2vi12DestroyLayerEPNS0_5LayerE', '_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE', '_ZN2nn2vi8FinalizeEv'):
            self.assertIn(f'"{name}"', front)
        applet = (HERE / 'runtime/applet_host.c').read_text()
        for name in ('_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_', '_ZN2nn2oe14RestartProgramEPKvm',
                     '_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE'):
            self.assertIn(f'"{name}"', applet)
        nvn = (HERE / 'third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text()
        self.assertIn('NVN(nvnSamplerPoolRegisterSamplerBuilder,', nvn)

    def test_rtld_symbol_order(self):
        # A global definition binds to its first definition in load order (main, then sdk), so main's operator new /
        # delete and malloc / free interpose on the SDK's own; a local symbol stays its module's.
        loader = (HERE / 'runtime/loader.c').read_text()
        first = loader.index('if(!p&&s->defined&&s->binding!=0)p=runtime_symbol(s->name);')
        own = loader.index('if(!p)p=s->defined?(s->value<m->key_off?m->base+s->value:NULL):runtime_symbol(s->name);')
        self.assertLess(first, own)

    def test_no_system_resource(self):
        self.assertIn('#define SYSTEM_RESOURCE_BUDGET 0ULL', (HERE / 'runtime/services.c').read_text())


class ZeldaLine(unittest.TestCase):
    def test_no_nxnative_code(self):
        self.assertFalse((HERE / 'third_party/nxnative').exists())
        for f in HERE.rglob('*'):
            if f.is_symlink():
                self.assertNotIn('nxnative', str(f.readlink()), f)
                self.assertNotIn('games/mk8d/', str(f.resolve()), f)

    def test_runtime_links_the_shared_files_mk8dx_links(self):
        mk = ROOT / 'games/mk8dx/runtime'
        for f in mk.iterdir():
            if f.is_symlink() and 'shared/runtime' in str(f.readlink()) and f.name != 'movie_pace.h':
                mine = HERE / 'runtime' / f.name
                self.assertTrue(mine.is_symlink() and mine.resolve() == f.resolve(), f.name)
        self.assertEqual((HERE / 'runtime/offline_host.c').resolve(), (ROOT / 'shared/runtime/offline_host.c').resolve())
        self.assertIn('defined(GAME_RDR)', (ROOT / 'shared/runtime/offline_host.c').read_text())

    def test_no_movie_decoder(self):
        # main decodes its Bink movies itself and imports no nn::movie: no host movie decoder is built
        self.assertFalse((HERE / 'runtime/movie_host.c').exists())
        self.assertNotIn('vpx', (HERE / 'Makefile').read_text())

    def test_game_switch(self):
        self.assertIn('#define GAME_RDR 1', (HERE / 'runtime/game_version.h').read_text())
        self.assertIn('-DGAME_RDR=1', (HERE / 'Makefile').read_text())
        self.assertIn('mng_active_start(@"rdr"', (HERE / 'runtime/frontend.m').read_text())


def zstandard_available():
    try:
        import zstandard  # noqa: F401
        return True
    except ImportError:
        return False


def program(stage, body, key_len=0x880):
    """A RAGE program record (scripts/rpf6.py): magic, code size, control size, the code (0x30-byte header, magic
    0x12345678), the control section (magic 0x98761234; c1 at +0x700, the code length at +0x704, the stage at +0x714)."""
    code = struct.pack('<I', 0x12345678) + bytes(0x2c) + body
    control = bytearray(key_len + 0xc)   # sections longer than what the runtime hashes, as the game's are
    struct.pack_into('<I', control, 0, 0x98761234)
    struct.pack_into('<I', control, 0x20, key_len)
    struct.pack_into('<III', control, 0x700, 0x30, len(code), 0)
    struct.pack_into('<I', control, 0x714, stage)
    record = struct.pack('<III', 0x19866891, len(code) + 8, len(control)) + code + bytes(8) + bytes(control)
    key = hashlib.sha256(bytes(control[:key_len]) + code).hexdigest()
    return record, key


def archive(files, entries=None, pad=b''):
    """An RPF6 archive: the header (big-endian), an (opaque) entry table, the files as Zstandard frames on 8-byte
    boundaries, then a debug area."""
    import zstandard
    entries = entries or len(files) + 1
    data = bytearray(b'RPF6' + struct.pack('>III', entries, 0, 0xfffffffd) + os.urandom(entries * 20))
    while len(data) % 0x800: data.append(0)
    data += pad
    for f in files:
        while len(data) % 8: data.append(0)
        data += zstandard.ZstdCompressor().compress(f)
    while len(data) % 8: data.append(0)
    struct.pack_into('>I', data, 8, len(data) // 8)
    return bytes(data) + os.urandom(64)


@unittest.skipUnless(zstandard_available(), 'zstandard (the seed reader\'s module)')
class Rpf6Reader(unittest.TestCase):
    def setUp(self):
        self.rpf6 = load(HERE / 'scripts/rpf6.py', 'rdr_rpf6')

    def test_files_and_programs(self):
        vs, vs_key = program(0, b'\x01' * 0x40)
        ps, ps_key = program(1, b'\x02' * 0x80, key_len=0x7d1)
        decoy = struct.pack('<III', 0x19866891, 0x40, 0x880) + bytes(0x40)   # the magic without a record behind it
        effect = b'rgxd' + bytes(12) + vs + decoy + ps
        data = archive([b'a.fx\r\nb.fx\r\n', effect], pad=bytes(8))
        entries, debug, crypt = self.rpf6.header(data)
        self.assertEqual((entries, crypt), (3, 0xfffffffd))
        self.assertEqual([c for _, _, c in self.rpf6.files(data)], [b'a.fx\r\nb.fx\r\n', effect])
        self.assertEqual([e for _, e in self.rpf6.effects(data)], [effect])
        found = {k: (st, len(c), len(code)) for k, st, c, code in self.rpf6.programs(effect)}
        self.assertEqual(found, {vs_key: ('vs', 0x880, 0x70), ps_key: ('ps', 0x7d1, 0xb0)})

    def test_refusals(self):
        with self.assertRaises(self.rpf6.Rpf6Error):
            self.rpf6.header(b'RPF7' + bytes(60))
        with self.assertRaises(self.rpf6.Rpf6Error):   # more files than entries
            self.rpf6.files(archive([b'rgxd1', b'rgxd2', b'rgxd3'], entries=2))
        data = bytearray(archive([b'rgxd' + bytes(4000)]))
        cut = data.index(b'\x28\xb5\x2f\xfd') + 12
        struct.pack_into('>I', data, 8, cut // 8)   # the debug area starts inside the frame: truncated
        with self.assertRaises(self.rpf6.Rpf6Error):
            self.rpf6.files(bytes(data))

    def test_a_record_with_a_bad_stage_or_length_is_not_a_program(self):
        record, _ = program(3, b'\x01' * 0x40)   # a geometry stage: no translation chain
        self.assertEqual(list(self.rpf6.programs(record)), [])
        record, _ = program(0, b'\x01' * 0x40)
        bad = bytearray(record); struct.pack_into('<I', bad, 12 + 0x78 + 0x704, 0x1000)   # longer than the code
        self.assertEqual(list(self.rpf6.programs(bytes(bad))), [])


class SeedTool(unittest.TestCase):
    """scripts/prepare_rdr_shader_seed.py: Odyssey's seed tool (the Zelda line's) with this game's discovery, depth and
    rules; nothing of the tool copied."""

    def setUp(self):
        self.mod = load(HERE / 'scripts/prepare_rdr_shader_seed.py', 'rdr_seed')

    def test_runs_the_zelda_lines_tool(self):
        text = (HERE / 'scripts/prepare_rdr_shader_seed.py').read_text()
        self.assertIn("GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'", text, 'the tool is run, not copied')
        self.assertNotIn('def prepare(', text)
        with tempfile.TemporaryDirectory() as d:
            seed = self.mod.load_seed_tool(Path(d), Path(d) / 'rules.tsv')
            self.assertIs(seed.inventory, self.mod.inventory)
            self.assertIs(seed.translation_request, self.mod.translation_request)
            self.assertEqual(seed.CHAIN, 'totk-native-v4-unverified')

    def test_depth_is_zero_to_one(self):
        control = bytearray(0x880); struct.pack_into('<II', control, 0x700, 0x30, 0x40)
        with tempfile.TemporaryDirectory() as d:
            request = self.mod.translation_request('k-v', 'vs', bytes(control), bytes(range(0x40)), {'attr': {}}, Path(d) / 'c')
            self.assertIs(request['depth_minus_w'], False)
            self.assertEqual((Path(d) / 'c').read_bytes(), bytes(range(0x30, 0x40)))
            out = Path(d) / 'seed'; out.mkdir()
            v = 'a' * 64 + '-' + 'b' * 12
            (out / f'{"a" * 64}.variants').write_text(json.dumps([{'variant': v, 'deps': {}}]))
            (out / f'{v}_b.json').write_text(json.dumps({'stage': 'vs'})); (out / f'{v}_b.metal').write_text('m')
            (out / 'shader-keys.json').write_text('{}')
            (out / 'seed-manifest.json').write_text(json.dumps({'chain': 'c', 'files': {}}))
            self.assertEqual(self.mod.finish(out, 'c'), 1)
            self.assertIs(json.loads((out / f'{"a" * 64}.variants').read_text())[0]['depth_minus_w'], False)
            self.assertFalse((out / 'shader-keys.json').exists(), 'the importer refuses it')
            self.assertEqual(set(json.loads((out / 'seed-manifest.json').read_text())['files']),
                             {f'{"a" * 64}.variants', f'{v}_b.json', f'{v}_b.metal'})

    def test_rules_are_the_macs_vertex_layouts(self):
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            rows = {'vs': [{'variant': 'a-1', 'deps': {'attr': {'0': 'float', '1': 'uint', '2': 'float'}}},
                           {'variant': 'a-2', 'deps': {'attr': {}}}],
                    'ps': [{'variant': 'b-1', 'deps': {'attr': {'0': 'uint'}}}]}
            for i, (stage, rs) in enumerate(rows.items()):
                (cache / f'{i}.variants').write_text(json.dumps(rs))
                for r in rs:
                    (cache / f'{r["variant"]}_b.json').write_text(json.dumps({'stage': stage}))
            rules = self.mod.rules_from_cache(cache)
            self.assertEqual(rules, ['{"attr":{"0":"float","1":"uint","2":"float"}}'])
            listed = cache / 'rules.tsv'
            shaders = {'c' * 64: ('vs', b'', b'', 's'), 'd' * 64: ('ps', b'', b'', 's')}
            self.assertEqual(self.mod.write_rules(listed, rules, shaders), 1)
            sys.path.insert(0, str(ROOT / 'games/odyssey/third_party/totk-scripts'))
            from shader_list import ShaderList
            self.assertEqual(ShaderList.load(listed).variants, {('c' * 64, rules[0])}, 'rows the tool reads')

    @unittest.skipUnless(zstandard_available(), 'zstandard')
    def test_inventory_reads_the_archive_and_the_modules(self):
        vs, vs_key = program(0, b'\x01' * 0x40)
        ps, ps_key = program(1, b'\x02' * 0x40)
        with tempfile.TemporaryDirectory() as d:
            romfs, modules = Path(d, 'romfs'), Path(d, 'modules')
            romfs.mkdir(); modules.mkdir()
            data = archive([b'rgxd' + vs])
            (romfs / 'romfs.bin').write_bytes(bytes(16) + data)
            (romfs / 'index.json').write_text(json.dumps({'status': 'pass', 'files': [
                {'path': 'shaders.rpf', 'offset': 16, 'size': len(data)}]}))
            (modules / 'main.patched.bin').write_bytes(bytes(64) + ps + vs)
            self.mod.MIN_SHADERS = 2
            _, shaders, startup, archives = self.mod.inventory(romfs, modules)
            self.assertEqual({k: v[3].split('@')[0] for k, v in shaders.items()}, {vs_key: 'shaders.rpf', ps_key: 'module:main'})
            self.assertEqual(startup, set(shaders), 'every shader gets its default variant')
            self.assertEqual(archives, [{'path': 'shaders.rpf', 'sha256': hashlib.sha256(data).hexdigest()}])
            self.mod.MIN_SHADERS = 3
            with self.assertRaises(ValueError):
                self.mod.inventory(romfs, modules)


if __name__ == '__main__':
    unittest.main()

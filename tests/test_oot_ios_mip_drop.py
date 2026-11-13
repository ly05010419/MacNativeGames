"""Ocarina of Time on iOS may leave out a large texture's top mip level (user 2026-11-13: the one exception to the full-mip
rule, iOS only, never on the Mac; nxnative #2270's rule: Metal working set under 6 GiB). Only sampling may see the smaller
copy: a render target, a copy's source or destination, a clear or an image names the game's own level numbers, so each of
those makes the texture whole (tex_whole) before it takes the Metal texture. Every other game keeps full mip chains.
Since the graphics layer unification (d763c68e) games/oot's layer is games/totk's: the OOT-only parts sit under GAME_OOT."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'   # games/oot/third_party/d2r_gfx links here
OOT = NVN / 'nvn_mtl.m'


def function_body(src, signature):
    """The text of the function whose definition starts with `signature`, up to its closing brace at column 0."""
    start = src.index(signature)
    end = src.index('\n}\n', start)
    return src[start:end]


def mip_drop_branches(src):
    """The four branches of mip_drop_of: OOT on iOS, other games on iOS, OOT on the Mac, other games on the Mac."""
    body = function_body(src, 'static unsigned mip_drop_of(const tex_t *t) {')
    heads = ['#if TARGET_OS_IPHONE && defined(GAME_OOT)', '#elif TARGET_OS_IPHONE', '#elif defined(GAME_OOT)', '#else']
    at = [body.index(h) for h in heads]
    assert at == sorted(at), 'branches in order'
    return [body[a:b] for a, b in zip(at, at[1:] + [body.index('#endif', at[-1])])]


class OotIosMipDrop(unittest.TestCase):
    src = OOT.read_text()

    def test_the_mac_never_leaves_a_level_out(self):
        _, _, mac, _ = mip_drop_branches(self.src)
        self.assertIn('return 0;', mac)
        self.assertNotIn('getenv', mac, 'no switch may turn the drop on on the Mac')
        self.assertNotIn('return 1', mac)

    def test_ios_follows_the_working_set_rule(self):
        ios, _, _, _ = mip_drop_branches(self.src)
        self.assertIn('recommendedMaxWorkingSetSize', ios)
        self.assertIn('6ull << 30', ios)
        self.assertIn('TOTK_MIP_DROP', ios)

    def test_every_use_but_sampling_makes_the_texture_whole_first(self):
        for signature in ('static id<MTLTexture> attachment_texture(const tex_t *t, const view_t *v, int has_v) {',
                          'static void copy_t2b(const p_copy_b2t *c) {',
                          'static void copy_b2t(const p_copy_b2t *c) {',
                          'static void copy_t2t(const p_copy_t2t *c) {',
                          'static void clear_tex(const p_clear_tex *c) {'):
            with self.subTest(signature):
                body = function_body(self.src, signature)
                take = re.search(r'\btex_mtl\(|BOTW_ATTACH_TEX_U\(', body)
                self.assertIsNotNone(take, 'the function takes its Metal texture')
                whole = body.find('tex_whole(')
                self.assertNotEqual(whole, -1, 'makes the texture whole')
                self.assertLess(whole, take.start(), 'before it takes the Metal texture')
        self.assertIn('tex_whole(c->src, "a copy source"); tex_whole(c->dst, "a copy destination");', self.src)

    def test_images_make_the_texture_whole_first(self):
        self.assertRegex(self.src, r'tex_whole\(&image,"an image"\);[^\n]*\n\s*id<MTLTexture> base=tex_mtl\(&image\)')

    def test_integer_texel_reads_share_the_path(self):
        self.assertIn('return tex_whole(&g_treg[tid].t, "an integer-texel read");',
                      function_body(self.src, 'static int tex_keep_full(int tid) {'))

    def test_shader_code_pools_stay_out_of_the_raw_pool_table(self):
        """nxnative 1c92e400 (#1971): a shader-code pool's own address does not put it into the raw pool table (which
        makes every pool in it resident and, on iOS, wires its pages); a buffer made in the pool still does."""
        hle = (NVN / 'hle_nvn.c').read_text()
        get = function_body(hle, 'NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) {')
        self.assertIn('NVN_MEMORY_POOL_FLAGS_SHADER_CODE|NVN_MEMORY_POOL_FLAGS_VIRTUAL', get)
        self.assertIn('mtl_pool_code_address(o->mem,o->size);', get)
        self.assertLess(get.find('#if defined(GAME_OOT)'), get.find('mtl_pool_code_address('), 'Ocarina of Time only for now')
        self.assertIn('pool_mark_addressed(o);', get.split('#endif', 1)[1], 'every other game marks every pool')
        self.assertRegex(hle, r'NVN\(nvnBufferGetAddress[^\n]*pool_mark_addressed')
        code = function_body(self.src, 'void mtl_pool_code_address(const void *mem, uint64_t size) {')
        self.assertNotIn('mtl_pool_addressed(', code)
        self.assertNotIn('g_addressed', code.replace('g_code_addressed', ''))

    def test_ios_wires_file_pages_before_the_residency_commit(self):
        """nxnative 2261a388: a shared buffer over file pages (the guest's memory in files on iOS) is mlocked before it
        joins the set and given back after the commit, so the commit never pages files in from the GPU driver's path."""
        add = function_body(self.src, 'static void res_add_at(id<MTLResource> r, int site) {')
        wire, join = add.find('file_wire(r)'), add.find('addAllocation:')
        self.assertNotEqual(wire, -1)
        self.assertLess(wire, join, 'wired before it joins the set')
        self.assertIn('#if NVN_FILE_WIRE', add[:wire])
        self.assertIn('#if TARGET_OS_IPHONE && defined(GAME_OOT)\n#define NVN_FILE_WIRE 1', self.src)
        commit = function_body(self.src, 'static void res_commit(void) {')
        self.assertLess(commit.find('[g_res commit]'), commit.find('munlock('), 'given back only after the commit')
        self.assertIn('mlock((void *)lo, hi - lo)', function_body(self.src, 'static void file_wire(id<MTLResource> r) {'))

    def test_ios_eviction_follows_what_the_residency_set_holds(self):
        """The app's allowance cannot see file pages wired by residency (the iPad's IOGPUFamily panic, 2026-11-10): on iOS
        the texture eviction also runs once the set passes 85 % of Metal's working set; the Mac never asks."""
        cap = function_body(self.src, 'static uint64_t res_over_working_set(void) {')
        ios, mac = cap.split('#else', 1)
        self.assertIn('#if TARGET_OS_IPHONE && defined(GAME_OOT)', ios)
        self.assertIn('recommendedMaxWorkingSetSize', ios)
        self.assertIn('g_res_live_bytes', ios)
        self.assertIn('return 0;', mac)
        sweep = function_body(self.src, 'static void pkg_evict_sweep(void) {')
        self.assertIn('res_over_working_set()', sweep)

    def test_ios_keeps_few_pipeline_archives_open(self):
        """The iPad ran out of vnodes with one open archive per pipeline (jetsam vnode-limit, 2026-11-13): on iOS
        Ocarina of Time keeps at most TOTK_PAR_OPEN_MAX archive files open, least recently used closed first."""
        par = (NVN / 'totk_pipeline_archive.h').read_text()
        self.assertIn('#if TARGET_OS_IPHONE && defined(GAME_OOT)\n/* Ocarina of Time on iOS: at most TOTK_PAR_OPEN_MAX', par)
        note = function_body(par, 'static void par_open_note(NSString *name) {')
        self.assertIn('[g_par_open removeObjectForKey:old];', note)
        get = function_body(par, 'static id<MTLBinaryArchive> par_archive_for(id<MTLDevice> dev, NSString *key) {')
        self.assertIn('if (a) { g_par_open[name] = a; PAR_OPEN_NOTE(name); }', get)
        self.assertIn('if (a) PAR_OPEN_NOTE(name);', get)

    def test_ios_warm_up_uses_several_threads(self):
        warm = function_body(self.src, 'static int warm_threads(void) {')
        oot = warm[warm.index('#if TARGET_OS_IPHONE && defined(GAME_OOT)'):warm.index('#elif TARGET_OS_IPHONE')]
        self.assertIn('activeProcessorCount - 2; if (d > 4) d = 4;', oot)
        self.assertIn('int d = 1;', warm[warm.index('#elif TARGET_OS_IPHONE'):], 'every other game keeps one thread on iOS')

    def test_other_games_keep_their_mip_behaviour(self):
        _, ios, _, mac = mip_drop_branches(self.src)
        self.assertIn('return 0;', ios)
        self.assertNotIn('return 1', ios, 'every other game keeps complete mip chains on iOS')
        self.assertIn('on = e ? atoi(e) != 0 : 0;', mac, 'and on the Mac only the TOTK_MIP_DROP=1 diagnostic')


if __name__ == '__main__':
    unittest.main()

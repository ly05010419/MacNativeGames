"""Compute dispatches ordered by the game's own barriers (port of nxnative 547c5db6, PR #2250, MIT): the shared graphics
layer's nvn_dispatch_order.h and its wiring in nvn_mtl.m / hle_nvn.c.

Source checks pin where the rule is fed (game barriers, event waits, command lists, host operations, encoder opens) and
the per-game default. The GPU probe (tests/dispatch_barrier_gpu_probe.m) runs the production header and cs_rangeless on a
real Metal 4 compute encoder in both modes, without a game; its negative controls take one rule out of the production
source and require the probe to fail."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
HEADER = NVN / 'nvn_dispatch_order.h'
PROBE = ROOT / 'tests/dispatch_barrier_gpu_probe.m'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class Wiring(unittest.TestCase):
    def setUp(self):
        self.mtl = (NVN / 'nvn_mtl.m').read_text(errors='replace')
        self.hle = (NVN / 'hle_nvn.c').read_text(errors='replace')
        self.header = HEADER.read_text()

    def test_upstream_attribution(self):
        self.assertIn('nxnative 547c5db6', self.header)
        self.assertIn('MIT', self.header)

    def test_default_is_ocarina_of_time_on_the_mac(self):
        self.assertRegex(self.header, r'#if defined\(GAME_OOT\) && !TARGET_OS_IPHONE\n#define DSP_BARRIER_DEFAULT 1\n#else\n#define DSP_BARRIER_DEFAULT 0')
        body = function(self.header, 'static int dbar_game(void) {')
        self.assertIn('getenv("DM_NVN_DISPATCH_BARRIER")', body)
        self.assertIn('"game"', body); self.assertIn('"full"', body)
        self.assertIn('MLOG(', body)   # the mode is logged once

    def test_shared_encoder_opens_only_where_the_rule_sees_it(self):
        # blit_kind (host operations) and disp_enc (dispatches) are the only places that open g_blit
        self.assertEqual(self.mtl.count('g_blit = enc_b_begin(g_cb)'), 2)
        start = self.mtl.index('static id<MTL4ComputeCommandEncoder> blit_kind(int buffer_alias) {')
        blit = self.mtl[start:self.mtl.index('static id<MTL4ComputeCommandEncoder> blit(void)', start)]
        self.assertIn('dsp_note_host_op();', blit)
        disp = function(self.mtl, 'static id<MTL4ComputeCommandEncoder> disp_enc(int rangeless) {')
        self.assertIn('dsp_note_encoder_open();', disp)
        self.assertRegex(disp, r'if \(dsp_barrier_before\(open, rangeless\) && !g_disp_probe_nobar\)[^\n]*\n\s*\[g_blit barrierAfterEncoderStages')
        self.assertEqual(disp.count('barrierAfterEncoderStages'), 1, 'no other barrier before a dispatch in disp_enc')

    def test_ordering_points_mark_the_next_dispatch(self):
        native = function(self.mtl, 'static void oot_bar_native(uint32_t bits) {')
        self.assertLess(native.index('if (!bits) return;'), native.index('dsp_note_game_barrier();'))
        wait = self.mtl[self.mtl.index('case OP_WAIT_EVENT: { volatile uint32_t *val;'):]
        self.assertIn('dsp_note_game_barrier();', wait[:wait.index('qwait_check(')])
        exec_ = self.mtl[self.mtl.index('void mtl_exec(const uint8_t *p) {'):]
        self.assertIn('if (!g_ret_n) dsp_note_game_barrier();', exec_[:exec_.index('for (;;)')])

    def test_dispatch_classifies_and_guards_late_host_operations(self):
        body = self.mtl[self.mtl.index('static void dispatch(const p_dispatch *dp) {'):]
        body = body[:body.index('dp_time_end(')]
        self.assertIn('int rangeless = cs_rangeless(cs);', body)
        self.assertIn('ce = disp_enc(rangeless); host_ops = g_dsp_host_ops;', body)
        self.assertRegex(body, r'if \(reuse && g_dsp_host_ops != host_ops && ce == \(id\)g_blit && !g_disp_probe_nobar\)')
        self.assertLess(body.index('g_dsp_host_ops != host_ops'), body.index('int dispatch_sample = dp_time_begin('))   # before the dispatch command

    def test_rangeless_bindings_follow_upstream(self):
        body = function(self.mtl, 'static int cs_rangeless(const NvShader *cs) {')
        self.assertIn('case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_SBUFP:', body)
        self.assertIn('case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2: break;', body)
        self.assertIn('default: return 1;', body)

    def test_counters_on_the_periodic_line(self):
        self.assertIn('void mtl_dispatch_barrier_stats(uint64_t v[5])', self.mtl)
        self.assertIn('mtl_dispatch_barrier_stats(v);', self.hle)
        self.assertIn('[nvn] compute dispatches up to frame', self.hle)

    def test_runtime_probe_orders_its_dependent_dispatches(self):
        probe = function(self.mtl, 'int mtl_probe_dispatch_chain(void) {')
        self.assertLess(probe.index('dsp_note_game_barrier();'), probe.index('dispatch(&dp);'))


def push_bank():
    return re.search(r'#define NVN_PUSH_BANK (\d+)', (NVN / 'nvn_obj.h').read_text()).group(1)


def defaults(defines, target):
    """DSP_BARRIER_DEFAULT as the header sets it for one configuration (preprocessor only)."""
    sdk = 'iphoneos' if 'ios' in target else 'macosx'
    root = subprocess.run(['xcrun', '--sdk', sdk, '--show-sdk-path'], capture_output=True, text=True)
    if root.returncode: return None
    src = '#define MLOG(...)\n#include "nvn_dispatch_order.h"\nint value = DSP_BARRIER_DEFAULT;\n'
    r = subprocess.run(['xcrun', 'clang', '-E', '-x', 'c', '-target', target, '-isysroot', root.stdout.strip(), *defines, f'-I{NVN}', '-'],
                       input=src, capture_output=True, text=True)
    if r.returncode: raise AssertionError(r.stderr[-2000:])
    return int(re.search(r'int value = (\d+);', r.stdout).group(1))


@unittest.skipUnless(sys.platform == 'darwin', 'Apple toolchain')
class Defaults(unittest.TestCase):
    def test_mac_per_game(self):
        mac = 'arm64-apple-macos26'
        self.assertEqual(defaults(['-DGAME_OOT=1', '-DGAME_NS2=1'], mac), 1)
        for cfg in ([], ['-DTOTK_GAME_V143=1'], ['-DGAME_BOTW=1'], ['-DGAME_DKB=1', '-DGAME_NS2=1'], ['-DGAME_POKOPIA=1', '-DGAME_NS2=1']):
            self.assertEqual(defaults(cfg, mac), 0, cfg)

    def test_ios_keeps_the_full_order(self):
        ios = defaults(['-DGAME_OOT=1', '-DGAME_NS2=1'], 'arm64-apple-ios26')
        if ios is None: self.skipTest('no iOS SDK')
        self.assertEqual(ios, 0)


@unittest.skipUnless(sys.platform == 'darwin', 'Metal requires macOS')
class GpuProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mtl = (NVN / 'nvn_mtl.m').read_text(errors='replace')
        cls.header = HEADER.read_text()

    def build(self, folder, header=None, mtl=None):
        folder = Path(folder)
        (folder / 'nvn_dispatch_order.h').write_text(header or self.header)
        src = PROBE.read_text()
        self.assertEqual(src.count('/* CS_RANGELESS */'), 1)
        src = src.replace('/* CS_RANGELESS */', function(mtl or self.mtl, 'static int cs_rangeless(const NvShader *cs) {'))
        (folder / 'probe.m').write_text(src)
        exe = folder / 'probe'
        r = subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-std=gnu11', '-O1', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', f'-DNVN_PUSH_BANK={push_bank()}', f'-I{folder}', f'-I{NVN}', str(folder / 'probe.m'),
                            '-framework', 'Foundation', '-framework', 'Metal', '-o', str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return exe

    def run_probe(self, exe, mode, **env):
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env={**os.environ, 'DM_NVN_DISPATCH_BARRIER': mode, **env})
        if r.returncode == 77: self.skipTest('no Metal 4 device in this session')
        return r

    def test_both_modes(self):
        with tempfile.TemporaryDirectory() as d:
            exe = self.build(d)
            for mode in ('game', 'full'):
                r = self.run_probe(exe, mode)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertRegex(r.stdout, rf'dispatch barrier probe \({mode}\): \d+ checks, 0 failed')
                self.assertIn(f'DM_NVN_DISPATCH_BARRIER={mode}', r.stderr)   # the mode line
                print(r.stdout.strip())

    def negative(self, label, header=None, mtl=None):
        with tempfile.TemporaryDirectory() as d:
            r = self.run_probe(self.build(d, header=header, mtl=mtl), 'game')
            self.assertNotEqual(r.returncode, 0, 'the probe passed without the rule')
            self.assertIn(label, r.stderr, r.stdout + r.stderr)

    def test_without_the_game_barrier_the_chain_has_no_barrier(self):
        old = 'bar = open && (g_dsp_game_bar || g_dsp_unknown);'
        self.assertEqual(self.header.count(old), 1)
        self.negative('a barrier before every dispatch the game ordered', header=self.header.replace(old, 'bar = open && g_dsp_unknown;'))

    def test_without_host_operations_a_fill_is_not_ordered_before_its_reader(self):
        old = 'static void dsp_note_host_op(void) { g_dsp_unknown = 1; g_dsp_host_ops++; }'
        self.assertEqual(self.header.count(old), 1)
        self.negative('a barrier before the fill and one between it and the dispatch', header=self.header.replace(old, 'static void dsp_note_host_op(void) { g_dsp_host_ops++; }'))

    def test_without_the_after_side_a_rangeless_dispatch_is_not_ordered_before_the_next(self):
        old = 'g_dsp_unknown |= rangeless;'
        self.assertEqual(self.header.count(old), 1)
        self.negative('the next one counted as after unknown work', header=self.header.replace(old, ''))

    def test_without_the_default_a_texture_binding_gets_a_range(self):
        body = function(self.mtl, 'static int cs_rangeless(const NvShader *cs) {')
        self.negative('has no range', mtl=self.mtl.replace(body, body.replace('default: return 1;', 'default: break;')))

    def test_race_demonstration(self):
        """Not asserted: whether dropped game barriers show as wrong values depends on the GPU's scheduling."""
        with tempfile.TemporaryDirectory() as d:
            r = self.run_probe(self.build(d), 'game', PROBE_RACE='1')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r'race \(game, game barriers dropped\): \d+ of 1048576 elements wrong, 0 barriers')
            print(r.stdout.strip())


if __name__ == '__main__':
    unittest.main()

"""Offline NS2 preparation must finish every listed state on the first invocation."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'macOS Foundation')
class SeedVariants(unittest.TestCase):
    def test_cold_listed_states_and_warm_reuse(self):
        mtl = (NVN / 'nvn_mtl.m').read_text()
        key = mtl[mtl.index('static void shader_key_of('):mtl.index('/* TOTK: prog_get answers')]
        local = mtl[mtl.index('static NSArray *rt_local(const uint8_t *control, int st) {'):mtl.index('/* A vertex attribute whose format')]
        depth = mtl[mtl.index('static void offline_depth_mode('):mtl.index('#if !TARGET_OS_IPHONE', mtl.index('static void offline_depth_mode('))]
        seed = mtl[mtl.index('void runtime_shader_seed('):]
        probe = r'''
#define totk_rt_pending seed_pending_real
#include "totk_shader_rt.m"
#undef totk_rt_pending
#define MLOG LOG
#define MDIE die
static int depth_mode;
int totk_depth_near_minus_w(void) { return depth_mode; }
void totk_depth_set_near_minus_w(int value) { depth_mode = value; }
uint64_t dm_nvn_frame_count(void) { return 0; }
void runtime_boot_status(const char *title, const char *detail) { (void)title; (void)detail; }
void runtime_boot_count(const char *title, uint64_t done, uint64_t total) { (void)title; (void)done; (void)total; }
// Keep background jobs queued until enumeration ends: no timing-dependent race.
int totk_rt_pending(void) {
    static int released;
    if (!released++) {
        printf("background jobs at drain: %d\n", seed_pending_real());
        atomic_store(&g_rt_urgent_busy, 0);
    }
    return seed_pending_real();
}
HELPERS
SEED
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    atomic_store(&g_rt_urgent_busy, 1);
    runtime_shader_seed(argv[1], argv[2]);
    return 0;
} }
'''.replace('HELPERS', key + local + depth).replace('SEED', seed)
        with tempfile.TemporaryDirectory(prefix='mng-ns2-seed-') as folder:
            root = Path(folder)
            (root / 'probe.m').write_text(probe)
            exe = root / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-DGAME_NS2=1',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-deprecated-declarations',
                '-I'+str(NVN), '-I'+str(NVN / 'include'),
                str(root / 'probe.m'), '-framework', 'Foundation', '-o', str(exe)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            # This service returns synthetic reflection; request production, queueing,
            # dependency matching and the on-disk cache are the actual runtime's.
            service = root / 'build/shader-service/totk_xlate'
            service.parent.mkdir(parents=True)
            service.write_text('#!' + sys.executable + '\n' + r'''
import json, pathlib, sys
out = pathlib.Path(sys.argv[sys.argv.index('--out') + 1])
for line in sys.stdin:
    q = json.loads(line)
    with open(out / 'requests.jsonl', 'a') as f:
        f.write(json.dumps(q) + '\n')
    if pathlib.Path(q['code']).read_bytes()[-1] == 255:
        print(json.dumps(dict(ok=False, error='synthetic unsupported shader')), flush=True)
        continue
    deps = dict(tex={'0': q['tex'].get('0', '2d')})
    (out / (q['variant'] + '_b.json')).write_text(json.dumps(dict(deps=deps)))
    (out / (q['variant'] + '_b.metal')).write_text('// synthetic source; no game code\n' * 4)
    print(json.dumps(dict(ok=True)), flush=True)
''')
            service.chmod(0o755)
            corpus = bytearray(b'NS2SHC01' + struct.pack('<I', 3))
            keys = []
            for tag in (1, 2, 255):
                control, code = bytearray(0xc0), bytearray(0x100)
                struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
                struct.pack_into('<I', control, 0x38, len(code))
                struct.pack_into('<II', control, 0x40, len(code), len(code))
                struct.pack_into('<I', code, 0, 1)  # synthetic vertex SPH
                code[-1] = tag
                keys.append(hashlib.sha256(control[:0x48] + code).hexdigest())
                corpus += struct.pack('<IIIH', 0, len(control), len(code), 0) + control + code
            (root / 'corpus.bin').write_bytes(corpus)
            rows = [(keys[0], '2d_depth'), (keys[0], 'cube'), (keys[2], '2d_depth')]
            (root / 'list.tsv').write_text(''.join('V\t' + key + '\t' + json.dumps(dict(tex={'0': kind})) + '\n'
                                                  for key, kind in rows))
            cache = root / 'cache'
            env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_NVN_'))}
            env.update(TOTK_NATIVE_DIR=str(root / 'native'), TOTK_SHADER_CACHE=str(cache),
                       TOTK_SHADER_SEED='off', TOTK_SHADER_SERVICE='native', TOTK_SEED_DEPTH_MODE='minus-w')
            for phase in ('cold', 'warm'):
                with self.subTest(phase=phase):
                    run = subprocess.run([str(exe), str(root / 'corpus.bin'), str(root / 'list.tsv')],
                        capture_output=True, text=True, env=env, timeout=30)
                    output = run.stdout + run.stderr
                    self.assertEqual(run.returncode, 0, output)
                    self.assertIn('2 of 3 shaders have a translation', output)
                    self.assertIn("bind states: 2 of 3 translated", output)
                    self.assertIn('synthetic unsupported shader', output)
                    if phase == 'cold':
                        self.assertIn('background jobs at drain: 3', output)   # every default runs in the background
                    index = json.loads((cache / (keys[0] + '.variants')).read_text())
                    self.assertEqual({r['deps']['tex']['0'] for r in index}, {'2d', '2d_depth', 'cube'})
                    requests = [json.loads(line) for line in (cache / 'requests.jsonl').read_text().splitlines()]
                    self.assertEqual(sum(q['variant'].startswith(keys[0]) for q in requests), 3)
                    self.assertEqual(sum(q['variant'].startswith(keys[1]) for q in requests), 1)
                    self.assertEqual(sum(q['variant'].startswith(keys[2]) for q in requests), 1 if phase == 'cold' else 2)


if __name__ == '__main__':
    unittest.main()

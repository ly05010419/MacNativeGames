"""A listed render pipeline never carries the host bits (nvn_pso_skey.h PSO_HOST_BITS: the VR host's own vertex function,
built in play, never archived or listed). 2026-11-13: Ocarina of Time's list holds 69 rows with a2c bit 6, recorded when the
bit meant nothing; on the shared layer the pipeline archive built them as VR host pipelines and stopped ("Labo VR vertex
function is unavailable"). pso_skey_parse refuses such a row; the other a2c bits still parse."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PROBE = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stddef.h>
@interface NvShader : NSObject { @public char key[80]; int stage; } @end   /* the fields pso_skey_from reads (nvn_mtl.m) */
@implementation NvShader @end
#include "nvn_pso_skey.h"
int main(void) {
    unsigned bits[] = {0, 1, 2, 8, 12, 16, 32, 64, 128, 64 | 8};
    for (size_t i = 0; i < sizeof bits / sizeof *bits; i++) {
        pso_skey k; memset(&k, 0, sizeof k); k.k.a2c = bits[i];
        char hex[sizeof k * 2 + 1]; pso_skey_hex(&k, hex);
        pso_skey back; int r = pso_skey_parse(hex, &back);
        printf("%u %d %u\n", bits[i], r, r ? 0u : back.k.a2c);
    }
    printf("offset %zu size %zu\n", offsetof(pso_skey, k) + offsetof(pso_key, a2c), sizeof(pso_skey));
    return 0;
}
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class HostBits(unittest.TestCase):
    def test_a_row_with_host_bits_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'p.m').write_text(PROBE)
            build = subprocess.run(['xcrun', 'clang', '-w', '-fobjc-arc', f'-I{NVN}', f'-I{NVN / "include"}', str(tmp / 'p.m'), '-framework', 'Foundation', '-o', str(tmp / 'p')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            lines = subprocess.run([str(tmp / 'p')], capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(lines[-1], 'offset 268 size 544')   # where tools read a2c in a listed row (the lists' hex keys)
        for line in lines[:-1]:
            bits, result, back = map(int, line.split())
            with self.subTest(a2c=bits):
                if bits & (64 | 128):
                    self.assertEqual(result, -1)
                else:
                    self.assertEqual((result, back), (0, bits))

    def test_host_pipelines_are_not_recorded(self):
        # nor written to pso_user.tsv / the contributions by a VR session (pso_adopt), nor their tile-read (T) rows
        text = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('if (k->a2c & (PSO_HDR | PSO_HOST_BITS)) { hm_put_id(&g_pso, k, ps); g_pso_gen++; return; }', text)
        self.assertIn('!(k.a2c & (PSO_VBORDER | PSO_HOST_BITS))', text)


if __name__ == '__main__':
    unittest.main()

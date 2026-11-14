"""Read the translator's emitted exports against the NVN block layout.

Synthetic instructions only. No game files, interpreter or GPU are needed.
The old interleaved export fails the multi-block cases below.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/oot/tests'))
from ampere_scoreboard_gpu import inst, mov


@unittest.skipUnless(sys.platform == 'darwin', 'NativeAOT translator is a Mac tool')
class MeshOutputLayout(unittest.TestCase):
    def test_translated_exports(self):
        binary = Path(os.environ.get('OOT_TEST_XLATE') or ROOT/'games/oot/prebuilt/macos-arm64/totk_xlate')
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            process = subprocess.Popen([str(binary), '--service', '--spirv-cross', '/unused',
                                        '--out', tmp, '--chain', 'mesh-layout-test'],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                for vertices, primitives, primitive_outputs in ((128, 64, True), (32, 32, True),
                                                               (128, 64, False), (64, 128, True)):
                    with self.subTest(vertices=vertices, primitives=primitives, outputs=primitive_outputs):
                        ctl, header, constants = bytearray(224), bytearray(128), bytearray(128)
                        struct.pack_into('<HH', ctl, 0x58, 32, 0)
                        struct.pack_into('<HH', ctl, 0x60, vertices, primitives)
                        ctl[0x5f] = 3
                        header[0x35], header[0x36] = 0xf0, 0xf  # position and attribute 0: 8 rows
                        if primitive_outputs:
                            ctl[0x68], constants[0x36] = 0x80, 0xf  # primitive attribute 0: 4 rows
                            struct.pack_into('<I', ctl, 0x40, 0x1000)
                            struct.pack_into('<I', ctl, 0x64, 0x1000)
                        key = f'layout-{vertices}-{primitives}-{int(primitive_outputs)}'
                        path = folder/(key+'.code')
                        path.write_bytes(header + mov(1, 0) + mov(2, 0) +
                                         inst(0x927, ra=1, b=2, c=0x8000) + inst(0x94d, c=0x03800000))
                        request = dict(variant=key, stage='ms', arch='sm86', code=str(path),
                                       mesh_ctl=ctl.hex(), c1=constants.hex(), depth_minus_w=True, stage_device=False)
                        process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
                        reply = json.loads(process.stdout.readline())
                        self.assertTrue(reply.get('ok'), reply)
                        text = (folder/(key+'_b.metal')).read_text()
                        exports = re.findall(r'uint b=(\d+)u\+\(n/32u\)\*(\d+)u\+n%32u;\s*dm_(vertex|prim)_t', text)
                        self.assertEqual(len(exports), 4 if primitive_outputs else 2)  # both depth conventions
                        blocks = (max(vertices, primitives)+31)//32
                        memory = [None]*4  # header, no task payload
                        for kind, count in [('vertex', 8), ('prim', 4 if primitive_outputs else 0)]:
                            for block in range(blocks):
                                for component in range(count):
                                    memory.extend((kind, block*32+lane, component) for lane in range(32))
                        for base, stride, kind in exports:
                            for n in range(vertices if kind == 'vertex' else primitives):
                                for component in range(8 if kind == 'vertex' else 4):
                                    address = int(base)+(n//32)*int(stride)+n%32+component*32
                                    self.assertEqual(memory[address], (kind, n, component))
            finally:
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
                self.assertEqual(process.wait(timeout=30), 0)
                process.stdout.close()


if __name__ == '__main__':
    unittest.main()

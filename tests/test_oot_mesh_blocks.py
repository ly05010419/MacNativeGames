"""A mesh shader with no task shader launches its workgroups 1024 at a time through its own object function (MeshStage.cs
ObjectEntry, after nxnative 09dcb627): Metal holds a mesh grid to 1024 threadgroups and Ocarina of Time's foliage drew up to
1,238 in one indirect draw (oot-fps90 n90d). The translator's object function runs on the GPU (tests/t_mesh_blocks.m) with
the block launch of nvn_mesh_tasks.inc mtk_encode: every workgroup of 1 to 3,000, direct and indirect, runs exactly once.
Synthetic instructions only; no game files."""
import importlib.util
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
_spec = importlib.util.spec_from_file_location('ampere_scoreboard_gpu', ROOT/'games/oot/tests/ampere_scoreboard_gpu.py')
_scoreboard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_scoreboard)
inst, mov = _scoreboard.inst, _scoreboard.mov
TASKS = (ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mesh_tasks.inc').read_text(encoding='utf-8')
STAGE = (ROOT/'games/oot/tools/totk_xlate/MeshStage.cs').read_text(encoding='utf-8')


def translate(folder):
    """The translator's MSL and reply for a 32-thread mesh shader with no task shader."""
    binary = Path(os.environ.get('OOT_TEST_XLATE') or ROOT/'games/oot/prebuilt/macos-arm64/totk_xlate')
    process = subprocess.Popen([str(binary), '--service', '--spirv-cross', '/unused', '--out', str(folder), '--chain', 'mesh-blocks-test'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        ctl, header = bytearray(224), bytearray(128)
        struct.pack_into('<HH', ctl, 0x58, 32, 0)
        struct.pack_into('<HH', ctl, 0x60, 32, 32)
        ctl[0x5f] = 3
        header[0x35] = 0xf0
        key = 'blocks'
        path = folder/(key+'.code')
        path.write_bytes(header + mov(1, 0) + mov(2, 0) + inst(0x927, ra=1, b=2, c=0x8000) + inst(0x94d, c=0x03800000))
        request = dict(variant=key, stage='ms', arch='sm86', code=str(path), mesh_ctl=ctl.hex(), c1=bytes(128).hex(),
                       depth_minus_w=True, stage_device=False)
        process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
        reply = json.loads(process.stdout.readline())
    finally:
        process.stdin.close(); process.wait(timeout=30); process.stdout.close()
    return reply, folder/(key+'_b.metal')


@unittest.skipUnless(sys.platform == 'darwin', 'NativeAOT translator and Metal are Mac tools')
class MeshBlocks(unittest.TestCase):
    def test_object_function_launches_every_workgroup_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            reply, metal = translate(Path(tmp))
            self.assertTrue(reply.get('ok'), reply)
            text = metal.read_text()
            self.assertIn('[[object]] void main0_o(', text)
            self.assertIn('dm_first[0]+dm_block.base+', text)
            reflection = json.loads(metal.with_name('blocks_b.json').read_text())
            self.assertEqual(reflection['mesh_stage'].get('object'), 'main0_o')   # the host loads it by this name (nvn_mtl.m)
            binary = Path(tmp)/'t_mesh_blocks'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(ROOT/'tests/t_mesh_blocks.m'),
                                    '-framework', 'Metal', '-framework', 'Foundation', '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(metal)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue('mesh blocks passed' in run.stdout or 'SKIP' in run.stdout, run.stdout)

    def test_host_constants_match_the_translator(self):
        for name, value in (('MTK_OBJECT_BLOCK', 'ObjectBlock'), ('MTK_OBJECT_COUNT_BUFFER', 'ObjectCountBuffer')):
            host = re.search(name + r'=(\d+)', TASKS).group(1)
            translator = re.search(value + r' = (\d+)', STAGE).group(1)
            self.assertEqual(host, translator, name)
        self.assertIn('ObjectEntry = "main0_o"', STAGE)
        self.assertIn('return v&&ms&&ms->mesh_notask&&ms->ofn;', TASKS)   # an older translation (no object function) draws as before


if __name__ == '__main__':
    unittest.main()

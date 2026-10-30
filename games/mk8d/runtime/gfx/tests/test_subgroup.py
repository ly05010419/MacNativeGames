"""Vertex subgroup masks reached through component pointers."""
import ast
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


source = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
sys.path.insert(0, str(source.parent))
from dmsh_chain import SPIRV_CROSS  # noqa: E402
tree = ast.parse(source.read_text())
nodes = [n for n in tree.body if
         isinstance(n, ast.FunctionDef) and n.name in ('spv_single_lane', '_ep_drop') or
         isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ('SG_OPS', 'SG_BUILTINS') for t in n.targets)]
scope = {'struct': struct}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), scope)

ASSEMBLY = '''OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformBallot
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %mask %vertex %position
OpDecorate %mask BuiltIn SubgroupLtMask
OpDecorate %vertex BuiltIn VertexIndex
OpDecorate %position BuiltIn Position
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v4uint = OpTypeVector %uint 4
%v4float = OpTypeVector %float 4
%pmask = OpTypePointer Input %v4uint
%pu = OpTypePointer Input %uint
%pi = OpTypePointer Input %int
%po = OpTypePointer Output %v4float
%mask = OpVariable %pmask Input
%vertex = OpVariable %pi Input
%position = OpVariable %po Output
%three = OpConstant %int 3
%main = OpFunction %void None %fn
%entry = OpLabel
%vid = OpLoad %int %vertex
%index = OpBitwiseAnd %int %vid %three
%ptr = ACCESS %pu %mask %index
%value = OpLoad %uint %ptr
%f = OpConvertUToF %float %value
%p = OpCompositeConstruct %v4float %f %f %f %f
OpStore %position %p
OpReturn
OpFunctionEnd
'''


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val') and shutil.which(SPIRV_CROSS), 'requires SPIR-V tools')
class SubgroupTests(unittest.TestCase):
    def test_mask_component_pointer_becomes_valid_constant_extraction(self):
        for access in ('OpAccessChain', 'OpInBoundsAccessChain'):
            with self.subTest(access=access), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                src, spv = root / 'mask.spvasm', root / 'mask.spv'
                src.write_text(ASSEMBLY.replace('ACCESS', access))
                subprocess.run(['spirv-as', '--target-env', 'spv1.3', str(src), '-o', str(spv)], check=True, capture_output=True)
                self.assertTrue(scope['spv_single_lane'](spv))
                result = subprocess.run(['spirv-val', '--target-env', 'spv1.3', str(spv)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([SPIRV_CROSS, '--msl', str(spv)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn('simd_', result.stdout)
                self.assertFalse(scope['spv_single_lane'](spv))


if __name__ == '__main__':
    unittest.main()

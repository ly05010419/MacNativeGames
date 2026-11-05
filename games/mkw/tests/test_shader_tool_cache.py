"""Changing a tool dependency must invalidate a staged shader translation chain."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / 'scripts/build_shader_tools.py'
spec = importlib.util.spec_from_file_location('mkw_shader_builder', path)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class ToolCache(unittest.TestCase):
    def test_changed_dependency_and_changed_sources_invalidate_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            for name in ('spirv-cross/spirv-cross', 'dmsh_xlate/dmsh_xlate', 'shader-service/ampere_service', 'dmsh_xlate/Shader.dll'):
                target = build / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b'fixture')
            (build / 'shader-tools.json').write_text(json.dumps(dict(source_sha256='original', outputs=builder.output_fingerprints(build))))
            self.assertTrue(builder.tools_current(build, 'original'))
            self.assertFalse(builder.tools_current(build, 'changed source'))
            (build / 'dmsh_xlate/Shader.dll').write_bytes(b'changed')
            self.assertFalse(builder.tools_current(build, 'original'))

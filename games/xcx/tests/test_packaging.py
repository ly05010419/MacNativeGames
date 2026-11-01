"""A GPU-enabled diagnostic must fail before creating an incomplete bundle."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("xcx_package", ROOT / "scripts/package_app.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackagingTest(unittest.TestCase):
    def test_missing_shader_tool_rejected_before_bundle_creation(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "build").mkdir()
            (work / "build/xcx-native").write_bytes(b"not executed")
            output = work / "Diagnostic.app"
            with self.assertRaisesRegex(ValueError, "Build shader-tools first"):
                package.package(work, output, extended_va=True, gpu_submit=True)
            self.assertFalse(output.exists())

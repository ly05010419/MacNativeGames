"""Platform conditionals keep persistent user libraries out of the iOS renderer.

This checks conditional source selection; it is not an iOS SDK build or device test.
"""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(shutil.which('unifdef'), 'requires the conditional-source tool unifdef')
class ShaderLibraryPlatform(unittest.TestCase):
    def selected(self, name, ios):
        result = subprocess.run(['unifdef', '-DTARGET_OS_IPHONE='+str(ios), str(NVN/name)],
                                capture_output=True, text=True)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        return result.stdout

    def test_ios_renderer_does_not_include_the_new_cache_path(self):
        source = self.selected('nvn_mtl.m', 1)
        for name in ('totk_shader_libraries.h', 'totk_shader_libraries_read', 'totk_shader_library_load',
                     'TOTK_SHADER_LIBRARIES', 'g_rt_user_prebuilt'):
            self.assertNotIn(name, source)
        self.assertIn('lib_from_file', source)
        self.assertIn('newLibraryWithData', source)

    def test_mac_renderer_includes_the_user_cache_and_memory_loader(self):
        source = self.selected('nvn_mtl.m', 0)
        for name in ('totk_shader_libraries.h', 'TOTK_SHADER_LIBRARIES', 'totk_shader_library_load'):
            self.assertIn(name, source)

    def test_ios_header_defines_no_loader_or_manifest_parser(self):
        source = self.selected('totk_shader_libraries.h', 1)
        self.assertNotIn('totk_shader_library_load', source)
        self.assertNotIn('totk_shader_libraries_read', source)
        self.assertNotIn('manifest.json', source)

"""Native visionOS configuration, SDK cache isolation and signing selection."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT/'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import build_ipa
import install_device


class VisionOS(unittest.TestCase):
    def test_platform_project_preserves_ios_and_uses_native_vision_device_family(self):
        source = (ROOT/'games/totk/ios/project.yml').read_text()
        self.assertEqual(build_ipa.project_for_platform(source, 'ios'), source)
        vision = build_ipa.project_for_platform(source, 'visionos')
        self.assertIn('platform: visionOS', vision)
        self.assertIn('TARGETED_DEVICE_FAMILY: "7"', vision)
        self.assertIn('XROS_DEPLOYMENT_TARGET: "26.0"', vision)
        self.assertNotIn('UIRequiresFullScreen', vision)
        with self.assertRaises(RuntimeError):
            build_ipa.project_for_platform(source.replace('platform: iOS', 'platform: changed'), 'visionos')

    def test_metallibs_are_never_reused_for_another_sdk(self):
        with tempfile.TemporaryDirectory() as d:
            seed = Path(d)
            (seed/'a_b.metallib').write_bytes(b'compiled')
            manifest = {'ios_source_manifest_sha256': 'source', 'ios_metallibs': 1,
                        'ios_metal_flags': ' '.join(build_ipa.METAL_FLAGS)}
            def check(sdk): return build_ipa.seed_reusable(seed, 'source', ['a_b.metal'], sdk)
            for origin in (None, 'iphoneos', 'xros'):
                with self.subTest(origin=origin):
                    if origin: manifest['ios_sdk'] = origin
                    (seed/'seed-manifest.json').write_text(json.dumps(manifest))
                    self.assertEqual(check('iphoneos'), origin != 'xros')
                    self.assertEqual(check('xros'), origin == 'xros')
            (seed/'a_b.metallib').unlink()
            self.assertFalse(check('xros'))

    def test_signing_platform_comes_from_app_for_folder_and_ipa(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); app = root/'TOTK.app'; app.mkdir()
            for info, want in [({'DTPlatformName': 'xros'}, 'visionos'),
                               ({'CFBundleSupportedPlatforms': ['XROS']}, 'visionos'),
                               ({'DTPlatformName': 'iphoneos'}, 'ios')]:
                with self.subTest(info=info):
                    raw = plistlib.dumps(info); (app/'Info.plist').write_bytes(raw)
                    ipa = root/'TOTK.ipa'
                    with zipfile.ZipFile(ipa, 'w') as z: z.writestr('Payload/TOTK.app/Info.plist', raw)
                    self.assertEqual(install_device.built_platform(app), want)
                    self.assertEqual(install_device.built_platform(ipa), want)

    def test_native_profile_uses_visionos_product_and_separate_cache(self):
        with tempfile.TemporaryDirectory() as d, patch.object(install_device, 'CACHE', Path(d)), \
                patch.object(install_device, 'run'), patch.object(install_device.subprocess, 'run') as run:
            run.return_value.returncode = 0
            for platform, sdk in [('ios', 'iphoneos'), ('visionos', 'xros')]:
                prov, app = install_device.profile('TEAM', 'org.macnativegames.totk', 'DEVICE', platform)
                project = app.parents[4]/'project.yml'
                self.assertIn('platform: '+('visionOS' if sdk == 'xros' else 'iOS'), project.read_text())
                self.assertEqual(app.parent.name, 'Debug-'+sdk)
                self.assertEqual(prov, app/'embedded.mobileprovision')

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS SDK')
    def test_unavailable_metalfx_modes_stay_off_even_when_requested_by_environment(self):
        # Exercise the visionOS branches on the host without launching a game or requiring a headset.
        nvn = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = root/'probe.m'; exe = root/'probe'
            source.write_text('''#include <TargetConditionals.h>
#undef TARGET_OS_VISION
#define TARGET_OS_VISION 1
#include "totk_mfx.m"
#include "totk_taa.m"
int main(void) {
    for (int mode = -1; mode <= 3; mode++) {
        mfx_set(mode); mfx_latch(); taa_set(mode & 1); taa_frame_end();
        if (mfx_enabled() || taa_enabled() || taa_active() || taa_ready()) return 1;
        if (mfx_frame_prepare(0.02) != -1 || mfx_output(0) || mfx_residency() || taa_residency()) return 2;
    }
    return 0;
}
''')
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-I'+str(nvn), str(source),
                            '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'MetalFX',
                            '-framework', 'QuartzCore', '-o', str(exe)], check=True, capture_output=True)
            for mode in ('0', 'interp30', 'interp', 'interpmax'):
                subprocess.run([str(exe)], env={**os.environ, 'TOTK_MFX': mode, 'TOTK_TAA': '1'},
                               check=True, capture_output=True, timeout=15)

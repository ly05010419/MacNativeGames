"""The iPhone / iPad app's shader translator (2026-11-03, after PR #1468): the Mac's totk_xlate service as a NativeAOT
static library (games/totk/prebuilt/ios-arm64/libtotk_xlate.a, tools/totk_xlate/publish_ios_lib.sh), linked by
build_ipa.py with SPIRV-Cross that build_app.py builds for the device, and called by totk_shader_rt.m (DEVICE_XLATE)
instead of starting the service. Read, not run: the translation itself was replayed (Mac library, iOS simulator) against
the shipped totk_xlate, every file byte-identical (issues_lessons.md 2026-11-03)."""
from pathlib import Path
import hashlib
import re
import unittest

REPO = Path(__file__).resolve().parents[1]
TOTK = REPO / 'games/totk'
XLATE = TOTK / 'tools/totk_xlate'
LIB = TOTK / 'prebuilt/ios-arm64/libtotk_xlate.a'


class DeviceTranslator(unittest.TestCase):
    def test_prebuilt_files_are_the_recorded_ones(self):
        for f in (LIB, LIB.with_name('libtotk_xlate_bootstrap.o')):
            want = (f.parent / (f.name + '.sha256')).read_text().strip()
            self.assertEqual(hashlib.sha256(f.read_bytes()).hexdigest(), want, f.name)

    def test_the_runtime_start_up_object_is_linked_as_a_file(self):
        """From inside the archive nothing pulls it in, and without its initializer the first call has no runtime."""
        script = (XLATE / 'publish_ios_lib.sh').read_text()
        self.assertNotIn('libbootstrapperdll.o libRuntime', script)
        self.assertIn('cp "$PACK/libbootstrapperdll.o" "$OUT/libtotk_xlate_bootstrap.o"', script)
        ipa = (TOTK / 'ios/tools/build_ipa.py').read_text()
        self.assertIn("' Gen/deps/lib/libtotk_xlate_bootstrap.o Gen/deps/lib/libtotk_xlate.a", ipa)

    def test_library_exports_and_project_switch(self):
        exports = (XLATE / 'LibExports.cs').read_text()
        for name in ('totk_xlate_init', 'totk_xlate_request'):
            self.assertIn(f'EntryPoint = "{name}"', exports)
        project = (XLATE / 'totk_xlate.csproj').read_text()
        self.assertRegex(project, r"Condition=\"'\$\(TotkXlateLib\)' == 'true'\">\s*<OutputType>Library</OutputType>\s*<NativeLib>Static</NativeLib>")
        script = (XLATE / 'publish_ios_lib.sh').read_text()
        self.assertIn('-r ios-arm64', script)
        self.assertIn('-p:TotkXlateLib=true', script)
        self.assertIn('-p:PublishAotUsingRuntimePack=true', script)

    def test_service_and_library_share_one_request_path(self):
        """The command-line service and the library both answer through HandleLine, so they cannot drift apart."""
        service = (XLATE / 'Service.cs').read_text()
        run = service[service.index('public static int Run('):service.index('public static void Configure(')]
        self.assertIn('stdout.Write(HandleLine(line) + "\\n");', run)
        self.assertNotIn('Handle(JsonNode.Parse', run)
        self.assertIn('ShaderService.HandleLine(', (XLATE / 'LibExports.cs').read_text())

    def test_runtime_calls_the_library_only_on_the_device_build(self):
        rt = (TOTK / 'third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m').read_text()
        self.assertIn('#if TARGET_OS_IPHONE && defined(TOTK_DEVICE_XLATE)\n#define DEVICE_XLATE 1', rt)
        self.assertIn('#if TARGET_OS_IPHONE && !defined(DEVICE_XLATE)\n    return 1;', rt)   # no_translator: others as before
        self.assertIn('totk_xlate_request(json_string(rq).UTF8String, line, sizeof line);', rt)
        nvn = (TOTK / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertIn('#if TARGET_OS_IPHONE && !defined(TOTK_DEVICE_XLATE)\n    return 0;   // no translator on the device', nvn)

    def test_packaging_builds_and_links_it_for_ios_only(self):
        build_app = (REPO / 'build_app.py').read_text()
        self.assertEqual(len(re.findall(r'^ +build_device_translator\(tree, logs\)$', build_app, re.M)), 3)   # Tears of the Kingdom, Breath of the Wild and NS2
        ns2 = build_app[build_app.index('def main_ns2('):build_app.index('def identify_game(')]
        self.assertIn("device_app = args.platform == 'ios'", ns2)
        self.assertIn("if args.platform == 'visionos':", ns2)
        self.assertIn("if args.platform == 'ios':   # visionOS keeps the startup shaders only", build_app)
        func = build_app[build_app.index('def build_device_translator('):build_app.index('def check_translator(')]
        self.assertIn("if n != 'main.cpp'", func)
        self.assertIn("'tools/totk_xlate/spirv_cross_inproc.cpp'", func)
        ipa = (TOTK / 'ios/tools/build_ipa.py').read_text()
        self.assertIn("device_xlate=not vision and all((xlate/lib).is_file() for lib in xlate_libs)", ipa)
        self.assertIn("OTHER_CFLAGS = $(inherited) -DTOTK_DEVICE_XLATE=1", ipa)
        self.assertRegex(ipa, r"libtotk_xlate\.a Gen/deps/lib/libtotk_spirv_cross\.a -lc\+\+ -lz -licucore")
        self.assertIn("xlate_libs=('libtotk_xlate_bootstrap.o','libtotk_xlate.a','libtotk_spirv_cross.a')", ipa)


if __name__ == '__main__':
    unittest.main()

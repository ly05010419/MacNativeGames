"""Every game's runtime starts only the native shader translator (games/*/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m).

The Python + .NET service (scripts/shader_service.py) was removed in 4eeecb39 and no game carries it, but until
2026-11-11 every copy of the runtime still started it when TOTK_SHADER_XLATE or TOTK_SHADER_SERVICE=python was set, or
when build/shader-service/totk_xlate was missing: a python3 child running a script that is not there (games/alc's
CHANGELOG records it happening). Now TOTK_SHADER_XLATE names another native program (MKW refuses it: it translates with
its Ampere service), any other service is refused and a missing program stops with its path."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COPIES = sorted(ROOT.glob('games/*/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m'))


def service_start(source):
    """The code that starts the service, without its // comments (they name the removed script on purpose)."""
    text = source.read_text(encoding='utf-8')
    start = text.index('NSString *sc = [r stringByAppendingString:@"/build/spirv-cross/spirv-cross"];')
    started = text.index('runtime translation service started', start)
    code = text[start:text.index(');', started) + 2]
    return '\n'.join(line.split('//', 1)[0] for line in code.splitlines())


class NativeTranslatorOnly(unittest.TestCase):
    def test_every_copy_is_checked(self):
        self.assertGreaterEqual(len(COPIES), 21)
        self.assertIn(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m', COPIES)

    def test_no_python_service_is_started(self):
        for source in COPIES:
            code = service_start(source)
            for gone in ('shader_service.py', 'python3', 'tooling/dotnet', 'totk_xlate.dll', 'posix_spawnp', 'use_native'):
                self.assertNotIn(gone, code, f'{source.relative_to(ROOT)}: {gone}')

    def test_a_trial_translator_is_a_native_program(self):
        for source in COPIES:
            code = service_start(source)
            self.assertIn('NSString *native = xe && *xe ? @(xe) : [r stringByAppendingString:@"/build/shader-service/totk_xlate"];',
                          code, source.relative_to(ROOT))
            self.assertIn('const char *argv[] = {native.UTF8String, "--service"', code, source.relative_to(ROOT))

    def test_other_services_and_a_missing_program_stop_with_a_reason(self):
        for source in COPIES:
            code = service_start(source)
            self.assertIn('if (want && *want && strcmp(want, "native")) die(', code, source.relative_to(ROOT))
            self.assertIn('if (access(native.fileSystemRepresentation, X_OK)) die("shader translator %s is missing', code,
                          source.relative_to(ROOT))

    def test_the_makefile_does_not_offer_the_old_service(self):
        makefile = (ROOT / 'games/totk/Makefile').read_text(encoding='utf-8')
        self.assertNotIn('TOTK_SHADER_SERVICE=python', makefile)
        self.assertNotIn('scripts/shader_service.py', makefile)


if __name__ == '__main__':
    unittest.main()

"""Tears of the Kingdom's runtime starts only the native shader translator (totk_shader_rt.m).

The Python + .NET service (scripts/shader_service.py) was removed in 4eeecb39, but until 2026-11-11 the runtime still
started it when TOTK_SHADER_XLATE or TOTK_SHADER_SERVICE=python was set, or when build/shader-service/totk_xlate was
missing: a python3 child running a script that is not there. Now TOTK_SHADER_XLATE names another native program, any
other service is refused and a missing program stops with its path."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m'


def service_start():
    """The code that starts the service, without its // comments (they name the removed script on purpose)."""
    text = SOURCE.read_text(encoding='utf-8')
    start = text.index('NSString *sc = [r stringByAppendingString:@"/build/spirv-cross/spirv-cross"];')
    code = text[start:text.index('pthread_mutex_unlock(&g_svc_start_lock);', start)]
    return '\n'.join(line.split('//', 1)[0] for line in code.splitlines())


class NativeTranslatorOnly(unittest.TestCase):
    def test_no_python_service_is_started(self):
        code = service_start()
        for gone in ('shader_service.py', 'python3', 'tooling/dotnet', 'totk_xlate.dll', 'posix_spawnp'):
            self.assertNotIn(gone, code)

    def test_a_trial_translator_is_a_native_program(self):
        code = service_start()
        self.assertRegex(code, r'NSString \*native = xe && \*xe \? @\(xe\) : \[r stringByAppendingString:@"/build/shader-service/totk_xlate"\];')
        self.assertIn('const char *argv[] = {native.UTF8String, "--service"', code)

    def test_other_services_and_a_missing_program_stop_with_a_reason(self):
        code = service_start()
        self.assertRegex(code, r'if \(want && \*want && strcmp\(want, "native"\)\) die\(')
        self.assertRegex(code, r'if \(access\(native\.fileSystemRepresentation, X_OK\)\) die\("shader translator %s is missing')

    def test_the_makefile_does_not_offer_the_old_service(self):
        makefile = (ROOT / 'games/totk/Makefile').read_text(encoding='utf-8')
        self.assertNotIn('TOTK_SHADER_SERVICE=python', makefile)
        self.assertNotIn('scripts/shader_service.py', makefile)


if __name__ == '__main__':
    unittest.main()

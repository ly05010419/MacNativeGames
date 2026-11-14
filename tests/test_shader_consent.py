"""Shader inbox consent regressions; synthetic rows and a loopback server only."""
import http.server
import json
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
GFX = TOTK / 'third_party/d2r_gfx/gfx/nvn'
PROBES = TOTK / 'tests/shader_contrib'


class Fixture(http.server.BaseHTTPRequestHandler):
    requests = []
    on_post = None

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append((self.path, body))
        if Fixture.on_post:
            Fixture.on_post()
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{}')

    def log_message(self, *_):
        pass


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode command line tools')
class ShaderConsentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='shader-consent-')
        cls.server = http.server.HTTPServer(('127.0.0.1', 0), Fixture)
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()
        cls.probe = Path(cls.temp.name) / 'consent'
        command = ['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                   '-DGAME_OOT=1', '-DTOTK_GAME_VERSION="131072"',
                   f'-DTOTK_CONTRIB_INBOX="http://127.0.0.1:{cls.server.server_port}/v1/contrib"',
                   f'-I{GFX}', str(PROBES / 'consent_probe.m'), str(PROBES / 'consent_other.m'),
                   '-framework', 'Foundation', '-o', str(cls.probe)]
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode:
            cls.server.shutdown()
            cls.server.server_close()
            cls.temp.cleanup()
            raise AssertionError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join(timeout=5)
        cls.temp.cleanup()

    def setUp(self):
        Fixture.requests = []
        Fixture.on_post = None
        self.folder = tempfile.TemporaryDirectory(prefix='shader-consent-case-')
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / 'contrib.txt'

    def run_probe(self, mode):
        env = {key: value for key, value in os.environ.items() if not key.startswith('TOTK_')}
        run = subprocess.run([str(self.probe), mode, str(self.path)], env=env,
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['status'], 'pass')
        self.assertEqual(result['failures'], 0)
        return result

    def test_off_during_one_chunk_stops_the_next_chunk(self):
        upload = self.path.parent / '.contrib.txt.upload'
        Fixture.on_post = lambda: upload.write_text('off\n')
        result = self.run_probe('chunk-stop')
        self.assertEqual(result['sent'], 2000)
        self.assertEqual(len(Fixture.requests), 1)
        route, body = Fixture.requests[0]
        self.assertEqual(route, '/v1/contrib')
        self.assertEqual((body['game'], body['game_version'], body['platform']), ('oot', '131072', 'mac'))
        self.assertEqual(len(body['rows']), 2000)
        self.assertTrue(all(row.startswith('V\t') for row in body['rows']))
        self.assertEqual(len([line for line in self.path.read_text().splitlines() if line.startswith('V\t')]), 2001)

    def test_quarantined_game_does_not_give_consent_to_the_new_game(self):
        for owner in ('totk', None):
            with self.subTest(owner=owner), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'contrib.txt'
                self.path = path
                original = ('# Inbox game: ' + owner + '\n' if owner else '') + 'R\tabcd\n'
                path.write_text(original)
                for ext, text in {'upload': 'auto\n', 'done': 'R\tabcd\n', 'pruned': 'old-app',
                                  'install': 'foreign-id', 'sent': '8\n', 'reminded': 'never\n'}.items():
                    (path.parent / ('.contrib.txt.' + ext)).write_text(text)
                # Claim through a separate process, as the real launcher does.
                run = subprocess.run([str(self.owner_probe), 'claim-write', str(path)], env=self.clean_env(),
                                     capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                asides = list(path.parent.glob('contrib.txt.*'))
                self.assertEqual(len(asides), 1)
                self.assertEqual(asides[0].read_text(), original)
                for ext in ('upload', 'done', 'pruned', 'install', 'sent', 'reminded'):
                    self.assertFalse((path.parent / ('.contrib.txt.' + ext)).exists())
                    self.assertTrue((path.parent / ('.' + asides[0].name + '.' + ext)).exists())
                self.run_probe('inspect')
                self.assertEqual(Fixture.requests, [])

    @staticmethod
    def clean_env():
        return {key: value for key, value in os.environ.items() if not key.startswith('TOTK_')}

    @property
    def owner_probe(self):
        probe = Path(self.temp.name) / 'owner'
        if not probe.exists():
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-DGAME_OOT=1', '-DTOTK_GAME_VERSION="131072"',
                                    '-DTOTK_CONTRIB_INBOX=""', f'-I{GFX}', str(PROBES / 'owner_probe.m'),
                                    '-framework', 'Foundation', '-o', str(probe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
        return probe

    def test_failed_quarantine_preserves_path_and_foreign_rows(self):
        self.path.write_text('# Inbox game: totk\nR\tabcd\n')
        (self.path.parent / '.contrib.txt.upload').write_text('auto\n')
        self.run_probe('claim-failure')
        self.assertEqual(Fixture.requests, [])

    def test_failed_opt_out_stops_uploads_in_the_other_translation_unit(self):
        self.run_probe('failed-off')
        self.assertEqual(Fixture.requests, [])

    def test_failed_opt_in_does_not_enable_uploads(self):
        self.run_probe('failed-on')
        self.assertEqual(Fixture.requests, [])

    def test_real_settings_page_compiles_with_the_persistence_result(self):
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                '-Wno-deprecated-declarations', '-Wno-unused-function', '-DGAME_OOT=1',
                                f'-I{GFX}', f'-I{TOTK / "runtime"}', '-fsyntax-only',
                                str(PROBES / 'consent_ui_syntax.m')], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)


if __name__ == '__main__':
    unittest.main()

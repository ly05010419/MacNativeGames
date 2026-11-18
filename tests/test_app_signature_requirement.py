"""Mac apps are signed with the bundle ID as their designated requirement, so macOS keeps the player's "removable volume"
permission across updates (user 2026-11-15: "如何 不弹 允许访问可移除宗卷？", then "加上 测试， 以后都这样").

A plain ad-hoc signature (codesign --sign -) makes the requirement this build's code hash. TCC.db then holds a cdhash for
the app: every update asks again, and an app started with open waits in its first file open on the external volume until
someone clicks (Ocarina of Time 23:05, "Failed to match existing code requirement … kTCCServiceSystemPolicyRemovableVolumes";
Tears of the Kingdom, Breath of the Wild and Pokopia, signed by build_app.sign_app, held an identifier and were not asked).
build_app.sign_app had it since 2026-10-31, but Ocarina of Time's own packaging and the MNG Launcher, written later, signed
without it and nothing checked them; so the same prompt came back.

Scope (user: "不要 管其他 社区游戏"): build_app.py, update_runtime.py, games/totk, games/oot, games/tmnt and tools/, Mac only (iOS apps
are signed by Xcode with a real identity). Every codesign call there that signs must name the bundle ID, or say on its line
that it signs something inside the app ("# nested: not the app"): a new signing call without either fails here."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCOPE = [ROOT / 'build_app.py', ROOT / 'update_runtime.py', ROOT / 'games/totk', ROOT / 'games/oot', ROOT / 'games/tmnt', ROOT / 'tools']
KINDS = {'.py', '.sh', '.mk', '.command'}
SIGN = re.compile(r"""codesign\W.*(--sign|\W-s\W)""")
MARK = '# nested: not the app'


def sources():
    for base in SCOPE:
        files = [base] if base.is_file() else sorted(p for p in base.rglob('*') if p.is_file())
        for path in files:
            parts = set(path.relative_to(ROOT).parts)
            if parts & {'ios', 'tests', 'third_party', '__pycache__', 'build'}:
                continue
            if path.suffix in KINDS or path.name == 'Makefile':
                yield path


def signing_lines():
    for path in sources():
        text = path.read_text(errors='replace')
        for number, line in enumerate(text.splitlines(), 1):
            code = line.split('#')[0] if path.suffix != '.py' else line
            if 'codesign' in line and SIGN.search(line) and '--verify' not in line and not line.lstrip().startswith(('#', '//')):
                yield path, number, line, text


class SignedByBundleId(unittest.TestCase):
    def test_every_signing_call_names_the_bundle_id_or_is_nested(self):
        found, bad = 0, []
        for path, number, line, text in signing_lines():
            found += 1
            names = 'designated => identifier' in line
            spread = re.search(r'\*(requirement|req)\b', line) and 'designated => identifier' in text
            if not (names or spread or MARK in line):
                bad.append(f'{path.relative_to(ROOT)}:{number}: {line.strip()}')
        self.assertGreater(found, 3, 'the scan found no signing calls: its pattern is wrong')
        self.assertEqual(bad, [], 'sign the app with -r=designated => identifier "<bundle id>" (build_app.sign_app), or mark '
                                  f'a nested bundle "{MARK}":\n' + '\n'.join(bad))

    def test_tmnt_packaging_and_update_name_the_plist_bundle_id(self):
        for name in ('package_private.py', 'tmnt_update.py'):
            source = (ROOT / 'games/tmnt/scripts' / name).read_text()
            self.assertIn('designated => identifier', source)
            self.assertIn('info["CFBundleIdentifier"]', source)
        self.assertIn(ROOT / 'games/tmnt', SCOPE)

    def test_build_app_sign_app_names_the_bundle_id(self):
        source = (ROOT / 'build_app.py').read_text()
        body = source[source.index('def sign_app('):]
        body = body[:body.index('\ndef ')]
        self.assertIn('designated => identifier', body)

    def test_ocarina_signs_its_app_with_sign_app(self):
        source = (ROOT / 'games/oot/build.py').read_text()
        self.assertIn("build_app.sign_app(temporary,work/'logs')", source)

    def test_the_launcher_names_its_bundle_id(self):
        source = (ROOT / 'tools/builder_app/make_app.py').read_text()
        self.assertIn('designated => identifier "{BUNDLE_ID}"', source)

    def test_the_check_catches_a_plain_ad_hoc_signature(self):
        line = "    subprocess.run(['codesign','--force','--sign','-',temporary],check=True)"
        self.assertTrue(SIGN.search(line))
        self.assertFalse('designated => identifier' in line or MARK in line)


if __name__ == '__main__':
    unittest.main()

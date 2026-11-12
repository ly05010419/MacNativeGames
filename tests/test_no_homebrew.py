"""Players need no Homebrew (user 2026-11-12: "玩家完全不用装 Homebrew", "全部 24 款，用共用的 tools/deps"): every build
dependency comes from tools/deps. This fails when a build file links a Homebrew package path, runs brew, or tells a
player to brew install something other than the optional Python alternative."""
import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Code a player's build runs. Docs are checked separately (players read them).
CODE = re.compile(r'(^|/)(Makefile|[^/]+\.(py|sh|command|c|m|h|mk|yml))$')
FORBIDDEN = re.compile(r'/opt/homebrew/(opt|Cellar|lib|include)/|brew --prefix|\bbrew install (?!python@3\.1[45]|python-tk@3\.1[45])')
# Not players' builds: movie probes against ffmpeg, the VR installer's adb, the Windows
# games (their own toolchain and skill), and the per-game history records.
ALLOWED = re.compile(r'^(windows_games/|zeldaVR/|tests/)|/movie_probe_inputs\.py$|/CHANGELOG\.md$'
                     r'|/local-changes\.patch$|/VALIDATION\.md$|/validation-history\.md$|^\.claude/skills/install-ra2/')


def tracked():
    out = subprocess.run(['git', 'ls-files'], cwd=REPO, capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if not ALLOWED.search(f)]


class NoHomebrew(unittest.TestCase):
    def offenders(self, files):
        found = []
        for name in files:
            try:
                text = (REPO / name).read_text(errors='ignore')
            except (OSError, UnicodeDecodeError):
                continue
            for number, line in enumerate(text.splitlines(), 1):
                if FORBIDDEN.search(line):
                    found.append(f'{name}:{number}: {line.strip()[:120]}')
        return found

    def test_build_code_takes_nothing_from_homebrew(self):
        found = self.offenders([f for f in tracked() if CODE.search(f)])
        self.assertEqual(found, [], 'use tools/deps (prefix / program / env) instead of Homebrew')

    def test_players_are_not_told_to_brew_install_build_dependencies(self):
        docs = [f for f in tracked() if f.endswith('.md') and (f.startswith(('docs/', 'games/')) or '/' not in f
                                                              or f.startswith('.claude/skills/'))]
        self.assertEqual(self.offenders(docs), [])

    def test_every_game_makefile_that_links_codecs_asks_tools_deps(self):
        for makefile in sorted(REPO.glob('games/*/Makefile')):
            text = makefile.read_text()
            for variable, name in (('OPUS_PREFIX', 'opus'), ('VPX_PREFIX', 'libvpx'), ('VORBIS_PREFIX', 'vorbis'),
                                   ('OGG_PREFIX', 'ogg'), ('DAV1D_PREFIX', 'dav1d')):
                if f'$({variable})' in text:
                    with self.subTest(makefile=makefile.parent.name, variable=variable):
                        self.assertIn(f'{variable} ?= $(or $(shell python3 ../../tools/deps prefix {name})', text)


if __name__ == '__main__':
    unittest.main()

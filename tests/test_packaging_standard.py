"""The packaging standards every game must meet (CLAUDE.md, "每个游戏都要满足的标准"), checked in the sources.

Each game's packaging is its own copy of the code, so an optimization made for one game does not reach the others by itself.
A new games/<name>/build.py fails here until it is added to PATHS. A standard a path does not meet must be listed in its gaps
with the reason; a gap that has been closed must be removed, so the table stays true.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(*names):
    return '\n'.join((ROOT / n).read_text(errors='replace') for n in names if (ROOT / n).is_file())


def seed_identity_ignores_runtime(text):
    """The translation cache's identity must not contain the runtime binary's hash."""
    body = re.search(r'def translation_context\(.*?\n    return ', text, re.S)
    return body is None or 'sha256(binary)' not in body.group(0)


STANDARDS = {
    'shader data in its own signed bundle': lambda t: 'ShaderData-' in t,
    'pipeline archive built while packaging': lambda t: 'pipeline-archive' in t or 'mac_pipelines' in t,
    'title does not wait for the warm-up': lambda t: 'TOTK_WARM_BEFORE_TITLE' in t,
    'no program runs inside a new app': lambda t: re.search(r"run_outside_bundle|work/'build/oot-native'", t) is not None,
    'seed and library digests are reused': lambda t: 'seed_digests' in t or 'record_verified' in t,
    'repackaging keeps the .app folder': lambda t: 'install_app(' in t and re.search(r'(partial|temporary)\.rename\(out\)', t) is None,
    'a runtime change can update quickly': lambda t: 'place_runtime' in t or 'update_ocarina' in t,
    'a runtime change keeps the translations': seed_identity_ignores_runtime,
    # the runtime's warm-up, its contribution filter and the launcher's prune read pso_title.tsv / pso_manifest.tsv beside the
    # shader map (build/): Ocarina of Time's were only in the seed, so every pipeline it drew was contributed (2026-11-10)
    'warm-up lists beside the shader map': lambda t: "'build/pso_manifest.tsv'" in t,
    # the pipeline archive is built from those same build/ lists: Ocarina of Time's fast update built it from the seed's
    # older list, 2,957 of the App's 7,516 render pipelines, and 4,559 were compiled at the first start (2026-11-11)
    'pipeline archive built from the App\'s lists': lambda t: (
        'check_archive_lists(' in t or "lists = [p for p in (build / 'pso_title.tsv', build / 'pso_manifest.tsv'" in t),
}

GFX = 'games/{}/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'

# name -> (the sources of that packaging path, {standard not met: why})
PATHS = {
    'totk': (['build_app.py', 'update_runtime.py', 'games/totk/scripts/prepare_shader_seed.py', GFX.format('totk')], {}),
    'switch 2 dumps (Bananza, Pokopia)': (
        ['build_app.py', 'update_runtime.py', 'games/totk/scripts/prepare_ns2_seed.py', GFX.format('totk')],
        {'a runtime change keeps the translations': 'games/totk/scripts/prepare_ns2_seed.py still hashes the runtime binary; to do, as games/oot does'}),
    'oot': (['games/oot/build.py', 'update_runtime.py', 'games/oot/scripts/prepare_ns2_seed.py', GFX.format('oot')], {}),
    'dread': (['games/dread/build.py'], {
        'shader data in its own signed bundle': 'not done for this game yet',
        'pipeline archive built while packaging': 'not done for this game yet',
        'title does not wait for the warm-up': 'not done for this game yet',
        'no program runs inside a new app': 'not done for this game yet',
        'seed and library digests are reused': 'not done for this game yet',
        'repackaging keeps the .app folder': 'not done for this game yet',
        'a runtime change can update quickly': 'not done for this game yet',
        'warm-up lists beside the shader map': 'not done for this game yet',
        'pipeline archive built from the App\'s lists': 'no pipeline archive for this game yet',
    }),
}

BUILD_SCRIPTS = {'games/oot/build.py': 'oot', 'games/dread/build.py': 'dread'}


class PackagingStandard(unittest.TestCase):
    def test_every_game_with_its_own_build_is_checked(self):
        found = {str(p.relative_to(ROOT)) for p in (ROOT / 'games').glob('*/build.py')}
        unknown = sorted(found - set(BUILD_SCRIPTS))
        self.assertEqual(unknown, [], 'a game with its own build.py: add it to PATHS and BUILD_SCRIPTS, and check it against the standards')

    def test_paths_meet_the_standards(self):
        for name, (sources, gaps) in PATHS.items():
            if not (ROOT / sources[0]).is_file():   # a branch without this game
                continue
            text = read(*sources)
            for standard, check in STANDARDS.items():
                with self.subTest(path=name, standard=standard):
                    met = bool(check(text))
                    if standard in gaps:
                        self.assertFalse(met, f'{name} now meets this standard: remove it from the gaps')
                    else:
                        self.assertTrue(met, f'{name} does not meet this standard; meet it, or list it in the gaps with the reason')

    def test_gaps_name_real_standards_and_reasons(self):
        for name, (_, gaps) in PATHS.items():
            for standard, why in gaps.items():
                self.assertIn(standard, STANDARDS, name)
                self.assertTrue(why.strip(), name)


if __name__ == '__main__':
    unittest.main()

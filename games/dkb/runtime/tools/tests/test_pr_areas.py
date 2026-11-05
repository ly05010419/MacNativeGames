"""Area labels for pull requests (dm/tools/pr_areas.py): paths give their areas, a profile's folder its game label with the game's name, and a profile main lacks the
"new game" label. Runs against a temporary main with a small games table and the real dmsh_chain.py; no network or GitHub needed."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pr_areas   # noqa: E402

BOTW = '01007EF00011E000'
REPO = Path(__file__).resolve().parents[3]


class Labels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        (self.root / 'dm/games' / BOTW).mkdir(parents=True)
        (self.root / 'dm/games' / BOTW / 'game.tsv').write_text('')
        (self.root / 'dm/gfx/tools').mkdir(parents=True)
        shutil.copy(REPO / 'dm/gfx/tools/dmsh_chain.py', self.root / 'dm/gfx/tools')   # the real chain file list
        (self.root / 'dm/games/README.md').write_text(f'| [The Legend of Zelda: Breath of the Wild]({BOTW}/README.md) | `{BOTW}` |\n')

    def tearDown(self):
        self.tmp.cleanup()

    def names(self, *paths):
        return set(pr_areas.labels(list(paths), self.root))

    def test_areas(self):
        self.assertEqual(self.names('dm/host/hle_fs.c'), {'host'})
        self.assertEqual(self.names('dm/host/front_mac.m'), {'host', 'mac'})
        self.assertEqual(self.names('dm/gfx/tools/dmsh_build.py'), {'shader chain'})
        self.assertEqual(self.names('dm/gfx/nvn/dmsh_chain.h'), {'nvn', 'shader chain'})
        self.assertEqual(self.names('dm/gfx/nvn/dmsh_abi.h'), {'nvn', 'shader chain'})   # in DMSH_CHAIN's files
        self.assertEqual(self.names('dm/ios/tools/ios_metal.py'), {'ios', 'shader chain'})
        self.assertEqual(self.names('dm/gfx/tools/dmsh_mtlcache.py'), set())   # a dmsh_ tool outside the chain
        self.assertEqual(self.names('scripts/build_ipa_from_nsp.py'), {'packing', 'tools'})
        self.assertEqual(self.names('README.md', '.github/workflows/tests.yml'), {'docs', 'ci'})

    def test_game(self):
        out = pr_areas.labels([f'dm/games/{BOTW}/README.md'], self.root)
        self.assertEqual(set(out), {f'game: {BOTW}', 'docs'})
        self.assertEqual(out[f'game: {BOTW}'][1], 'The Legend of Zelda: Breath of the Wild')

    def test_new_game(self):
        self.assertEqual(self.names('dm/games/0100ACD0163D0000/game.tsv'), {'game: 0100ACD0163D0000', 'new game'})

    def test_patches_are_rewriter(self):
        self.assertIn('rewriter', self.names(f'dm/games/{BOTW}/patches.py'))


if __name__ == '__main__':
    unittest.main()

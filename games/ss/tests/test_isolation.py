"""No profile or rewrite rule outside this folder takes part: the environment entries the copied tools used to read are closed in the code.
The rewrite case needs the real main of Skyward Sword HD 1.0.1 (SS_TEST_MAIN=<extracted ExeFS/main>); it is skipped without it."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
WITNESS = 'open(__import__("os").environ["SS_WITNESS"], "w").write("loaded")\nCUTS = {}\n'
CLOSED = "for games_dir in filter(None, os.environ.get('DM_GAMES_DIR', '').split(os.pathsep)):   # provisional / derived profiles outside the repository\n" \
         "    PATCH_FILES += sorted(glob.glob(os.path.join(games_dir, '*/patches.py')))\n"


class ProfileTests(unittest.TestCase):
    def test_game_profiles_ignore_outside_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            outside = Path(tmp)/'01002DA013484000'; outside.mkdir(); (outside/'game.tsv').write_text('title_id\t01002DA013484000\n')
            sys.path.insert(0, str(GAME/'runtime/games'))
            import game
            with mock.patch.dict(os.environ, DM_GAMES_DIR=tmp, DM_GAMES_FIRST='1'):
                self.assertEqual(game.find_dir('01002DA013484000'), GAME/'runtime/games/01002DA013484000')
                self.assertEqual(game.search_dirs(), [GAME/'runtime/games'])


@unittest.skipUnless(os.environ.get('SS_TEST_MAIN'), 'needs SS_TEST_MAIN, the real main of 1.0.1')
class RewriteTests(unittest.TestCase):
    def rewrite(self, tools, root):
        outside = root/'outside/01002DA013484000'; outside.mkdir(parents=True)
        (outside/'patches.py').write_text(WITNESS)
        witness, out = root/'witness', root/'out'; out.mkdir()
        env = dict(os.environ, DM_GAMES_DIR=str(root/'outside'), DM_GAMES_FIRST='1', PYTHONPATH=str(root/'outside'), SS_WITNESS=str(witness))
        r = subprocess.run([sys.executable, str(tools/'patch_nso.py'), os.environ['SS_TEST_MAIN'], str(out), 'main'], env=env, capture_output=True, text=True)
        return r, witness.exists(), out

    def test_outside_rules_never_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            r, loaded, out = self.rewrite(GAME/'runtime/tools', root)
            self.assertEqual(r.returncode, 0, r.stderr[-800:])
            self.assertFalse(loaded)
            rows = [l for l in (out/'main.cuts.tsv').read_text().splitlines() if not l.startswith('#')]
            self.assertEqual(rows, [])   # the base port has no patches.py: nothing of the game's code is cut

    def test_control_with_the_entry_reopened_loads_them(self):
        """The unmutated control: the copied rewriter as it was (the two lines restored) does load the outside rule, so the test above can see one."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copytree(GAME/'runtime/tools', root/'dm/tools'); shutil.copytree(GAME/'runtime/games', root/'dm/games')
            p = root/'dm/tools/patch_nso.py'; s = p.read_text()
            anchor = "PATCH_FILES = [os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sdk_patches.py')]"
            line = next(l for l in s.splitlines(keepends=True) if l.startswith(anchor))
            p.write_text(s.replace(line, line+CLOSED, 1))
            r, loaded, _ = self.rewrite(root/'dm/tools', root)
            self.assertTrue(loaded, r.stderr[-800:])


if __name__ == '__main__':
    unittest.main()

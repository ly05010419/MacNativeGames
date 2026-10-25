"""The button icon tools build_app.py puts into the app (button_icons_kit; app/launcher.m runs them at start, docs/mods.md):
the scripts in the repo layout, kit.txt with the tool and an id that changes with the scripts and the RomFS, and the tool
starting from the kit alone."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import build_app

REPO = Path(__file__).resolve().parents[1]


def work_tree(root, index=b'index'):
    """A work folder with the repo's scripts and a stand-in RomFS index (work/totk, work/botw as prepare_work makes them)."""
    tree = root / 'work/totk'
    (tree / 'runtime').mkdir(parents=True)
    (tree / 'scripts').symlink_to(REPO / 'games/totk/scripts')
    (tree / 'runtime/controller_map.h').symlink_to(REPO / 'games/totk/runtime/controller_map.h')
    (root / 'work/botw').mkdir()
    (root / 'work/botw/scripts').symlink_to(REPO / 'games/botw/scripts')
    (tree / 'extracted/botw190/romfs').mkdir(parents=True)
    (tree / 'extracted/botw190/romfs/runtime-index.bin').write_bytes(index)
    return tree


def kit_info(res):
    return dict(line.split('=', 1) for line in (res / 'button-icons/kit.txt').read_text().splitlines())


class ButtonIconKit(unittest.TestCase):
    def test_kit_layout_and_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            tree = work_tree(tmp / 'a')
            build_app.button_icons_kit(tree, tmp / 'res', 'botw190', 'botw')
            kit = tmp / 'res/button-icons'
            for rel in ('games/botw/scripts/botw_prompts.py', 'games/totk/scripts/prompt_layout.py', 'games/totk/runtime/controller_map.h'):
                self.assertTrue((kit / rel).is_file(), rel)
            info = kit_info(tmp / 'res')
            self.assertEqual(info['tool'], 'games/botw/scripts/botw_prompts.py')
            self.assertEqual(info['python'], str(tree.resolve() / '.venv/bin/python'))
            # another RomFS, or a build overlay, gives another id: a set made for the earlier app is not reused
            other = work_tree(tmp / 'b', index=b'other')
            build_app.button_icons_kit(other, tmp / 'res2', 'botw190', 'botw')
            self.assertNotEqual(kit_info(tmp / 'res2')['id'], info['id'])
            (other / 'extracted/botw190/romfs-patches').mkdir()
            (other / 'extracted/botw190/romfs-patches/manifest.tsv').write_text('# none\n')
            (other / 'extracted/botw190/romfs/runtime-index.bin').write_bytes(b'index')
            build_app.button_icons_kit(other, tmp / 'res3', 'botw190', 'botw')
            self.assertNotEqual(kit_info(tmp / 'res3')['id'], info['id'])

    def test_totk_kit_names_its_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            tree = work_tree(tmp)
            build_app.button_icons_kit(tree, tmp / 'res', 'botw190', 'totk')
            self.assertEqual(kit_info(tmp / 'res')['tool'], 'games/totk/scripts/make_button_icons.py')
            self.assertFalse((tmp / 'res/button-icons/games/botw').exists())

    def test_tools_start_from_the_kit(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build_app.button_icons_kit(work_tree(tmp), tmp / 'res', 'botw190', 'botw')
            for tool in ('games/botw/scripts/botw_prompts.py', 'games/totk/scripts/make_button_icons.py'):
                run = subprocess.run([sys.executable, '-E', '-s', tmp / 'res/button-icons' / tool, '--help'], capture_output=True, text=True)
                if run.returncode and 'fontTools' in run.stderr:
                    self.skipTest('needs fontTools (requirements.txt)')
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn('--face-buttons', run.stdout)


if __name__ == '__main__':
    unittest.main()

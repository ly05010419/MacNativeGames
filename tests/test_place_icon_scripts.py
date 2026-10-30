"""The app carries the button icon scripts (build_app.place_icon_scripts): the settings panel's Apply button icons runs
them from the app (runtime/icons_apply.m), so a Mac app works without this repository."""
import ast
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_app

SCRIPTS = build_app.GAME / 'scripts'
NAMES = ('make_button_icons.py', 'make_mod_manifest.py', 'make_romfs_patches.py')


class PlaceIconScripts(unittest.TestCase):
    def test_copies_the_scripts(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = Path(tmp) / 'Contents/Resources/totk'
            build_app.place_icon_scripts(res)
            for name in NAMES:
                self.assertEqual((res / 'scripts' / name).read_bytes(), (SCRIPTS / name).read_bytes())

    def test_the_scripts_need_nothing_else_from_the_repository(self):
        """Every import of the two scripts is the standard library, fontTools or one of the two."""
        local = {name[:-3] for name in NAMES}
        for name in NAMES:
            tree = ast.parse((SCRIPTS / name).read_text())
            modules = {a.name.split('.')[0] for n in ast.walk(tree) if isinstance(n, ast.Import) for a in n.names}
            modules |= {n.module.split('.')[0] for n in ast.walk(tree) if isinstance(n, ast.ImportFrom) and n.module}
            with self.subTest(script=name):
                self.assertEqual(modules - local - set(sys.stdlib_module_names) - {'fontTools'}, set())

    def test_place_runtime_places_them_for_tears_of_the_kingdom_only(self):
        for game, placed in (('totk', True), ('botw', False)):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(build_app, 'clone'), patch.object(build_app, 'place_icon_scripts') as place:
                app = Path(tmp) / 'x.app'
                build_app.place_runtime(Path(tmp), app, 'v143', Path(tmp), game=game, lists=False)
                self.assertEqual(place.called, placed)
                if placed:
                    place.assert_called_once_with(app / 'Contents/Resources/totk')


if __name__ == '__main__':
    unittest.main()

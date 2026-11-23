"""An iOS build's libraries are remade when the work folder was built somewhere else.

2026-11-19: Breath of the Wild's iPhone build reused a work folder moved on 2026-11-09 (mac-APFS -> /Volumes/开发); its
opus CMake cache still named the old path and CMake refused it ("CMakeCache.txt directory ... is different than the
directory ..."). Every game's build_deps.py keeps build/.built-at and remakes build/ when it differs or is missing.
The guard is run here on its own, as the four scripts carry it.
"""
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = [ROOT / f'games/{g}/ios/tools/build_deps.py' for g in ('totk', 'oot', 'dread', 'tmnt')]
LINE = "src = root/'src'; build = root/'build'; src.mkdir(parents=True, exist_ok=True); build.mkdir(exist_ok=True)\n"


def guard_of(script):
    text = script.read_text()
    start = text.index(LINE)
    end = text.index("_where.write_text(str(build.resolve()))\n", start)
    return text[start:end] + "_where.write_text(str(build.resolve()))\n"


class MovedWorkFolder(unittest.TestCase):
    def test_every_script_carries_the_same_guard(self):
        guards = {s: guard_of(s) for s in SCRIPTS}
        self.assertEqual(len(set(guards.values())), 1, 'the four build_deps.py differ in their guard')

    def run_guard(self, root):
        exec(guard_of(SCRIPTS[0]), {'root': root, '__builtins__': __builtins__})

    def test_a_tree_built_elsewhere_or_unrecorded_is_remade(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'ios-deps'
            stale = root / 'build/opus-ninja/CMakeCache.txt'
            stale.parent.mkdir(parents=True)
            stale.write_text('CMAKE_CACHEFILE_DIR:INTERNAL=/old/place\n')
            self.run_guard(root)   # no record of where it was built: remade
            self.assertFalse(stale.exists())
            self.assertEqual((root / 'build/.built-at').read_text(), str((root / 'build').resolve()))
            kept = root / 'build/libvpx/libvpx.a'
            kept.parent.mkdir(parents=True)
            kept.write_text('x')
            self.run_guard(root)   # built here: kept
            self.assertTrue(kept.exists())
            (root / 'build/.built-at').write_text('/Volumes/mac-APFS/old/ios-deps/build')
            self.run_guard(root)   # built at another path: remade
            self.assertFalse(kept.exists())

    def test_the_guard_comes_before_the_first_cmake(self):
        for s in SCRIPTS:
            text = s.read_text()
            first = min(i for i in (text.find("run(['cmake'"), text.find("run([cmake")) if i >= 0)   # tmnt: run([cmake,...
            self.assertLess(text.index('_where = build'), first, s)


if __name__ == '__main__':
    unittest.main()

"""zeldaVR's hooks (zeldaVR/runtime/vr_camera.h): supported Mac games define their features, and every
one the header declares has a weak default in zeldaVR/gfx/vr_defaults.c (in every build: the Makefile's GFX_SRC, the
iOS project's zeldaVR/gfx), and no source declares one weak_import. ld fails on an undefined weak_import symbol: after
d1509b4b that stopped the 1.0.0, 1.1.2, 1.2.1, Breath of the Wild and iPhone / iPad links (PR #1592)."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[1]
HEADER = REPO / 'zeldaVR/runtime/vr_camera.h'
DEFAULTS = REPO / 'zeldaVR/gfx/vr_defaults.c'
SOURCES = [*(REPO / 'games/totk/runtime').glob('*.[chm]'), *(REPO / 'games/totk/third_party/d2r_gfx/gfx/nvn').glob('*.[chm]'),
           *(REPO / 'zeldaVR/gfx').glob('*.[chm]'), *(REPO / 'games/totk/ios/Sources').glob('*.[chm]')]


def hooks():
    text = HEADER.read_text()
    block = text[text.index('Optional runtime hooks:'):]
    return re.findall(r'^\w+\s+(runtime_\w+)\s*\(', block, re.M)


class VrHooks(unittest.TestCase):
    def test_header_names_the_hooks(self):
        self.assertEqual(sorted(hooks()), ['runtime_aspect_vr', 'runtime_camera_distance_available', 'runtime_camera_distance_cm',
                                           'runtime_camera_distance_set', 'runtime_vr_base_view', 'runtime_vr_find_head'])

    def test_every_hook_has_a_weak_default(self):
        text = DEFAULTS.read_text()
        for name in hooks():
            self.assertRegex(text, rf'__attribute__\(\(weak\)\)\s+\w+\s+{name}\s*\([^;]*\)\s*\{{', name)

    def test_no_source_declares_a_hook_weak_import(self):
        for path in SOURCES:
            for name in hooks():
                self.assertNotRegex(path.read_text(errors='replace'), rf'{name}\s*\([^;]*\)\s*__attribute__\(\(weak_import\)\)',
                                    f'{path.relative_to(REPO)}: {name}')

    def test_only_supported_games_define_each_hook_strongly(self):
        """Each game supplies its own feature; other builds keep the weak defaults."""
        strong = {name: [p.name for p in SOURCES if p != DEFAULTS and re.search(
            rf'^(?!.*__attribute__\(\(weak\)\))\w[\w\s\*]*\b{name}\s*\([^;{{]*\)\s*\{{', p.read_text(errors='replace'), re.M)]
            for name in hooks()}
        self.assertEqual(strong['runtime_aspect_vr'], ['aspect_patch.h'])
        for name in hooks():
            if name.startswith('runtime_camera_distance_'):
                self.assertEqual(sorted(strong[name]), ['botw_vr_camera.h', 'game_patches_v143.c'], name)
            elif name == 'runtime_vr_base_view':
                self.assertEqual(strong[name], ['botw_vr_camera.h'], name)
            elif name != 'runtime_aspect_vr':
                self.assertEqual(strong[name], ['game_patches_v143.c'], name)


if __name__ == '__main__':
    unittest.main()

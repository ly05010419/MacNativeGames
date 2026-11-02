"""games/totk/Makefile and the shared sources (S24.1, after zeldaVR, MNG d1509b4): every build links with each
weak_import function the shared code references either defined in what it links or named by -Wl,-U on its link line;
the -U list names exactly those functions and no line uses a general option that leaves any symbol unresolved; and
every call of the five zeldaVR functions is guarded, so the null they read as on the other builds is never called.
Read, not run: the real links (pure upstream before and after, BotW 1.6.0 / 1.9.0, TOTK base / 1.1.2 / 1.2.1 / 1.4.3)
are in the story."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[1]
TOTK = REPO / 'games/totk'
MAKEFILE = (TOTK / 'Makefile').read_text()
SHARED = [TOTK / 'runtime/settings_panel.h', TOTK / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m',
          *sorted((REPO / 'zeldaVR/gfx').glob('*.m'))]
BUILDS = ('v0', 'v112', 'v121', 'v143', 'botw160', 'botw190')    # v0: the base game, `make runtime`'s build/totk-native
FIVE = ('runtime_aspect_vr', 'runtime_camera_distance_available', 'runtime_camera_distance_cm',
        'runtime_camera_distance_set', 'runtime_vr_find_head')


def weak_names():
    found = set()
    for path in SHARED:
        found |= set(re.findall(r'(\w+)\s*\([^;()]*\)\s*__attribute__\(\(weak_import\)\)', path.read_text()))
    return found


def recipe(build):
    lines = MAKEFILE.split('\n')
    target = 'build/totk-native:' if build == 'v0' else f'build/totk-native-{build}:'
    i = next(n for n, line in enumerate(lines) if line.startswith(target))
    return lines[i + 1]


def variable(name):
    m = re.search(rf'^{name} := ((?:.*\\\n)*.*)$', MAKEFILE, re.M)
    return ' '.join(m[1].replace('\\\n', ' ').split()) if m else ''


def linked_text(build):
    """The sources the build's line names, the graphics and zeldaVR sources every build links, and the local headers
    they include (followed through)."""
    line = recipe(build)
    paths = [TOTK / p for p in re.findall(r'(runtime/\w+\.(?:c|m))\b', line)]
    paths += sorted((TOTK / 'third_party/d2r_gfx/gfx/nvn').glob('*.[cm]'))
    paths += sorted((REPO / 'zeldaVR/gfx').glob('*.[cm]'))
    seen, text = set(), []
    while paths:
        path = paths.pop()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        body = path.read_text(errors='replace')
        text.append(body)
        paths += [path.parent / h for h in re.findall(r'#include "([\w./]+\.h)"', body)]
    return '\n'.join(text)


def defined(name, text):
    return re.search(rf'^[A-Za-z_][^;\n(]*\b{name}\s*\([^;{{]*\)\s*\{{', text, re.M) is not None


class WeakLinks(unittest.TestCase):
    def test_the_five_are_the_weak_functions_the_shared_code_references(self):
        self.assertTrue(set(FIVE) <= weak_names(), weak_names())
        self.assertEqual(variable('VR_WEAK').split(), [f'-Wl,-U,_{name}' for name in FIVE])

    def test_each_build_defines_them_or_lets_exactly_them_stay_undefined(self):
        names = weak_names()
        for build in BUILDS:
            with self.subTest(build=build):
                line = recipe(build).replace('$(VR_WEAK)', variable('VR_WEAK'))
                text = linked_text(build)
                missing = {n for n in names if not defined(n, text)}
                allowed = set(re.findall(r'-Wl,-U,_(\w+)', line))
                self.assertLessEqual(missing, allowed)                  # each one it lacks may stay undefined
                self.assertLessEqual(allowed, set(FIVE))                 # and nothing else may
                self.assertNotRegex(line, r'-undefined|dynamic_lookup|-flat_namespace')
        self.assertNotIn('$(VR_WEAK)', recipe('v143'))                   # 1.4.3 defines them all
        self.assertTrue(all(defined(n, linked_text('v143')) for n in FIVE))

    def test_every_call_is_guarded(self):
        nvn = (TOTK / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertIn('if (runtime_vr_find_head) runtime_vr_find_head(v);', nvn)
        self.assertIn('if (runtime_camera_distance_set) runtime_camera_distance_set(v);', nvn)
        vr = (REPO / 'zeldaVR/gfx/totk_vr.m').read_text()
        self.assertLess(vr.index('!runtime_aspect_vr) return;'), vr.index('runtime_aspect_vr(a, k)'))
        panel = (TOTK / 'runtime/settings_panel.h').read_text()
        gate = panel.index('if(!runtime_camera_distance_available||!runtime_camera_distance_available())return;')
        start = panel.rindex('\nstatic ', 0, gate)                       # the function the gate opens
        end = panel.index('\n}\n', gate)
        for call in ('runtime_camera_distance_cm()', 'runtime_camera_distance_set(v)'):
            uses = [m.start() for m in re.finditer(re.escape(call), panel)]
            self.assertTrue(uses and all(gate < u < end for u in uses), call)   # only after the group's gate
            self.assertTrue(all(u > start for u in uses))
        for name in FIVE:                                                # no other file calls them
            callers = [p.name for p in (TOTK / 'runtime').glob('*.[chm]')
                       if re.search(rf'\b{name}\s*\(', p.read_text())]
            self.assertLessEqual(set(callers), {'settings_panel.h', 'game_patches_v143.c', 'aspect_patch.h'}, name)


if __name__ == '__main__':
    unittest.main()

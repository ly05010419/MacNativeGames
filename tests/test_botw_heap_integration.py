"""games/totk/runtime/game_patches_botw.c and games/totk/Makefile as written (read, not run) against botw_heap.c's
plan (E9 S9.5): prepared before the first patch writes and applied right after the output size, each Mac build with its
own version's table and botw_heap.c linked, and patch_output_res naming the two 1.6.0 heap sites only in its table of
checked words. The plan itself, on images: tests/test_botw_heap.py."""
from pathlib import Path
import unittest

RUNTIME = Path(__file__).resolve().parents[1] / 'games/totk/runtime'


def function(code, header):
    """The body of the function whose definition starts with header (to its closing brace at column 0)."""
    start = code.index(header + ' {')
    return code[start:code.index('\n}\n', start)]


class Integration(unittest.TestCase):
    """game_patches_botw.c and the Makefile as written (read, not run): the plan is prepared before the first patch
    writes and applied right after the output size, each Mac build takes its own table and links botw_heap.c, and
    patch_output_res names the two heap sites only in its table of checked words."""
    code = (RUNTIME / 'game_patches_botw.c').read_text()
    MAC = '#if (defined(GAME_BOTW160) || defined(GAME_BOTW190)) && !TARGET_OS_IPHONE'

    def test_prepare_comes_first_and_apply_after_the_output_size(self):
        lines = [line.strip() for line in function(self.code, 'void runtime_game_patches(Module *m)').splitlines()[1:]]
        writers = ('runtime_survey_init(', 'patch_output_res(', 'patch_render_1080(', 'patch_phone_render(',
                   'botw_aspect_install(', 'patch_realtime(', 'excl_acquire(', 'memcpy(')
        first_writer = next(i for i, line in enumerate(lines) if any(w in line for w in writers))
        self.assertEqual(lines.index('heap_prepare(m);'), first_writer - 1)
        self.assertTrue(lines[lines.index('heap_apply(m);') - 1].startswith('if (!patch_output_res(m))'))
        self.assertEqual(sum('heap_prepare(' in line or 'heap_apply(' in line for line in lines), 2)

    def test_the_output_size_only_checks_the_heap_sites(self):
        body = function(self.code[self.code.index('#ifdef GAME_BOTW160\n/* Output size'):], 'static int patch_output_res(Module *m)')
        table = body[body.index('check[] = {'):body.index('};', body.index('check[] = {'))]
        for site in ('0xe536dc', '0xe53fa0'):
            self.assertEqual((body.count(site), table.count(site)), (1, 1), site)

    def test_the_wrappers_call_the_plan_with_their_versions_table(self):
        mac = self.code[self.code.index(self.MAC):]
        mac = mac[:mac.index('\n#else\nstatic void heap_prepare')]
        self.assertIn('#ifdef GAME_BOTW160\n#define HEAP_TABLE botw_heap_160\n#else\n#define HEAP_TABLE botw_heap_190\n#endif', mac)
        self.assertIn('botw_heap_prepare(&HEAP_TABLE, m->base, m->size, m->text_end', function(mac, 'static void heap_prepare(Module *m)'))
        self.assertIn('botw_heap_apply(m->base, &g_heap)', mac)

    def test_both_mac_builds_link_the_plan(self):
        makefile = (RUNTIME.parent / 'Makefile').read_text().splitlines()
        for define in ('-DGAME_BOTW160=1', '-DGAME_BOTW190=1'):
            with self.subTest(build=define):
                link = [line for line in makefile if define in line and 'runtime/loader.c' in line]
                self.assertEqual(len(link), 1)
                self.assertIn('runtime/botw_heap.c', link[0])



if __name__ == '__main__':
    unittest.main()

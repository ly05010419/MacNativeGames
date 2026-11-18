"""Ocarina of Time on the Mac offers 4K (user 2026-11-18: "设置里 添加 4k（2k放大）和 原生4k 分辨率支持").

Two things in the game stood in the way, both set at its start (games/oot/runtime/game_patches_ns2.c):
- the window textures were 2560x1440, so a 3840x2160 canvas had nothing to be presented from
  (patch_oot_window_textures makes them 3840x2160);
- the render targets come from one pool of 305 MiB that cannot grow, too small for a 3840x2160 3D picture: the
  allocator went on with a null range (patch_oot_render_target_pool makes it 2.25 times that).
The two functions are compiled from the source and run on a stand-in for the game's code; the settings row, the canvas
list and the saved choice are checked against each other."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'games/oot/runtime/game_patches_ns2.c'
SETTINGS = ROOT / 'games/oot/runtime/settings_game.h'

WINDOW_SITE, POOL_SITE = 0x1202ee0, 0x36aeaa0
GAME_WINDOW = (0xd2814008, 0xf2c0b408)      # mov x8, #0xa00; movk x8, #0x5a0, lsl #32  (2560x1440)
GAME_POOL = 0xf942de6a                      # ldr x10, [x19, #0x5b8]
GAME_POOL_BYTES = 319815680                 # read from the running game (keep/tools/oot-fps90/tools/rtpool.py)


def function(source, name):
    """The text of one function defined at the start of a line: up to its closing brace at the start of a line, or the
    whole line when the function is written on one."""
    m = (re.search(r'^static [^\n(]*\b%s\([^\n]*\{\n.*?^\}\n' % re.escape(name), source, re.S | re.M)
         or re.search(r'^static [^\n(]*\b%s\([^\n]*\)\{[^\n]*\}[^\n]*\n' % re.escape(name), source, re.M))
    if not m:
        raise AssertionError('%s is not in the source' % name)
    return m.group(0)


HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint8_t *base; } Module;
static void runtime_fail(const char *fmt, ...) { printf("refused: %%s\n", fmt); exit(3); }
%(functions)s
#if defined(__aarch64__) && defined(__APPLE__)
#define HAVE_ASSEMBLER 1
/* what the assembler makes of the instructions the comments name */
__asm__(".text\n.globl _expected_words\n.p2align 2\n_expected_words:\n"
        " mov x8, #0xf00\n movk x8, #0x870, lsl #32\n mov x10, #0x2ae40000\n");
extern const uint32_t expected_words[3];
#endif
int main(int argc, char **argv) {
    Module m = {calloc(1, 0x36aeab0)};
    uint32_t window[2] = {0x%(w0)08x, 0x%(w1)08x}, pool = 0x%(pool)08x, got[3];
    if (argc > 1 && !strcmp(argv[1], "other-window")) window[1] ^= 0x20;
    if (argc > 1 && !strcmp(argv[1], "other-pool")) pool ^= 0x400;
    memcpy(m.base + 0x%(window_site)x, window, 8); memcpy(m.base + 0x%(pool_site)x, &pool, 4);
    patch_oot_window_textures(&m);
    patch_oot_render_target_pool(&m);
    memcpy(got, m.base + 0x%(window_site)x, 8); memcpy(got + 2, m.base + 0x%(pool_site)x, 4);
    printf("words %%08x %%08x %%08x\n", got[0], got[1], got[2]);
#ifdef HAVE_ASSEMBLER
    if (argc > 1 && !strcmp(argv[1], "assembler"))
        printf("assembler %%08x %%08x %%08x\n", expected_words[0], expected_words[1], expected_words[2]);
#endif
    return 0;
}
'''

SAVED_HARNESS = r'''
#include <stdio.h>
static int stored, have;
static int settings_read_int(const char *name, int *value) { (void)name; if (!have) return 0; *value = stored; return 1; }
%(function)s
int main(void) {
    have = 0; printf("none=%%d", settings_oot_canvas_saved());
    have = 1;
    for (stored = -1; stored <= 10; stored++) printf(" %%d=%%d", stored, settings_oot_canvas_saved());
    puts("");
    return 0;
}
'''


class Compiled(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('cc')
        if not cls.compiler:
            raise unittest.SkipTest('A C compiler is required')
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)

    def build(self, name, text):
        source, program = Path(self.folder.name) / (name + '.c'), Path(self.folder.name) / name
        source.write_text(text, encoding='utf-8')
        done = subprocess.run([self.compiler, '-std=gnu11', '-Wall', '-Wno-unused-function', '-o', str(program), str(source)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        return program

    def run_program(self, program, *arguments, env=None):
        base = {'PATH': '/usr/bin:/bin'}
        base.update(env or {})
        return subprocess.run([str(program), *arguments], capture_output=True, text=True, env=base)


class GamePatches(Compiled):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        source = PATCHES.read_text(encoding='utf-8')
        cls.text = HARNESS % {
            'functions': function(source, 'patch_oot_window_textures') + function(source, 'patch_oot_render_target_pool'),
            'w0': GAME_WINDOW[0], 'w1': GAME_WINDOW[1], 'pool': GAME_POOL, 'window_site': WINDOW_SITE, 'pool_site': POOL_SITE}

    def setUp(self):
        self.program = self.build('patches', self.text)

    def words(self, *arguments, env=None):
        done = self.run_program(self.program, *arguments, env=env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return dict(line.split(' ', 1) for line in done.stdout.strip().split('\n'))

    def test_window_textures_3840x2160_and_a_pool_of_2_25_times_the_games(self):
        out = self.words('assembler')
        # mov x8, #0xf00; movk x8, #0x870, lsl #32 (3840x2160); mov x10, #0x2ae40000
        self.assertEqual(out['words'], 'd281e008 f2c10e08 d2a55c8a')
        if 'assembler' in out:      # an arm64 host: the assembler's own encoding of those instructions
            self.assertEqual(out['words'], out['assembler'])
        pool = (int(out['words'].split()[2], 16) >> 5 & 0xffff) << 16
        self.assertEqual(pool * 4, GAME_POOL_BYTES * 9)      # the pixels of 3840x2160 over 2560x1440
        self.assertGreaterEqual(pool, 371 << 20)             # in use with a 3840x2160 3D picture (res4k-9)

    def test_the_games_own_sizes_on_request(self):
        out = self.words(env={'OOT_FB_SIZE': '2560x1440', 'OOT_RT_POOL_MB': '305'})
        self.assertEqual(out['words'], '%08x %08x %08x' % (GAME_WINDOW + (GAME_POOL,)))

    def test_other_sizes_on_request(self):
        out = self.words(env={'OOT_FB_SIZE': '1920x1080', 'OOT_RT_POOL_MB': '512'})
        self.assertEqual(out['words'], '%08x %08x %08x' % (0xd2800008 | 1920 << 5, 0xf2c00008 | 1080 << 5, 0xd2a00000 | 0x2000 << 5 | 10))

    def test_the_ios_export_keeps_the_games_code(self):
        # the iOS package's game code is exported by the Mac runtime: Mac-only patches must not go with it
        out = self.words(env={'TOTK_EXPORT_IOS': '1'})
        self.assertEqual(out['words'], '%08x %08x %08x' % (GAME_WINDOW + (GAME_POOL,)))

    def test_other_game_code_is_refused(self):
        for case in ('other-window', 'other-pool'):
            done = self.run_program(self.program, case)
            self.assertEqual(done.returncode, 3, case + ': ' + done.stdout)
            self.assertIn('differs from verified update 131072', done.stdout)

    def test_sizes_out_of_range_are_refused(self):
        for env in ({'OOT_FB_SIZE': '4096x2304'}, {'OOT_FB_SIZE': '1280x720'}, {'OOT_FB_SIZE': 'big'},
                    {'OOT_RT_POOL_MB': '100'}, {'OOT_RT_POOL_MB': '4096'}, {'OOT_RT_POOL_MB': 'more'}):
            done = self.run_program(self.program, env=env)
            self.assertEqual(done.returncode, 3, '%s: %s' % (env, done.stdout))

    def test_both_run_on_the_mac_with_the_games_other_patches(self):
        source = PATCHES.read_text(encoding='utf-8')
        body = source[source.index('void runtime_game_patches(Module *m) {'):]
        mac = body[body.index('#elif defined(GAME_OOT) && !TARGET_OS_IPHONE'):]
        mac = mac[:mac.index('#e', 1)]
        self.assertIn('patch_oot_window_textures(m);', mac)
        self.assertIn('patch_oot_render_target_pool(m);', mac)


class ResolutionRow(Compiled):
    def setUp(self):
        self.patches = PATCHES.read_text(encoding='utf-8')
        self.settings = SETTINGS.read_text(encoding='utf-8')
        row = function(self.settings, 'settings_oot_output_row')
        self.labels = re.findall(r'settings_text\("([^"]+)","([^"]+)"\)', row)[1:]      # the first pair is the row's title
        self.offered = [int(n) for n in re.findall(r'@(\d+)', re.search(r'@\[(@\d+(?:,@\d+)*)\]', row).group(1))]
        entries = re.search(r'runtime_oot_canvas_set\(int index, int live\) \{\s*static const struct \{[^}]*\} list\[\] = \{(.*?)\};',
                            self.patches, re.S).group(1)
        self.canvases = [(int(w), int(h), float(s.rstrip('f'))) for w, h, s in re.findall(r'\{(\d+),(\d+),([0-9.]+f?)\}', entries)]
        sizes = re.search(r'settings_oot_canvases\[\] = \{(.*?)\};', self.settings).group(1)
        self.windows = [(int(w), int(h)) for w, h in re.findall(r'\{(\d+),(\d+)\}', sizes)]

    def test_the_row_offers_both_4k_choices(self):
        self.assertEqual(self.offered, [0, 1, 2, 3, 4, 5, 6, 7])
        self.assertEqual(len(self.labels), len(self.offered))
        self.assertEqual(self.labels[6], ('4K（2K 放大）', '4K (from 2K)'))
        self.assertEqual(self.labels[7], ('4K（3840×2160 原生）', '4K (3840×2160 native)'))
        self.assertEqual(self.canvases[6][:2], (3840, 2160))
        self.assertAlmostEqual(self.canvases[6][2] * 3840, 2560, delta=1)       # the 3D picture: 2560x1440
        self.assertEqual(self.canvases[7], (3840, 2160, 1.0))

    def test_every_offered_canvas_fits_the_window_textures(self):
        size = re.search(r'patch_oot_window_textures\(Module \*m\) \{\s*const char \*e = getenv\("OOT_FB_SIZE"\); unsigned w = (\d+), h = (\d+);',
                         self.patches)
        width, height = int(size.group(1)), int(size.group(2))
        for index in self.offered:
            self.assertLessEqual(self.canvases[index][0], width, index)
            self.assertLessEqual(self.canvases[index][1], height, index)

    def test_the_window_follows_the_canvas_of_every_offered_choice(self):
        for index in self.offered:
            self.assertEqual(self.windows[index], self.canvases[index][:2], index)

    def saved(self, text):
        done = self.run_program(self.build('saved', SAVED_HARNESS % {'function': text}))
        self.assertEqual(done.returncode, 0, done.stderr)
        return dict(pair.split('=') for pair in done.stdout.split())

    def test_a_saved_choice_comes_back_at_the_start(self):
        text = function(self.settings, 'settings_oot_canvas_saved')
        got = self.saved(text)
        self.assertEqual(got['none'], '1')                       # no choice saved: 720p native
        for index in self.offered:
            self.assertEqual(got[str(index)], str(index), index)
        self.assertEqual(got['8'], '0')                          # the old number of "720p from 480p"
        for other in ('-1', '9', '10'):
            self.assertEqual(got[other], '1', other)
        # the check sees the change: with the range before 2026-11-18 both 4K choices fell back to the default
        before = self.saved(text.replace('v<=7', 'v<6'))
        self.assertEqual((before['6'], before['7']), ('1', '1'))


if __name__ == '__main__':
    unittest.main()

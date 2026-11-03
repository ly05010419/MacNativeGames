"""tools/builder/controls_file.py (E6 S6.2b): parse() against runtime/controller_map.h itself. A harness built with
xcrun clang reads length-prefixed texts on stdin and prints, for each, controller_map_parse's line, error, map and
modes; parse() must give the same for every fixture and for thousands of random texts made of the names, the modes, '=',
'#', the white space C and Python disagree on, NUL, a BOM, the Kelvin sign, 'ſ' and random bytes. The seed is fixed plus
one per run, printed on a failure."""
import json
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from tools.builder import controls_file as cf

RUNTIME = Path(__file__).resolve().parents[1] / 'games/totk/runtime'
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "controller_map.h"
int main(void) {
    unsigned char size[4];
    while (fread(size, 1, 4, stdin) == 4) {
        unsigned n = size[0] | size[1] << 8 | size[2] << 16 | (unsigned)size[3] << 24;
        char *text = calloc(n + 1, 1);
        if (fread(text, 1, n, stdin) != n) return 2;
        uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(map, modes);
        const char *error = ""; int line = controller_map_parse(text, map, modes, &error);
        printf("{\"line\": %d, \"error\": \"", line);
        for (const char *e = error; *e; e++) printf(*e == '"' ? "\\\"" : "%c", *e);
        printf("\", \"map\": [");
        for (int i = 0; i < CONTROLLER_BUTTONS; i++) printf("%s%u", i ? ", " : "", map[i]);
        printf("], \"modes\": [");
        for (int i = 0; i < CONTROLLER_BUTTONS; i++) printf("%s%u", i ? ", " : "", modes[i]);
        printf("]}\n");
        free(text);
    }
    return 0;
}
'''
WORDS = [n.encode() for n in cf.PHYSICAL + cf.SWITCH] + [b'none', b'hold', b'tap', b'toggle', b'aim', b'SOUTH', b'Zr',
                                                          b'NONE', b'Tap', b'nothing', b'a1']
PHYS = [n.encode() for n in cf.PHYSICAL] + [b'SOUTH', b'l2x']
SWITCHES = [n.encode() for n in cf.SWITCH] + [b'none', b'Zr', b'q']
MODES = [b'hold', b'tap', b'toggle', b'aim', b'TAP', b'sometimes']
SEPARATORS = [b' ', b'\t', b'', b'  ', b'\r', b' \r', b'\xc2\xa0', b'\x0b', b'\x0c', b'\x1c', b'\xc2\x85',
              b'\xe2\x80\xa8', b'\x00', b'\n', b' # ', b'\t\r ']
NOISE = [b'=', b'#', b' ', b'\t', b'\r', b'\n', b'\xc2\xa0', b'\x0b', b'\x0c', b'\x1c', b'\xc2\x85', b'\xe2\x80\xa8',
         b'\x00', b'\xef\xbb\xbf', b'\xe2\x84\xaa', b'\xc5\xbf', b'  ', b' = ', b'\n']


def runtime(texts, binary):
    stdin = b''.join(struct.pack('<I', len(t)) + t for t in texts)
    out = subprocess.run([str(binary)], input=stdin, capture_output=True, timeout=120, check=True).stdout
    return [json.loads(line) for line in out.decode().splitlines()]


def mine(text):
    p = cf.parse(text)
    as_map = [0 if t == cf.NONE else 1 << t for t in p.map]
    return {'line': p.line, 'error': p.error, 'map': as_map, 'modes': list(p.modes)}


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class AgainstTheHeader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        source, cls.binary = Path(cls.tmp) / 'harness.c', Path(cls.tmp) / 'harness'
        source.write_text(HARNESS)
        build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Werror', f'-I{RUNTIME}', str(source), '-o',
                                str(cls.binary)], capture_output=True, text=True, timeout=120)
        if build.returncode:
            shutil.rmtree(cls.tmp)
            raise AssertionError(build.stderr[-2000:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def same(self, texts, seed=None):
        expected = runtime(texts, self.binary)
        for text, want in zip(texts, expected):
            self.assertEqual(mine(text), want, f'text {text!r} (seed {seed})')

    def test_fixtures(self):
        self.same([b'', b'\n\n', b'# only a comment\n', b'south = a\nsouth = b\n', b'SOUTH = A  # caps\r\n',
                   b'\tl1\t=\tr\ttap\t', b'east = none', b'east = none hold', b'east = none tap', b'l2 = zr aim r2',
                   b'l2 = zr aim', b'l2 = zr aim l2', b'l2 = zr aim r2 extra', b'south = a toggle extra',
                   b'south a', b'nowhere = a', b'south = q', b'south = a sometimes', b'touchpad = minus',
                   b'mouse_left = zl\nmouse_right = none', b'\xef\xbb\xbfsouth = a', b'south\xc2\xa0= a',
                   b'south = a\x00garbage = !!', b'south = \xe2\x84\xaa', b'\xc5\xbfouth = a', b'south =',
                   b'= a', b'south = a # tap', b'south = a\x0b', b'south = a\rnorth = b', b'south\r= a',
                   b'south =\ra', b'south = a\r tap', b'l2 = zr aim\rr2', b'south\x0b= a', b'south =\xc2\xa0a',
                   b'l2 = zr\x00 aim r2', b'south = a\t\ttoggle\t'])

    def test_random_texts(self):
        for seed in (20261026, random.randrange(1 << 30)):
            rng, texts = random.Random(seed), []
            for _ in range(3000):
                if rng.random() < 0.5:                       # a near-valid line, any separator from the alphabet
                    sep = lambda: rng.choice(SEPARATORS)  # noqa: E731
                    pieces = [rng.choice(PHYS), sep(), b'=', sep(), rng.choice(SWITCHES), sep(), rng.choice(MODES),
                              sep(), rng.choice(PHYS)][:rng.randint(3, 9)]
                else:
                    pieces = [rng.choice(WORDS + NOISE) for _ in range(rng.randint(0, 14))]
                if rng.random() < 0.05:
                    pieces.append(bytes(rng.randrange(256) for _ in range(3)))
                text = b''.join(pieces)
                try:
                    text.decode('utf-8')
                except UnicodeDecodeError:
                    self.assertEqual(cf.parse(text).error, 'not UTF-8 text')   # the launcher refuses it first
                    continue
                texts.append(text)
            self.same(texts, seed)

    def test_rendered_files_parse_in_the_runtime(self):
        original = b'# mine\nl1 = r tap\nr2 = zr aim l2\nview = minus  # kept\n'
        inputs, custom = cf.layout(cf.parse(original), False)
        owned = {d for d in cf.DRAWN if d not in custom}
        new = cf.render(original, cf.swap(inputs, 'x', 'south'), False, owned)
        [result] = runtime([new], self.binary)
        self.assertEqual(result['line'], 0)
        self.assertEqual(mine(new), result)
        index = cf.PHYSICAL.index
        self.assertEqual((result['modes'][index('l1')] & 7, result['map'][index('l1')]),
                         (cf.TAP, 1 << cf.SWITCH.index('r')))
        self.assertEqual(result['modes'][index('r2')], cf.AIM | index('l2') << 3)


class Limits(unittest.TestCase):
    def test_size_and_utf8(self):
        self.assertEqual((cf.parse(b'#' * 16382).line, cf.parse(b'#' * 16383).error), (0, 'larger than 16 KB'))
        self.assertEqual(cf.parse(b'south = a \xff').error, 'not UTF-8 text')

    def test_controller_type_words(self):
        for data, word in ((None, 'auto'), (b'switch\n', 'switch'), (b' \tswitch\v\f\r\n', 'switch'),
                           (b'Switch', 'auto'), (b'xbox', 'xbox'), (b'playstation\n', 'playstation'), (b'', 'auto')):
            self.assertEqual(cf.read_type(data), word, data)


if __name__ == '__main__':
    unittest.main()

"""tools/builder/progress.py (S1.4): parse_line on the lines build_app.py prints, edge cases, a seeded fuzz, and the
drift test against build_app.py's source (read as text, never imported)."""
from pathlib import Path
import random
import re
import unittest

from tools.builder.progress import (KNOWN_STAGES, STAGE_TOTAL, Done, Failure, GameIdentified, Heartbeat, Line, Stage,
                                    UpdateIdentified, parse_line)

REPO = Path(__file__).resolve().parents[1]
T = '[10:01:02] '
HUGE = '9' * 5000

# Lines as build_app.py prints them; the stage texts are copied from its say() calls.
SHAPES = [
    (T + '1/4 code: rewriting main for Apple silicon (the big one: about a minute)',
     Stage(1, 4, 'code', 'rewriting main for Apple silicon (the big one: about a minute)', '', '10:01:02')),
    (T + '2/4 data: extracting and verifying the RomFS (17 GB, a minute or two)',
     Stage(2, 4, 'data', 'extracting and verifying the RomFS (17 GB, a minute or two)', '', '10:01:02')),
    (T + '2/4 update: merging base + 1.4.3 and verifying all IVFC levels',
     Stage(2, 4, 'update', 'merging base + 1.4.3 and verifying all IVFC levels', '', '10:01:02')),
    (T + '3/4 shaders: discovering and translating startup shaders from your game data',
     Stage(3, 4, 'shaders', 'discovering and translating startup shaders from your game data', '', '10:01:02')),
    (T + "4/4 app: assembling /Users/p/Games/Tears of the Kingdom.app",
     Stage(4, 4, 'app', 'assembling /Users/p/Games/Tears of the Kingdom.app', '', '10:01:02')),
    (T + '   ... still running (3 min)', Heartbeat(3, '', '10:01:02')),
    (T + 'Breath of the Wild 1.6.0; work folder /Users/p/My Games/BotW_build',
     GameIdentified('botw', '1.6.0', '', '10:01:02')),
    (T + 'work folder /Users/p/My Games/TOTK_build (412 GB free on its volume)', GameIdentified('totk', None, '', '10:01:02')),
    (T + 'update identified by verified ExeFS hashes: 1.2.1', UpdateIdentified('1.2.1', '', '10:01:02')),
    (T + 'update identified by verified ExeFS hashes: Breath of the Wild 1.9.0', UpdateIdentified('1.9.0', '', '10:01:02')),
    (T + 'done in 14 min: /Users/p/Games/Tears of the Kingdom.app', Done(14, '/Users/p/Games/Tears of the Kingdom.app', '', '10:01:02')),
    ('ERROR: needs about 25 GB free for the work folder (the app itself is an APFS clone of it when on the same volume)',
     Failure('needs about 25 GB free for the work folder (the app itself is an APFS clone of it when on the same volume)', '', None)),
]


def expected(line, event):
    """The fixture's event with its raw field filled in (raw is always the line without its ending)."""
    return type(event)(**{**event.__dict__, 'raw': line})


class Shapes(unittest.TestCase):
    def test_every_recognised_shape(self):
        for line, event in SHAPES:
            with self.subTest(line=line[:40]):
                self.assertEqual(parse_line(line), expected(line, event))

    def test_one_stage_per_known_name(self):
        names = {parse_line(line).name for line, _ in SHAPES[:5]}
        self.assertEqual(names, set(KNOWN_STAGES))


class EdgeCases(unittest.TestCase):
    def test_empty_and_bare_timestamp(self):
        self.assertEqual(parse_line(''), Line('', '', None))
        self.assertEqual(parse_line(T), Line('', T, '10:01:02'))
        self.assertEqual(parse_line('[10:01:02]'), Line('[10:01:02]', '[10:01:02]', None))

    def test_stage_range(self):
        for line in (T + '0/4 code: x', T + '5/4 code: x'):
            with self.subTest(line=line):
                self.assertIsInstance(parse_line(line), Line)
        self.assertEqual(parse_line(T + '9/9 extra: x').name, 'extra')       # unknown names are allowed

    def test_line_endings_only(self):
        self.assertEqual(parse_line(T + '1/4 code: x  \r\n').text, 'x  ')
        self.assertEqual(parse_line(T + '1/4 code: x \n').raw, T + '1/4 code: x ')
        self.assertEqual(parse_line('  inner  spaces  ').text, '  inner  spaces  ')

    def test_errors_need_the_exact_prefix(self):
        for line in ('ERROR:no space', 'error: lower case', T + 'ERROR: after a timestamp'):
            with self.subTest(line=line):
                self.assertIsInstance(parse_line(line), Line)

    def test_continuations_are_lines(self):
        for line in ('full output: /w/logs/rewrite.log', '  File "x.py", line 3', '', 'usage: build_app.py [-h]'):
            with self.subTest(line=line):
                self.assertEqual(parse_line(line), Line(line, line, None))

    def test_say_message_with_an_embedded_line_break(self):
        first, second = (T + '1/4 code: first part\nsecond part').split('\n')
        self.assertIsInstance(parse_line(first), Stage)
        self.assertEqual(parse_line(second), Line('second part', 'second part', None))

    def test_non_ascii_kept(self):
        self.assertEqual(parse_line(T + '4/4 app: assembling /Users/p/Juegos – Zelda.app').text,
                         'assembling /Users/p/Juegos – Zelda.app')

    def test_non_str_raises_type_error_from_the_entry_check(self):
        for value in (None, b'[10:00:00] 1/4 code: x', 4, 1.5):
            with self.subTest(value=value):
                with self.assertRaisesRegex(TypeError, 'expects a str'):
                    parse_line(value)


class HugeNumbers(unittest.TestCase):
    def test_huge_numbers_are_lines(self):
        for line in (T + f'{HUGE}/{HUGE} code: x', T + f'   ... still running ({HUGE} min)', T + f'done in {HUGE} min: /a.app'):
            with self.subTest(line=line[:30]):
                self.assertIsInstance(parse_line(line), Line)

    def test_digit_boundaries(self):
        self.assertIsInstance(parse_line(T + '999/999 code: x'), Stage)
        self.assertIsInstance(parse_line(T + '1000/1000 code: x'), Line)
        self.assertEqual(parse_line(T + '   ... still running (999999 min)').minutes, 999999)
        self.assertIsInstance(parse_line(T + '   ... still running (1000000 min)'), Line)
        self.assertEqual(parse_line(T + 'done in 999999 min: /a.app').minutes, 999999)
        self.assertIsInstance(parse_line(T + 'done in 1000000 min: /a.app'), Line)


class NeverRaises(unittest.TestCase):
    def test_seeded_random_strings(self):
        rng = random.Random(1234)
        alphabet = 'abcdefghijklmnopqrstuvwxyz ERO[]0123456789/:().-–\r\n\t'
        templates = ['', T, T + '1/4 ', T + '   ... still running (', 'ERROR: ', T + 'done in ']
        for _ in range(2000):
            line = rng.choice(templates) + ''.join(rng.choice(alphabet) for _ in range(rng.randrange(60)))
            self.assertIn(type(parse_line(line)), (Stage, Heartbeat, GameIdentified, UpdateIdentified, Done, Failure, Line))


class DriftAgainstBuildApp(unittest.TestCase):
    def setUp(self):
        self.source = (REPO / 'build_app.py').read_text()

    def test_stage_prefixes_match_the_known_stages(self):
        found = {(int(i), int(n), name) for i, n, name in re.findall(r"""['"]([0-9])/([0-9]) ([a-z]+):""", self.source)}
        self.assertEqual({n for _, n, _ in found}, {STAGE_TOTAL})
        self.assertEqual({name for _, _, name in found}, set(KNOWN_STAGES))

    def test_literal_pieces_the_patterns_rely_on(self):
        for piece in ('... still running (', 'Breath of the Wild {expected["display_version"]}; work folder', 'GB free on its volume)',
                      'update identified by verified ExeFS hashes:', "say(f'done in {", "print(f'\\nERROR: {e}'",
                      'print(f\'[{time.strftime("%H:%M:%S")}] {msg}\''):
            with self.subTest(piece=piece):
                self.assertIn(piece, self.source, f'build_app.py changed: {piece!r} is gone; update progress.py')


if __name__ == '__main__':
    unittest.main()

"""A merged or trimmed card image ("1G+1U", #2242) keeps no system update partition, so no system fonts: MNG Launcher says
so before the build, from the card's HFS0 tables, which need no keys (after PR #2247, which made the build's font step
say it). Synthetic card images only: the layout extract_exefs.partition_entries reads."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import classify  # noqa: E402


def hfs0(files):
    """An HFS0 table and its files: [(name, bytes)] -> bytes."""
    strings, offsets = b'', []
    for name, _ in files:
        offsets.append(len(strings))
        strings += name.encode() + b'\0'
    table, data = b'', b''
    for (name, body), nameoff in zip(files, offsets):
        table += struct.pack('<QQI', len(data), len(body), nameoff) + b'\0' * (64 - 20)
        data += body
    return b'HFS0' + struct.pack('<II', len(files), len(strings)) + b'\0' * 4 + table + strings + data


def card(partitions):
    """A game card image whose root HFS0 at 0x200 holds these partitions ([(name, [(file, bytes)])])."""
    head = bytearray(0x200)
    head[0x100:0x104] = b'HEAD'
    head[0x130:0x138] = struct.pack('<Q', 0x200)
    return bytes(head) + hfs0([(name, hfs0(files)) for name, files in partitions])


class CardUpdatePartition(unittest.TestCase):
    def write(self, data, name='game.xci'):
        path = Path(self.tmp.name) / name
        path.write_bytes(data)
        return str(path)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_count_of_update_ncas(self):
        game = [('aa.nca', b'x' * 32)]
        self.assertEqual(classify.card_update_ncas(self.write(card([('secure', game)]))), 0)              # merged: none
        self.assertEqual(classify.card_update_ncas(self.write(card([('update', []), ('secure', game)]))), 0)   # empty
        full = card([('update', [('a.cnmt.nca', b'm'), ('b.nca', b'f' * 8), ('c.nca', b'g' * 8)]), ('secure', game)])
        self.assertEqual(classify.card_update_ncas(self.write(full)), 2)
        self.assertIsNone(classify.card_update_ncas(self.write(b'not a card' * 100)))
        self.assertIsNone(classify.card_update_ncas(self.write(b'\0' * 0x100 + b'HEAD' + b'\0' * 64)))   # no root table

    def blocks(self, data, game='totk', extra=()):
        items = [classify.Item(self.write(data), 'base', game), *extra]
        return classify.plan(items).blocks

    def test_no_fonts_builds_with_the_open_ones_silently(self):
        # User 2026-11-20: "如果遇到 没有字体的情况下 自动使用 开源字体 … 不要报错 不要提示 直接顺利安装": a merged card
        # ("1G+1U") or an NSP without fonts plans the open-source fonts at once, with no block and no note
        game = [('aa.nca', b'x' * 32)]
        merged = card([('secure', game)])
        for game_id, data, name in (('totk', merged, 'game.xci'), ('odyssey', b'nsp', 'game.nsp'), ('acnh', b'nsp', 'game.nsp'),
                                    ('eow', b'nsp', 'game.nsp'), ('botw', merged, 'game.xci')):
            p = classify.plan([classify.Item(self.write(data, name), 'base', game_id)])
            self.assertTrue(p.open_fonts, game_id)
            self.assertIsNone(p.font_block, game_id)
            self.assertFalse([b for b in p.blocks if 'font' in b.lower() or '字体' in b], game_id)
            self.assertFalse([n for n in p.notes if 'Noto' in n or '字体' in n], game_id)

    def test_fonts_that_are_there_win(self):
        merged = card([('secure', [('aa.nca', b'x' * 32)])])
        fonts = classify.Item(str(Path(self.tmp.name) / 'shared-fonts'), 'fonts')
        given = classify.plan([classify.Item(self.write(merged), 'base', 'totk'), fonts])
        self.assertFalse(given.open_fonts)   # the player's fonts
        full = card([('update', [('b.nca', b'f' * 8)]), ('secure', [('aa.nca', b'x' * 32)])])
        self.assertFalse(classify.plan([classify.Item(self.write(full), 'base', 'totk')]).open_fonts)   # the card's own
        for game_id in ('dread', 'tmnt'):   # games that take no system fonts
            self.assertFalse(classify.plan([classify.Item(self.write(merged), 'base', game_id)]).open_fonts, game_id)


class OpenFontsInTheBuild(unittest.TestCase):
    def test_the_build_takes_the_open_fonts_folder(self):
        from tools.builder_jobs import build_job
        said = []
        emit = lambda kind, **f: said.append((kind, f))
        argv = ['/game.nsp', '-k', '/prod.keys', '--work', '/w', '-o', '/o.app']
        made = build_job.with_open_fonts(argv, emit, make=lambda: '/cache/open-fonts-x')
        self.assertEqual(made, ['/game.nsp', '-k', '/prod.keys', '--shared-fonts', '/cache/open-fonts-x', '--work', '/w', '-o', '/o.app'])
        self.assertEqual(said[0][0], 'log')
        given = argv[:3] + ['--firmware', '/fw'] + argv[3:]
        self.assertEqual(build_job.with_open_fonts(given, emit, make=lambda: self.fail('made')), given)

        def broken():
            raise OSError('no network')
        with self.assertRaises(build_job.Refused):
            build_job.with_open_fonts(argv, emit, make=broken)


if __name__ == '__main__':
    unittest.main()

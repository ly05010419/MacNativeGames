import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]   # games/ssbu
SCRIPTS = GAME/'scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('extract_dlc', SCRIPTS/'extract_dlc.py')
dlc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dlc)


class SsbuDlc(unittest.TestCase):
    # Q96: a DLC's index is its title id minus Smash's add-on content base (the application id + 0x1000)
    def test_fighter_titles_are_indices_1_to_12(self):
        self.assertEqual(dlc.dlc_index(0x01006A800016F001), 1)    # Piranha Plant
        self.assertEqual(dlc.dlc_index(0x01006A800016F00C), 12)   # Challenger Pack 11
        self.assertEqual(dlc.dlc_index(0x01006A800016F088), 136)

    def test_other_titles_are_refused(self):
        for title in (0x01006A800016F000, 0x01006A800016E000, 0x01006A800016E800, 0x01006A8000170000, 0x0100000000010000):
            with self.assertRaises(Exception, msg=f'{title:016X}'):
                dlc.dlc_index(title)

    def test_runtime_limit_matches(self):
        text = (GAME/'runtime/services.c').read_text()
        self.assertIn(f'#define AOC_MAX {dlc.AOC_MAX}', text)
        self.assertIn('build/dlc/owned.txt', text)


HARNESS = r'''
#include "aoc_scan.h"
int main(int argc, char **argv) {
    int out[2048], nsps = 0, skipped = 0;
    int n = aoc_scan_dir(argv[1], 0x01006A800016E000ULL, out, 2048, &nsps, &skipped);
    printf("%d %d %d %d", n, nsps, skipped, n > 0 ? aoc_fighters(out, n) : 0);
    for (int i = 0; i < n; i++) printf(" %d", out[i]);
    printf("\n");
    return 0;
}
'''


def pfs0(names, data=None):
    """A PFS0 container with these file names; data gives some files' contents (others are 16 zero bytes)."""
    data = data or {}
    table = b''.join(n.encode() + b'\0' for n in names)
    table += bytes(-len(table) % 16)
    entries, body, at = b'', b'', 0
    for n in names:
        content = data.get(n, bytes(16))
        entries += struct.pack('<QQII', len(body), len(content), at, 0)
        body += content
        at += len(n) + 1
    return b'PFS0' + struct.pack('<III', len(names), len(table), 0) + entries + table + body


def cnmt_xml(title, app=0x01006A800016E000, kind='AddOnContent'):
    return (f'<?xml version="1.0" encoding="utf-8"?>\n<ContentMeta>\n  <Type>{kind}</Type>\n  <Id>0x{title:016x}</Id>\n'
            f'  <Version>0</Version>\n  <Content>\n    <Type>Meta</Type>\n    <Id>a694d64205f9674cf96bd7edb13f3972</Id>\n  </Content>\n'
            f'  <ApplicationId>0x{app:016x}</ApplicationId>\n</ContentMeta>\n').encode()


class SsbuDlcFolder(unittest.TestCase):
    # Q96: runtime/aoc_scan.h reads a folder of DLC NSPs without keys (the app's DLC menu, iOS Documents/DLC)
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src, cls.exe = Path(cls.tmp.name)/'harness.c', Path(cls.tmp.name)/'harness'
        src.write_text(HARNESS)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-I', str(GAME/'runtime'), src, '-o', cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scan(self, files):
        with tempfile.TemporaryDirectory() as d:
            for name, data in files.items():
                (Path(d)/name).write_bytes(data)
            return subprocess.run([self.exe, d], capture_output=True, text=True, check=True).stdout.split()

    def test_owned_dlc_are_counted_once_and_others_skipped(self):
        tik = lambda title: f'{title:016x}0000000000000005.tik'
        out = self.scan({
            'fighter.nsp': pfs0(['a.nca', 'b.cnmt.nca', tik(0x01006A800016F002), 'c.cert']),
            'same again.NSP': pfs0(['b.cnmt.nca', tik(0x01006A800016F002)]),        # the same DLC, upper-case suffix
            'costume.nsp': pfs0(['b.cnmt.nca', tik(0x01006A800016F045)]),
            'game.nsp': pfs0(['b.cnmt.nca', tik(0x01006A800016E000)]),              # the base game
            'update.nsp': pfs0(['b.cnmt.nca', tik(0x01006A800016E800)]),            # its update
            'no meta.nsp': pfs0(['a.nca', tik(0x01006A800016F003)]),
            'two titles.nsp': pfs0(['b.cnmt.nca', tik(0x01006A800016F004), tik(0x01006A800016F005)]),
            'broken.nsp': b'NOPE' + bytes(60),
            'notes.txt': b'not an nsp',
        })
        # 2 DLC of 8 NSPs (the copy counts once and is no skip), 5 skipped, 1 fighter: indices 2 and 69
        self.assertEqual(out, ['2', '8', '5', '1', '2', '69'])

    def test_nsps_without_a_ticket_are_read_from_their_cnmt_xml(self):
        # 13 of the player's 99 NSPs carry no ticket (no rights management), only a plain .cnmt.xml (25.10.)
        names = ['d.nca', 'm.cnmt.nca', 'm.cnmt.xml']
        out = self.scan({
            'pack 8.nsp': pfs0(names, {'m.cnmt.xml': cnmt_xml(0x01006A800016F009)}),
            'other game.nsp': pfs0(names, {'m.cnmt.xml': cnmt_xml(0x0100000000011000, app=0x0100000000010000)}),
            'an update.nsp': pfs0(names, {'m.cnmt.xml': cnmt_xml(0x01006A800016E800, kind='Patch')}),
            'empty xml.nsp': pfs0(names, {'m.cnmt.xml': b''}),
        })
        self.assertEqual(out, ['1', '4', '3', '1', '9'])

    def test_missing_folder_is_minus_one(self):
        out = subprocess.run([self.exe, '/nonexistent/dlc'], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out[0], '-1')


if __name__ == '__main__':
    unittest.main()

"""tools/builder/checks_inputs.py (S1.2b): the helpers, repository, input_files and input_rules. Disk checks, the
full run with default paths and the drift test are in test_builder_checks_disk.py. Tiny files stand in for game
files: only their first bytes matter, and no game data is used."""
import errno
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import mock_open, patch

from tools.builder import checks_inputs
from tools.builder.checks import BuildPaths, CheckContext, Status, run_checks
from tools.builder.checks_inputs import (InputFiles, InputRules, Repository, cached_fonts, effective_paths,
                                         font_file_names, input_choice, read_kind)
from tools.builder.games import BOTW, TOTK
from tools.builder.inputs import BaseKind, Fonts, UpdateKind

REPO = Path(__file__).resolve().parents[1]
NAMES = font_file_names(REPO)


def write_nsp(path):
    path.write_bytes(b'PFS0' + bytes(16))
    return path


def write_xci(path):
    data = bytearray(0x200)
    data[0x100:0x104] = b'HEAD'
    path.write_bytes(bytes(data))
    return path


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name).resolve()
        self.keys = self.tmp / 'prod.keys'
        self.keys.write_text('k')

    def fonts(self, folder, names=NAMES, manifest=False):
        folder.mkdir(parents=True, exist_ok=True)
        for n in names:
            (folder / n).write_bytes(b'f')
        if manifest:
            (folder / 'manifest.json').write_text('{}')
        return folder

    def ctx(self, paths, spec=TOTK, repo_root=REPO):
        return CheckContext(runner=None, repo_root=repo_root, spec=spec, paths=paths)


class EffectivePaths(Base):
    def test_defaults_for_each_game(self):
        game = write_xci(self.tmp / 'g.xci')
        for spec, work, app in ((TOTK, 'TOTK_build', 'Tears of the Kingdom.app'), (BOTW, 'BotW_build', 'Breath of the Wild.app')):
            with self.subTest(spec=spec.id):
                p = effective_paths(spec, BuildPaths(game, self.keys))
                self.assertEqual((p.work, p.out), (self.tmp / work, self.tmp / app))
                self.assertEqual(effective_paths(spec, p), p)       # idempotent

    def test_given_paths_are_kept_and_home_is_expanded(self):
        with patch.dict(os.environ, {'HOME': str(self.tmp)}):
            p = effective_paths(TOTK, BuildPaths(Path('~/g.nsp'), Path('~/prod.keys'), work=Path('~/w'), out=Path('~/o.app')))
        self.assertEqual((p.game, p.work, p.out), (self.tmp / 'g.nsp', self.tmp / 'w', self.tmp / 'o.app'))


class FontFileNames(Base):
    def test_real_repository(self):
        self.assertEqual(NAMES[0], '0-standard.bfttf')
        self.assertEqual(NAMES[5], '5-nintendo-extension.bfttf')
        self.assertEqual(len(NAMES), 6)

    def test_none_cases(self):
        target = self.tmp / checks_inputs.FONTS_MANIFEST
        target.parent.mkdir(parents=True)
        five = {str(i): 'x' for i in range(5)}
        for content in (b'\xff\xfe', b'{bad', json.dumps({}).encode(), json.dumps({'shared_fonts_sha256': five}).encode(),
                        json.dumps({'shared_fonts_sha256': []}).encode(), json.dumps([1]).encode()):
            with self.subTest(content=content[:20]):
                target.write_bytes(content)
                self.assertIsNone(font_file_names(self.tmp))
        target.unlink()
        self.assertIsNone(font_file_names(self.tmp))                # missing file
        with patch.object(Path, 'read_bytes', side_effect=PermissionError('denied')):
            self.assertIsNone(font_file_names(REPO))


class ReadKind(Base):
    def test_kinds(self):
        cases = {'a.nsp': ('nsp', write_nsp), 'a.NSZ': ('nsz', write_nsp), 'b.nsz': ('nsz', write_nsp),
                 'a.xci': ('xci', write_xci), 'a.xcz': ('xcz', write_xci)}
        for name, (kind, write) in cases.items():
            with self.subTest(name=name):
                self.assertEqual(read_kind(write(self.tmp / name)), kind)

    def test_short_and_unknown_files(self):
        (self.tmp / 'four.nsp').write_bytes(b'PFS0')
        (self.tmp / 'three.xci').write_bytes(b'HEA')
        (self.tmp / 'zeros.xci').write_bytes(bytes(0x200))
        self.assertEqual([read_kind(self.tmp / n) for n in ('four.nsp', 'three.xci', 'zeros.xci')], ['nsp', 'unknown', 'unknown'])

    def test_os_error_propagates(self):
        with patch.object(checks_inputs, 'open', side_effect=OSError(errno.EIO, 'I/O error'), create=True):
            self.assertRaises(OSError, read_kind, self.tmp / 'x.nsp')


class CachedFontsAndChoice(Base):
    def test_cached_fonts(self):
        cache = self.tmp / 'w' / checks_inputs.FONT_CACHE
        self.assertFalse(cached_fonts(self.tmp / 'w', NAMES))
        self.fonts(cache, manifest=True)
        self.assertTrue(cached_fonts(self.tmp / 'w', NAMES))
        self.assertFalse(cached_fonts(self.tmp / 'w', None))
        (cache / 'manifest.json').unlink()
        self.assertFalse(cached_fonts(self.tmp / 'w', NAMES))
        (cache / 'manifest.json').write_text('{}')
        (cache / NAMES[3]).unlink()
        self.assertFalse(cached_fonts(self.tmp / 'w', NAMES))

    def test_input_choice(self):
        game, update = write_nsp(self.tmp / 'g.nsp'), write_xci(self.tmp / 'u.xcz')
        p = effective_paths(TOTK, BuildPaths(game, self.keys, update=update, firmware=self.tmp, shared_fonts=self.tmp))
        c = input_choice(TOTK, p, NAMES)
        self.assertEqual((c.base_kind, c.update_kind, c.fonts, c.fonts_cached), (BaseKind.NSP, UpdateKind.XCI, Fonts.BOTH, False))
        p = effective_paths(TOTK, BuildPaths(write_xci(self.tmp / 'g.xci'), self.keys, firmware=self.tmp))
        c = input_choice(TOTK, p, NAMES)
        self.assertEqual((c.base_kind, c.update_kind, c.fonts), (BaseKind.XCI, UpdateKind.NONE, Fonts.FIRMWARE))
        c = input_choice(TOTK, effective_paths(TOTK, BuildPaths(game, self.keys, shared_fonts=self.tmp)), NAMES)
        self.assertEqual(c.fonts, Fonts.SHARED)


class RepositoryCheck(Base):
    def test_real_repository_for_both_games(self):
        game = write_xci(self.tmp / 'g.xci')
        self.assertEqual(Repository().run(self.ctx(BuildPaths(game, self.keys))).detail, '1.0.0, 1.1.2, 1.2.1, 1.4.3')
        self.assertEqual(Repository().run(self.ctx(BuildPaths(game, self.keys), BOTW)).detail, '1.6.0, 1.9.0')

    def fake_repo(self, build_app=True):
        root = self.tmp / f'repo-{build_app}'
        (root / TOTK.assets_dir).mkdir(parents=True)
        if build_app:
            (root / 'build_app.py').write_text('')
        for v in TOTK.versions:
            (root / TOTK.assets_dir / v.manifest).write_text(json.dumps({'display_version': v.display, 'title_id': TOTK.title_id}))
        return root

    def run_on(self, root):
        return Repository().run(self.ctx(BuildPaths(write_xci(self.tmp / 'g.xci'), self.keys), repo_root=root))

    def test_outcomes(self):
        self.assertEqual(self.run_on(self.fake_repo()).status, Status.OK)
        r = self.run_on(self.fake_repo(build_app=False))
        self.assertEqual((r.status, r.message_id, r.detail), (Status.BLOCK, 'repository_incomplete', 'build_app.py missing'))

    def test_broken_manifest_blocks(self):
        root = self.fake_repo()
        (root / TOTK.assets_dir / 'expected-v1.2.1.json').write_text('[')
        r = self.run_on(root)
        self.assertEqual((r.status, r.detail), (Status.BLOCK, 'expected-v1.2.1.json:invalid_json'))

    def test_unknown_manifest_warns(self):
        root = self.fake_repo()
        (root / TOTK.assets_dir / 'expected-v9.9.9.json').write_text('{}')
        r = self.run_on(root)
        self.assertEqual((r.status, r.message_id, r.detail), (Status.WARN, 'repository_newer_than_builder', 'expected-v9.9.9.json'))


class InputFilesCheck(Base):
    def run_with(self, **kw):
        kw.setdefault('game', write_xci(self.tmp / 'g.xci'))
        kw.setdefault('keys', self.keys)
        return InputFiles().run(self.ctx(BuildPaths(**kw)))

    def test_ok(self):
        self.assertEqual(self.run_with().status, Status.OK)

    def test_first_failure_wins(self):
        r = self.run_with(game=self.tmp / 'missing.xci', keys=self.tmp / 'missing.keys')
        self.assertEqual((r.message_id, r.detail), ('input_missing', f'game: {self.tmp / "missing.xci"}'))

    def test_each_field(self):
        self.assertEqual(self.run_with(keys=self.tmp).detail, f'keys: {self.tmp}')                     # a folder
        self.assertEqual(self.run_with(update=self.tmp / 'u.nsp').detail, f'update: {self.tmp / "u.nsp"}')
        self.assertEqual(self.run_with(firmware=self.keys).detail, f'firmware: {self.keys}')
        self.assertEqual(self.run_with(shared_fonts=self.tmp / 'none').detail, f'shared_fonts: {self.tmp / "none"}')
        r = self.run_with(out=self.tmp / 'TOTK.ipa')
        self.assertEqual((r.message_id, r.fix), ('output_not_app', None))

    def test_unreadable_file(self):
        real_open = open
        def deny_keys(path, mode='r', *a, **k):
            if Path(path) == self.keys:
                raise PermissionError(errno.EACCES, 'Permission denied')
            return real_open(path, mode, *a, **k)
        with patch.object(checks_inputs, 'open', side_effect=deny_keys, create=True):
            r = self.run_with()
        self.assertEqual((r.status, r.message_id), (Status.BLOCK, 'input_unreadable'))
        self.assertTrue(r.detail.startswith('keys: '))

    def test_system_error_on_open_propagates_to_check_crashed(self):
        with patch.object(checks_inputs, 'open', side_effect=OSError(errno.EIO, 'I/O error'), create=True):
            with self.assertRaises(OSError):
                self.run_with()
            r = run_checks([InputFiles()], self.ctx(BuildPaths(write_xci(self.tmp / 'g.xci'), self.keys)))[0]
        self.assertEqual((r.status, r.message_id), (Status.BLOCK, 'check_crashed'))

    def test_fonts(self):
        fonts = self.fonts(self.tmp / 'fonts', NAMES[:2] + NAMES[3:])
        self.assertEqual(self.run_with(shared_fonts=fonts).detail, f'shared_fonts: {NAMES[2]}')
        (fonts / NAMES[2]).write_bytes(b'f')
        opened = []
        real_open = open
        def record(path, mode='r', *a, **k):
            opened.append((Path(path).name, mode))
            if Path(path).name == NAMES[1]:
                raise PermissionError(errno.EACCES, 'Permission denied')
            return real_open(path, mode, *a, **k)
        with patch.object(checks_inputs, 'open', side_effect=record, create=True):
            r = self.run_with(shared_fonts=fonts)
        self.assertEqual(r.message_id, 'input_unreadable')
        self.assertTrue(r.detail.startswith(f'shared_fonts: {NAMES[1]}: '))
        self.assertEqual([n for n, _ in opened if n.endswith('.bfttf')], list(NAMES[:2]))    # stopped at the failure

    def test_fonts_opened_rb_and_never_read(self):
        fonts = self.fonts(self.tmp / 'fonts')
        m = mock_open()
        with patch.object(checks_inputs, 'open', m, create=True):
            self.assertEqual(self.run_with(shared_fonts=fonts).status, Status.OK)
        font_calls = [c for c in m.call_args_list if str(c.args[0]).endswith('.bfttf')]
        self.assertEqual(len(font_calls), 6)
        self.assertTrue(all(c.args[1] == 'rb' for c in font_calls))
        m.return_value.read.assert_not_called()

    def test_font_names_unknown(self):
        with patch.object(checks_inputs, 'font_file_names', return_value=None):
            r = self.run_with(shared_fonts=self.tmp)
        self.assertEqual(r.message_id, 'font_names_unknown')

    def test_missing_spec_or_paths_is_a_programming_error(self):
        r = run_checks([InputFiles()], CheckContext(runner=None, spec=TOTK))[0]
        self.assertEqual((r.status, r.message_id), (Status.BLOCK, 'check_crashed'))
        self.assertIn('ValueError', r.detail)


class InputRulesCheck(Base):
    def verdict(self, spec, game, **kw):
        return InputRules().run(self.ctx(BuildPaths(game, self.keys, **kw), spec))

    def test_scenarios_reach_the_result_unchanged(self):
        xci, nsp = write_xci(self.tmp / 'g.xci'), write_nsp(self.tmp / 'g.nsp')
        r = self.verdict(TOTK, xci)                                                         # T1
        self.assertEqual((r.status, r.detail), (Status.OK, ''))
        r = self.verdict(TOTK, nsp)                                                         # T6
        self.assertEqual((r.status, r.message_id, r.detail), (Status.BLOCK, 'input_rules', 'fonts_needed'))
        r = self.verdict(BOTW, xci)                                                         # B4
        self.assertEqual(r.detail, 'fonts_needed')
        r = self.verdict(BOTW, nsp, shared_fonts=self.tmp)                                  # B5
        self.assertEqual(r.detail, 'botw_needs_update')
        r = self.verdict(BOTW, nsp)
        self.assertEqual(r.detail, 'fonts_needed, botw_needs_update')

    def test_cached_fonts_note(self):
        self.fonts(self.tmp / 'w' / checks_inputs.FONT_CACHE, manifest=True)
        r = self.verdict(TOTK, write_nsp(self.tmp / 'g.nsp'), work=self.tmp / 'w')          # T5 without update
        self.assertEqual((r.status, r.detail), (Status.OK, 'fonts_cached'))


if __name__ == '__main__':
    unittest.main()

"""Breath of the Wild from a base NSP plus the 1.9.0 update NSP: package roles, the botw190 route of build_app.main, its pins.

Synthetic PFS0 files with zero-key tickets only; no game data and no keys.
"""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT)]
import build_app

BOTW_BASE, BOTW_UPDATE, BOTW_DLC = '01007EF00011E000', '01007EF00011E800', '01007EF00011F001'
TOTK_BASE, TOTK_UPDATE = '0100F2C0115B6000', '0100F2C0115B6800'


def pfs0(path, entries):
    strings, table, payload = b'', bytearray(), b''
    for name, data in entries:
        table += struct.pack('<QQII', len(payload), len(data), len(strings), 0)
        strings += name.encode() + b'\0'
        payload += data
    path.write_bytes(b'PFS0' + struct.pack('<III', len(entries), len(strings), 0) + bytes(table) + strings + payload)
    return path


def ticket(title_id):
    """A 0x2C0-byte RSA-2048 ticket: signature type 04 00 01 00, the rights ID at 0x140 + 0x160, a zero key."""
    data = bytearray(0x2C0)
    data[0:4] = bytes([4, 0, 1, 0])
    data[0x2A0:0x2A8] = bytes.fromhex(title_id)
    data[0x2A8:0x2B0] = (1).to_bytes(8, 'big')
    return bytes(data)


def package(folder, name, title_id):
    return pfs0(folder / name, [(f'{title_id.lower()}.tik', ticket(title_id)), ('other.nca', b'x' * 64)])


class Stop(Exception):
    pass


def card_image(folder):
    xci = folder / 'botw.xci'
    xci.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x200))
    return xci


def run_main(case, folder, base, update=None, dlcs=(), game='botw', stop='step_code_botw', fonts=True, version='botw190'):
    """build_app.main with the long steps mocked: it stops at `stop` (Stop) or at the first BuildError, which it returns
    with the started mocks. fonts=False leaves out --firmware; version is what the update is identified as."""
    keys = folder / 'prod.keys'
    keys.write_text('')
    firmware = folder / 'firmware'
    firmware.mkdir(exist_ok=True)
    argv = [str(base), '-k', str(keys), *(['--firmware', str(firmware)] if fonts else [])]
    if update:
        argv += ['--update', str(update)]
    for dlc in dlcs:
        argv += ['--dlc', str(dlc)]

    def prepare_work(work, py, logs):
        work.mkdir(parents=True, exist_ok=True)
        return work / 'totk'
    mocks = dict(
        prerequisites=mock.patch.object(build_app, 'prerequisites', return_value=Path('/usr/bin/python3')),
        prepare_work=mock.patch.object(build_app, 'prepare_work', side_effect=prepare_work),
        identify_game=mock.patch.object(build_app, 'identify_game', return_value=game),
        container=mock.patch.object(build_app, 'prepare_update_container', side_effect=lambda tree, source, keys, logs: source),
        identify_update=mock.patch.object(build_app, 'identify_botw_update', return_value=version),   # by the update's ExeFS hashes
        step_code_botw=mock.patch.object(build_app, 'step_code_botw', side_effect=Stop if stop == 'step_code_botw' else None),
        step_data_botw=mock.patch.object(build_app, 'step_data_botw', side_effect=Stop if stop == 'step_data_botw' else None),
        step_code=mock.patch.object(build_app, 'step_code', side_effect=Stop),
        disk=mock.patch.object(build_app.shutil, 'disk_usage', return_value=mock.Mock(free=1000 * 2**30)))
    started = {name: patcher.start() for name, patcher in mocks.items()}
    for patcher in mocks.values():
        case.addCleanup(patcher.stop)
    try:
        build_app.main(argv)
        error = None
    except (build_app.BuildError, Stop) as e:
        error = e
    return error, started


class GameSelectionTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def test_ticket_title_ids_and_kinds(self):
        for title_id in (BOTW_BASE, BOTW_UPDATE, BOTW_DLC):
            self.assertEqual(build_app.ticket_title_ids(package(self.root, f'{title_id}.nsp', title_id)), [title_id])
        self.assertEqual(build_app.title_kind(BOTW_BASE), (BOTW_BASE, 'base'))
        self.assertEqual(build_app.title_kind(BOTW_UPDATE), (BOTW_BASE, 'update'))
        self.assertEqual(build_app.title_kind('01007EF00011F002'), (BOTW_BASE, 'dlc'))
        xci = self.root / 'game.xci'
        xci.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x200))
        self.assertEqual(build_app.ticket_title_ids(xci), [])
        self.assertEqual(build_app.ticket_title_ids(pfs0(self.root / 'free.nsp', [('a.nca', b'x' * 32)])), [])

    def test_a_file_on_the_wrong_flag_is_named(self):
        with self.assertRaisesRegex(build_app.BuildError, 'pass it as --update'):
            build_app.check_package(package(self.root, 'u.nsp', BOTW_UPDATE), 'botw', 'base')
        with self.assertRaisesRegex(build_app.BuildError, 'pass it as the game file'):
            build_app.check_package(package(self.root, 'b.nsp', BOTW_BASE), 'botw', 'update')
        with self.assertRaisesRegex(build_app.BuildError, 'pass it as --dlc'):
            build_app.check_package(package(self.root, 'd.nsp', BOTW_DLC), 'botw', 'base')
        build_app.check_package(package(self.root, 'b2.nsp', BOTW_BASE), 'botw', 'base')
        build_app.check_package(package(self.root, 'u2.nsp', BOTW_UPDATE), 'botw', 'update')

    def test_packages_of_another_game_stop(self):
        with self.assertRaises(build_app.BuildError) as ctx:
            build_app.check_package(package(self.root, 'totk-u.nsp', TOTK_UPDATE), 'botw', 'update')
        self.assertIn('Tears of the Kingdom', str(ctx.exception))
        self.assertIn('Breath of the Wild', str(ctx.exception))

    def test_rights_free_and_xci_pass_the_check(self):
        build_app.check_package(pfs0(self.root / 'free.nsp', [('a.nca', b'x')]), 'botw', 'base')
        xci = self.root / 'game.xci'
        xci.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x200))
        build_app.check_package(xci, 'botw', 'base')

    def test_1_9_0_pins_have_the_shape_of_1_6_0(self):
        pins, card = build_app.VERSIONS['botw190'], build_app.VERSIONS['botw160']
        self.assertEqual(pins['title_id'], card['title_id'])
        self.assertEqual((pins['version'], pins['display_version']), (1114112, '1.9.0'))
        for key in ('exefs_sha256', 'rewritten_sha256', 'shared_fonts_sha256'):
            self.assertEqual(list(pins[key]), list(card[key]))
        self.assertEqual(sorted(pins['romfs']), ['files', 'sha256', 'size'])
        self.assertEqual(pins['aoc'], card['aoc'])   # the same two RomFS images, from the --dlc NSPs
        self.assertNotEqual(pins['exefs_sha256']['main'], card['exefs_sha256']['main'])

    def test_botw190_data_merges_base_and_update_and_takes_fonts_from_firmware(self):
        tree, romfs = self.root / 'tree', self.root / 'tree/extracted/botw190/romfs'
        tree.mkdir()
        commands = []

        def run(cmd, log, cwd=None, **_):
            commands.append([Path(c).name if isinstance(c, Path) else c for c in cmd])
            if 'extract_romfs.py' in str(cmd[1]):
                romfs.mkdir(parents=True)
                (romfs / 'romfs.bin').write_bytes(b'x' * 8)
                (romfs / 'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256='ab' * 32, romfs_size=8)))
        pins = {**build_app.BOTW190, 'romfs': {'size': 8, 'sha256': 'ab' * 32}}
        game, update, firmware = self.root / 'base.nsp', self.root / 'update.nsp', self.root / 'firmware'
        with mock.patch.dict(build_app.VERSIONS, {'botw190': pins}), mock.patch.object(build_app, 'run', side_effect=run), \
             mock.patch.object(build_app, 'build_hactool', return_value=Path('hactool')), \
             mock.patch.object(build_app, 'validate_fonts', side_effect=[build_app.BuildError('no fonts yet'), None, build_app.BuildError('none')]) as validate:
            build_app.step_data_botw(tree, tree.parent / 'botw', game, Path('prod.keys'), self.root / 'logs',
                                     version='botw190', update=update, firmware=firmware)
            self.assertEqual(commands[0][1:5], ['extract_romfs.py', 'base.nsp', '--update', 'update.nsp'])   # #117's base + separate update merge
            self.assertEqual(commands[1][1], 'scripts/extract_shared_fonts.py')   # no --dlc given: no add-on content
            self.assertEqual(len(commands), 2)
            # without a font source the build stops and names both options (#170)
            with self.assertRaisesRegex(build_app.BuildError, 'needs --firmware .* or --shared-fonts'):
                build_app.step_data_botw(tree, tree.parent / 'botw', game, Path('prod.keys'), self.root / 'logs',
                                         version='botw190', update=update)

    # ---- main() ----
    def run_main(self, base, update=None, game='botw', fonts=True):
        return run_main(self, self.root, base, update, game=game, fonts=fonts)

    def card_image(self):
        return card_image(self.root)

    def test_botw_nsp_without_the_update_names_it(self):
        error, started = self.run_main(package(self.root, 'botw.nsp', BOTW_BASE))
        self.assertIn('needs the 1.9.0 update', str(error))
        self.assertIn('--update', str(error))
        started['step_code_botw'].assert_not_called()

    def test_botw_update_decides_the_version(self):
        update = package(self.root, 'botw-u.nsp', BOTW_UPDATE)
        error, started = self.run_main(self.card_image(), update)
        self.assertIsInstance(error, Stop)
        started['identify_update'].assert_called_once()
        self.assertEqual(started['step_code_botw'].call_args[0][2], update.resolve())
        self.assertEqual(started['step_code_botw'].call_args.kwargs['version'], 'botw190')

    def test_botw_card_image_is_1_6_0(self):
        error, started = self.run_main(self.card_image())
        self.assertIsInstance(error, Stop)
        self.assertEqual(started['step_code_botw'].call_args.kwargs['version'], 'botw160')
        self.assertEqual(started['prepare_work'].call_args[0][0], self.root.resolve() / 'BotW_build')

    def test_botw_nsp_with_update_is_1_9_0(self):
        base, update = package(self.root, 'botw.nsp', BOTW_BASE), package(self.root, 'botw-u.nsp', BOTW_UPDATE)
        error, started = self.run_main(base, update)
        self.assertIsInstance(error, Stop)
        # the update NSP is the ExeFS source of step_code_botw, as for a Tears of the Kingdom update
        self.assertEqual(started['step_code_botw'].call_args[0][2], update.resolve())
        self.assertEqual(started['step_code_botw'].call_args.kwargs['version'], 'botw190')

    def test_botw_files_on_the_wrong_flag_stop_before_any_step(self):
        base, update = package(self.root, 'botw.nsp', BOTW_BASE), package(self.root, 'botw-u.nsp', BOTW_UPDATE)
        error, started = self.run_main(update, base)
        self.assertIn('pass it as --update', str(error))
        error, started = self.run_main(base, package(self.root, 'totk-u.nsp', TOTK_UPDATE))
        self.assertIn('Tears of the Kingdom', str(error))
        started['step_code_botw'].assert_not_called()

    def test_botw_build_fonts_serve_botw_only(self):
        # valid fonts in BotW_build next to the game file pass the early NSP font check; a TOTK NSP stops right after
        # identify_game, before any long step, as without them
        botw_fonts = lambda folder: 'BotW_build' in str(folder)
        with mock.patch.object(build_app, 'fonts_valid', side_effect=botw_fonts):
            base, update = package(self.root, 'botw.nsp', BOTW_BASE), package(self.root, 'botw-u.nsp', BOTW_UPDATE)
            error, started = self.run_main(base, update, fonts=False)
            self.assertIsInstance(error, Stop)
            error, started = self.run_main(package(self.root, 'totk.nsp', TOTK_BASE), game='totk', fonts=False)
            self.assertIn('NSP files do not include system shared fonts', str(error))
            started['step_code'].assert_not_called()

    def test_totk_route_is_unchanged(self):
        error, started = self.run_main(package(self.root, 'totk.nsp', TOTK_BASE), game='totk')
        self.assertIsInstance(error, Stop)
        started['step_code'].assert_called_once()
        started['step_code_botw'].assert_not_called()
        self.assertEqual(started['prepare_work'].call_args[0][0], self.root.resolve() / 'TOTK_build')


if __name__ == '__main__':
    unittest.main()

"""S5.4 C3 (R1-R3): code mods through the build. tools/builder/mod_code.py on a real library in a temporary folder;
the --mod argv parsed by build_app.py itself; prepare_request composing at the build's start; BuildRun calling the
record's writer on success only; and the controller's start_build carrying it all (its library patched in)."""
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest.mock import patch

import build_app
from builder_fake_process import Clock, FakeProcess, T, drain
from builder_fakes import FakeRunner, ok
from test_builder_build_run import Base as RunBase
from test_builder_controller import Base as ControllerBase
from tools.builder import controller_mods, mod_code, mod_library
from tools.builder.build_request import prepare_request
from tools.builder.build_run import BuildRequest, BuildRun
from tools.builder.checks import BuildPaths
from tools.builder.command import build_argv
from tools.builder.config import SavedPaths
from tools.builder.games import TOTK
from tools.builder.jobs import InlineJobs
from tools.builder.mod_tools import Validation
from tools.builder.session import BuildSession, Finished, Outcome
from tools.builder.start_options import StartPaths

REPO = Path(__file__).resolve().parents[1]
OK, OTHER = Validation(True, code='ok'), Validation(True, code='other_build')
PCHTXT = '@nsobid-{}\n@flag offset_shift 0x100\n@enabled\n0001 00\n@stop\n'


def library_in(root):
    library = mod_library.open_library(root, 'v143', root / 'load')
    return library


def add_code(library, mod_id, files, on=True):
    exefs = library.path(mod_id) / 'exefs'
    exefs.mkdir(parents=True)
    for name, data in files.items():
        (exefs / name).write_bytes(data)
    library.state['order'].append(mod_id)
    if on:
        mod_code.set_code(library, mod_id, True, OK)
    return library


class Model(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        self.library = library_in(self.tmp / 'TOTK')

    def test_only_mergeable_code_can_be_switched_on_and_the_order_is_the_list(self):
        add_code(self.library, 'b', {'main.pchtxt': b'b'}, on=False)
        add_code(self.library, 'a', {'main.pchtxt': b'a'}, on=False)
        self.assertRaises(ValueError, mod_code.set_code, self.library, 'a', True, OTHER)
        for mod_id in ('a', 'b'):
            mod_code.set_code(self.library, mod_id, True, OK)
        self.assertEqual(mod_code.included(self.library), ['b', 'a'])
        mod_code.set_code(self.library, 'b', False, OTHER)               # switching off needs no check
        self.assertEqual(mod_code.included(self.library), ['a'])

    def test_compose_renames_pchtxt_and_leaves_out_what_is_no_patch(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a', 'main.npdm': b'n', 'subsdk9': b's'})
        add_code(self.library, 'b', {'main.pchtxt': b'b', 'main.ips': b'PATCHEOF'})
        folder, record = mod_code.compose(self.library, ['a', 'b'])
        self.assertEqual(sorted(p.name for p in (folder / 'exefs').iterdir()),
                         ['a-main.pchtxt', 'b-main.pchtxt', 'main.ips'])
        self.assertEqual([(m, [f for f, _ in files]) for m, files in record],
                         [('a', ['a-main.pchtxt']), ('b', ['main.ips', 'b-main.pchtxt'])])

    def test_two_ips_for_main_are_refused_and_nothing_is_left(self):
        add_code(self.library, 'a', {'main.ips': b'PATCHEOF'})
        add_code(self.library, 'b', {'MAIN.ips': b'PATCHEOF'})
        self.assertRaisesRegex(ValueError, 'two IPS patches', mod_code.compose, self.library, ['a', 'b'])
        self.assertFalse(mod_code.folder(self.library).exists())
        self.assertEqual(mod_code.compose(self.library, []), (None, ()))

    def test_the_record_and_build_again(self):
        app = self.tmp / 'Tears of the Kingdom.app'
        self.assertFalse(mod_code.pending(self.library, app))            # nothing on, nothing recorded
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertTrue(mod_code.pending(self.library, app))
        _, record = mod_code.compose(self.library, ['a'])
        mod_code.write_record(self.library.root, record, app)
        self.assertFalse(mod_code.pending(self.library, app))
        self.assertTrue(mod_code.pending(self.library, self.tmp / 'Other.app'))   # recorded for another app
        (self.library.path('a') / 'exefs/main.pchtxt').write_bytes(b'changed')
        self.assertTrue(mod_code.pending(self.library, app))

    def test_planned_only_for_totk_with_an_update(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertEqual([mod_code.planned(self.library, g, u) for g, u in (('totk', True), ('totk', False),
                                                                             ('botw', True))], [('a',), (), ()])


class Command(unittest.TestCase):
    def test_build_app_reads_the_folder_as_its_mod_and_installs_no_romfs(self):
        paths = BuildPaths(Path('/G/t.xci'), Path('/k/prod.keys'), update=Path('/G/u.nsp'))
        args = build_app.parse_args(build_argv(TOTK, paths, REPO, 'py', Path('/L/Builder code mods'))[2:])
        self.assertEqual((args.mod, args.install_mod_romfs), (Path('/L/Builder code mods'), False))
        self.assertNotIn('--mod', build_argv(TOTK, paths, REPO, 'py'))

    def test_build_app_still_merges_a_mod_into_the_updated_main_only(self):        # drift, R1
        source = Path(build_app.__file__).read_text()
        self.assertIn("step_code(tree, game, keys, env, logs, mod=None if update else code_mod)", source)
        self.assertIn("(mod / 'exefs').is_dir() or (mod / 'romfs').is_dir()", source)


class Prepare(unittest.TestCase):
    def test_composed_at_the_start_and_carried_in_the_request(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        fields = (('game', '/G/t.xci'), ('keys', '/k'), ('update', '/G/u.nsp'))
        runner, cancel, calls = FakeRunner({lambda argv: argv[0] == 'git': ok('cafe\n')}), threading.Event(), []
        compose = lambda: calls.append(1) or (Path('/L/code'), (('a', (('a-main.pchtxt', 'f00'),)),), 'writer')
        request = prepare_request(TOTK, fields, REPO, 'py', Path(tmp), 0, runner, cancel, compose)
        self.assertEqual(request.argv[-2:], ('--mod', '/L/code'))
        self.assertEqual((request.code[0][0], request.on_success), ('a', 'writer'))
        cancel.set()
        self.assertIsNone(prepare_request(TOTK, fields, REPO, 'py', Path(tmp), 0, runner, cancel, compose))
        self.assertEqual(calls, [1])                                     # a cancel first composes nothing


class RecordOnSuccess(RunBase):
    def run_with(self, end, *lines):
        written, self.jobs = [], InlineJobs()
        self.request = BuildRequest('totk', None, SavedPaths(game='/g.xci', keys='/k'), ('py',), self.tmp / 'c.log',
                                    'abc', 'py', (('a', ()),), written.append)
        self.run = BuildRun(self.request, FakeProcess(), BuildSession('totk'), self.jobs, self.store, Clock(),
                            self.cancel, self.events.append)
        self.proc = self.run.process
        self.started()
        self.proc.emit(*lines)
        self.proc.end(end)
        self.finish_jobs()
        return written

    def test_written_on_success_only(self):
        self.assertEqual(self.run_with(0, T + 'work folder /w (1 GB free on its volume)', T + 'done in 1 min: /A.app'),
                         ['/A.app'])
        self.assertEqual(self.run_with(1, 'ERROR: this patch is for another build of main'), [])


class StartBuild(ControllerBase):
    """A real start_build over the real library lookup (main's tools from REPO; supported patched, HOME inside)."""

    def setUp(self):
        super().setUp()
        for patcher in (patch.object(controller_mods.mod_tools, 'supported', return_value=True),
                        patch.dict(os.environ, HOME=str(self.tmp / 'home'))):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.ctl.repo_root, self.ctl.start.paths = REPO, StartPaths(self.tmp / 'Builder/s.json', self.tmp / 'data')
        self.library = add_code(library_in(self.tmp / 'data/TOTK'), 'a', {'main.pchtxt': b'a'})
        mod_library.save(self.library)
        self.app = self.tmp / 'A.app'

    def build(self, update='/u.nsp', app=None, change=None):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'), update=update)
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        self.ctl.start_build()
        if change:
            change()
        drain(self.jobs, self.ctl.tick)
        proc = self.procs[-1]
        proc.emit(T + 'work folder /w (1 GB free on its volume)', T + f'done in 1 min: {app or self.app}')
        proc.end(0)
        drain(self.jobs, self.ctl.tick)
        self.assertTrue([e for e in self.events() if isinstance(e, Finished) and e.outcome is Outcome.SUCCESS])
        return proc.started[0]

    def recorded(self, app=None):
        return [m['id'] for m in mod_code.read_records(self.library.root)[str(app or self.app)]]

    def switch(self, on):
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        mod_code.set_code(library, 'a', on, OK)
        mod_library.save(library)

    def test_the_toggles_at_build_are_composed_and_dry_run_writes_nothing(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'), update='/u.nsp')
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        folder = str(mod_code.folder(self.library))
        self.assertEqual(self.ctl.planned_command()[-2:], ['--mod', folder])
        self.assertFalse(Path(folder).exists())
        argv = self.build(change=lambda: self.switch(False))             # after Build: no effect on this run
        self.assertEqual((argv[-2:], self.recorded()), (['--mod', folder], ['a']))

    def test_switched_off_then_built_clears_the_record(self):              # O2, the audit's B1
        self.build()
        self.switch(False)
        argv = self.build()
        self.assertEqual(('--mod' in argv, self.recorded()), (False, []))
        self.assertFalse(mod_code.pending(mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp), self.app))

    def test_without_the_update_no_code_and_build_again_stays(self):
        argv = self.build(update=None)
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp)
        self.assertEqual(('--mod' in argv, self.recorded(), mod_code.pending(library, self.app)), (False, [], True))

    def test_two_apps_keep_their_own_records(self):
        self.build()
        self.switch(False)
        self.build(app=self.tmp / 'B.app')
        self.assertEqual((self.recorded(), self.recorded(self.tmp / 'B.app')), (['a'], []))
        self.ctl.close()

    def test_another_game_has_no_library(self):
        snapshot = ('botw', (('game', '/g.xci'), ('keys', '/k'), ('update', '/u.nsp')))
        self.assertEqual(controller_mods._build_library(REPO, self.tmp / 'data', snapshot), (None, ()))


if __name__ == '__main__':
    unittest.main()

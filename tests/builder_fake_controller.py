"""Test helpers for S2.1's text mode: a scripted controller, the CLI's arguments and a Ctrl-C schedule."""
from argparse import Namespace
from collections import deque

from tools.builder.config import PATH_FIELDS
from tools.builder.session import ChecksDone, SettingsLoaded


class FakeStart:
    """controller.start (S5.1): records calls; busy while a call waits for the next tick, refused then like the real
    one; the test delivers the events (StartStateReady, PlayDone, GpuProbed) through the controller's events."""

    def __init__(self, owner):
        self.owner, self.calls, self.pending, self.gpu = owner, [], False, None

    @property
    def busy(self):
        return self.pending

    def _call(self, *call):
        self.calls.append(call)
        if self.pending:
            raise RuntimeError('a settings change is still running')
        self.pending = True

    def _game(self):
        if self.owner.selection()[0] is None:
            raise RuntimeError('no game chosen')

    def state(self):
        self._call('state')
        self._game()

    def choose(self, option, choice):
        self._call('choose', option, choice)
        self._game()

    def play(self):
        self._call('play')
        if not self.owner.idle:
            raise RuntimeError('a build is running or being prepared')
        self._game()

    def probe(self):
        self.calls.append(('probe',))
        self.gpu = self.gpu or 'checking'


class FakeMods:
    """controller.mods (S5.2): records calls; busy while a call waits for the next tick, refused then like the real
    one; Apply refused unless the controller is idle; the test delivers ModsStateReady / ModsApplied."""

    def __init__(self, owner):
        self.owner, self.calls, self.pending = owner, [], False

    @property
    def busy(self):
        return self.pending

    @property
    def applying(self):
        return self.pending and self.calls and self.calls[-1][0] == 'apply'

    @property
    def holds(self):
        return self.applying

    def cancel(self):
        self.cancels = getattr(self, 'cancels', 0) + 1

    def _call(self, *call):
        self.calls.append(call)
        if self.pending:
            raise RuntimeError('a mods change is still running')
        if self.owner.selection()[0] is None:
            raise RuntimeError('no game chosen')
        self.pending = True

    def state(self, full=True):
        self._call('state') if full else self._call('state', 'quick')

    def code(self, mod_id, on):
        self._call('code', mod_id, on)

    def add(self, folder):
        self._call('add', folder)

    def remove(self, mod_id):
        self._call('remove', mod_id)

    def enable(self, mod_id, on):
        self._call('enable', mod_id, on)

    def move(self, mod_id, step):
        self._call('move', mod_id, step)

    def remove_backups(self, names):
        self._call('remove_backups', tuple(names))

    def apply(self):
        if not self.owner.idle or self.owner.start.busy:
            self.calls.append(('apply',))
            raise RuntimeError('a build, an update or Play is running')
        self._call('apply')


class FakeControls:
    """controller.controls (E6 S6.2b): records calls; busy until the next tick, refused then like the real one; swap is
    the model's; the test delivers ControlsReady / ControlsSaved."""

    def __init__(self, owner):
        self.owner, self.calls, self.pending = owner, [], False

    @property
    def busy(self):
        return self.pending

    @property
    def holds(self):
        return self.pending and self.calls[-1][0] in ('save', 'restore', 'update_icons')

    @staticmethod
    def swap(inputs, switch_input, drawn):
        from tools.builder.controls_file import swap
        return swap(inputs, switch_input, drawn)

    @staticmethod
    def xbox_letters(inputs):
        from tools.builder.controls_file import xbox_letters
        return xbox_letters(inputs)

    def _call(self, *call):
        self.calls.append(call)
        if self.pending:
            raise RuntimeError('a controls change is still running')
        self.pending = True

    def read(self):
        self._call('read')

    def answer(self, kind):
        self._call('answer', kind)

    def save(self, state, desired, converted=()):
        self._call('save', dict(desired), tuple(sorted(converted)))

    def restore(self, state):
        self._call('restore')

    def icons_check(self):
        self._call('icons_check')

    def update_icons(self, install_env=False, switch_off=()):
        self._call('update_icons', bool(install_env), tuple(switch_off))


class FakeController:
    """The BuildController methods the CLI uses. Each command queues its script, one batch of events per tick; after
    the build's batches, the controller stays busy (not idle) for saving_ticks more ticks, then becomes idle."""

    def __init__(self, load=None, checks=None, build=(), saving_ticks=0):
        self.load = [[SettingsLoaded(())]] if load is None else load
        self.checks = [[ChecksDone(True)]] if checks is None else checks
        self.build, self.saving_ticks = list(build), saving_ticks
        self.batches, self.events, self.calls = deque(), [], []
        self.busy, self.cancel_failed, self.closes, self.ticks = False, False, 0, 0
        self.planned = ['/py', 'build_app.py', 'totk']
        self.tick_errors = deque()              # S2.1b: exceptions tick() raises during a build, one per call
        self.saved, self.game, self.fields = {}, None, {}       # S2.2: like the controller's selection
        self.update_calls, self.repo_root = [], '/repo'
        self.background_calls = []                                  # S4.3
        self.update_pending = self.background_pending = False       # S4.4b: refused like the real controller
        self.background_forced = False                              # a test may hold the picture "busy"
        self.start = FakeStart(self)                                # S5.1: refuses like controller_start
        self.mods = FakeMods(self)                                  # S5.2: refuses like controller_mods
        self.controls = FakeControls(self)                          # E6 S6.2b

    @property
    def background_busy(self):
        return self.background_pending or self.background_forced

    @background_busy.setter
    def background_busy(self, value):
        self.background_forced = value

    @property
    def idle(self):
        return not self.busy and not self.update_pending and not self.controls.holds

    def load_settings(self):
        self.calls.append('load_settings')
        self.batches.extend(self.load)

    def select_game(self, game_id):
        self.calls.append(('select_game', game_id))
        self.game, self.fields = game_id, dict(self.saved.get(game_id, {}))   # refilled from the saved paths

    def set_paths(self, **fields):
        self.calls.append(('set_paths', fields))
        self.fields.update(fields)                                              # a None stays None

    def selection(self):
        return self.game, dict(self.fields)

    def games(self):
        return (('totk', 'Tears of the Kingdom'), ('botw', 'Breath of the Wild'))

    def path_fields(self):
        return PATH_FIELDS

    def check_updates(self):                    # S4.1: recorded apart, so call-order tests keep their lists
        self.update_calls.append('check_updates')

    def apply_update(self):
        """S4.4b: recorded first, then refused like the real one when not idle; pending until the next tick."""
        self.update_calls.append('apply_update')
        if not self.idle:
            raise RuntimeError('a build is running or being prepared')
        self.update_pending = True

    def background(self, action, *args):
        """S4.4b: recorded first, then refused like the real one while a change is pending (until the next tick)."""
        self.background_calls.append((action, *args))
        if self.background_pending:
            raise RuntimeError('a background change is still running')
        self.background_pending = True

    def run_checks(self):
        self.calls.append('run_checks')
        self.batches.extend(self.checks)

    def planned_command(self):
        self.calls.append('planned_command')
        return self.planned

    def start_build(self):
        self.calls.append('start_build')
        self.busy = True
        self.batches.extend(self.build)

    def cancel(self):
        self.calls.append('cancel')

    def tick(self):
        self.ticks += 1
        self.update_pending = self.background_pending = self.start.pending = self.mods.pending = False
        self.controls.pending = False
        if self.busy and self.tick_errors:
            raise self.tick_errors.popleft()
        if self.batches:
            self.events += self.batches.popleft()
        elif self.busy and self.saving_ticks:
            self.saving_ticks -= 1
        elif self.busy:
            self.busy = False

    def take_events(self):
        events, self.events = self.events, []
        return events

    def close(self):
        if self.busy:
            raise RuntimeError('a build is running, being prepared or saving')
        self.closes += 1


def cli_args(**values):
    base = {'cli': True, 'game': 'totk', 'check_only': False, 'dry_run': False, 'yes': False, 'paths': {}}
    return Namespace(**{**base, **values})


class CtrlC:
    """interrupted(): raises KeyboardInterrupt on the given loop turns (1-based), and can run a hook on a turn."""

    def __init__(self, *turns, hooks=None):
        self.turns, self.hooks, self.n = set(turns), hooks or {}, 0

    def __call__(self):
        self.n += 1
        if self.n in self.hooks:
            self.hooks[self.n]()
        if self.n in self.turns:
            raise KeyboardInterrupt

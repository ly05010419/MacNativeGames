"""The text mode: a view of the BuildController in a terminal (S2.1, T5 and T7).

View only: it reads the controller's events, prints them through strings.text() and turns the outcome into an exit code.
Everything it needs from outside is injected: out(line), ask(prompt) -> str, sleep(seconds) and interrupted(), which is
called once per loop turn and raises KeyboardInterrupt there after a Ctrl-C (S2.1b, H1: __main__'s SigintFlag; tests
drive it the same way). The path flags come in as args.paths, only those given (H2)."""
from collections import deque
import shlex

from tools.builder import check_report
from tools.builder.session import (BuildStarted, CancelStuck, CheckResultReady, ChecksDone, Finished, Heartbeat,
                                   LogLine, Outcome, PathsIncomplete, ProcessError, SettingsLoaded, SettingsNotSaved,
                                   StageChanged)
from tools.builder.strings import cancel_report, check_sentence, text

TICK_S = 0.2
LAST_LINES = 20
EXIT_CODES = {Outcome.SUCCESS: 0, Outcome.FAILED: 1, Outcome.INCOMPLETE: 1, Outcome.GAME_MISMATCH: 1,
              Outcome.START_FAILED: 3, Outcome.CANCELLED: 130}
NO_FINISHED, BLOCKED, INTERRUPTED = 1, 2, 130
TICK_FAILURES = 3              # consecutive tick() errors after which a Ctrl-C may stop tracking (S2.1b, g2)
LOAD_WAIT_TURNS = 50           # 10 s: then go on without the saved settings (S3.4, f4)


class CliView:
    def __init__(self, controller, out, ask, sleep, interrupted):
        self.controller, self.out, self.ask, self.sleep, self.interrupted = controller, out, ask, sleep, interrupted
        self._queued = deque()
        self._last = deque(maxlen=LAST_LINES)
        self._log, self._abandoned = None, False

    def say(self, key, **values):
        self.out(text(key, **values))

    def run(self, args) -> int:
        """T5. A Ctrl-C before a build starts returns 130; during a build _build() handles it. One close(), here."""
        try:
            return self._run(args)
        except KeyboardInterrupt:
            self.say('cli.interrupted')
            return INTERRUPTED
        finally:
            if not self._abandoned:             # g2: a busy controller cannot close; tracking was given up
                self.controller.close()

    def _run(self, args):
        c = self.controller
        c.load_settings()
        if self._until(self._on_load, turns=LOAD_WAIT_TURNS) is None:
            self.say('cli.load_slow', seconds=int(LOAD_WAIT_TURNS * TICK_S))      # a late answer changes no choice
        c.select_game(args.game)
        if args.paths:
            c.set_paths(**args.paths)               # only the flags given: None would clear a saved path
        c.run_checks()
        verdict = self._until(self._on_check)
        if verdict != 'ready':
            if verdict == 'blocked':
                self.say('cli.checks_blocked')
            return BLOCKED
        self.interrupted()                          # g1: a Ctrl-C in the checks' last turn stops here, not the build
        if args.dry_run:
            self.say('cli.command', command=shlex.join(c.planned_command()))     # copy-pasteable
            return 0
        self.say('cli.checks_ready')
        if args.check_only:
            self._say_logs(args.game)               # S2.3: the same log lines the window's summary shows
            return 0
        if not args.yes and self.ask(text('cli.prompt')).strip().lower() != 'y':
            self.say('cli.not_started')
            return 0
        self.interrupted()                          # S2.1b P3-1: a Ctrl-C around the prompt starts nothing
        c.start_build()
        return self._build()

    def _until(self, handle, turns=None):
        """Ticks until handle(event) returns something; events after that one wait for the next call. With turns,
        None after that many turns without an answer."""
        turn = 0
        while turns is None or turn < turns:
            turn += 1
            self.interrupted()
            self.controller.tick()
            self._queued.extend(self.controller.take_events())
            while self._queued:
                result = handle(self._queued.popleft())
                if result is not None:
                    return result
            self.sleep(TICK_S)

    def _on_load(self, event):
        if isinstance(event, SettingsLoaded):
            for problem in event.problems:
                self.say('cli.settings_problem', problem=problem)
            return True
        if isinstance(event, ProcessError) and event.where == 'load':
            self.say('cli.process_error', where=event.where, error=event.error)
            return True
        return None

    def _say_logs(self, game):
        """The logs folder and each log, from check_report.py: the window's summary reads the same source."""
        logs = getattr(self.controller, 'logs_dir', None)
        if logs is None:
            return
        for line in check_report.log_lines(logs, game):
            self.say('cli.logs', path=line)

    def _on_check(self, event):
        if isinstance(event, PathsIncomplete):
            self.say('cli.paths_missing', fields=', '.join(text(f'cli.flag.{f}') for f in event.missing))
            return 'incomplete'
        if isinstance(event, CheckResultReady):
            self._check_line(event.result)
        elif isinstance(event, ProcessError):
            self.say('cli.process_error', where=event.where, error=event.error)
        elif isinstance(event, ChecksDone):
            return 'ready' if event.ready else 'blocked'
        return None

    def _check_line(self, r):
        label = text(f'check.{r.id}')
        if r.status.name == 'OK':
            self.say('cli.check_ok', label=label)
        else:
            sentence = check_sentence(r.message_id, r.detail)
            self.say('cli.check', status=r.status.name, label=label, sentence=sentence)
        if r.fix:
            self.say('cli.fix', fix=r.fix)

    def _build(self):
        """T5 steps 6-8: ticks until the controller is idle (T8), so close() is allowed and the saves are done."""
        c, finished, ctrl_c, failures, last_error = self.controller, None, 0, 0, None
        while not c.idle:
            try:
                self.interrupted()
                try:
                    c.tick()
                except Exception as e:      # H5: R4 has the view tick again; the view's one deliberate broad except
                    failures += 1
                    if repr(e) != last_error:
                        self.say('cli.tick_error', error=repr(e))
                    last_error = repr(e)
                    self.sleep(TICK_S)
                    continue
                failures, last_error = 0, None
                for event in c.take_events():
                    finished = self._on_build(event) or finished
                if not c.idle:
                    self.sleep(TICK_S)
            except KeyboardInterrupt:
                if failures >= TICK_FAILURES:
                    self.say('cli.tracking_lost', log=self._log or 'unknown')
                    self._abandoned = True
                    return NO_FINISHED
                if ctrl_c == 0 or c.cancel_failed:
                    c.cancel()
                    self.say('cli.cancelling' if ctrl_c == 0 else 'cli.retrying')
                else:
                    self.say('cli.still_stopping')
                ctrl_c += 1
        if finished is None:
            self.say('cli.no_finished')
            return NO_FINISHED
        return self._report(finished)

    def _on_build(self, event):
        if isinstance(event, BuildStarted):
            self._log = event.log_path
            self.say('cli.started', log=event.log_path)
        elif isinstance(event, StageChanged):
            self.say('cli.stage', index=event.index, total=event.total, name=event.name, text=event.text)
        elif isinstance(event, Heartbeat):
            self.say('cli.heartbeat', minutes=event.minutes)
        elif isinstance(event, ProcessError):
            self.say('cli.process_error', where=event.where, error=event.error)
            if event.where in ('cancel', 'kill'):
                self.say('cli.retry_hint')
        elif isinstance(event, CancelStuck):
            self.say('cli.cancel_stuck')
        elif isinstance(event, SettingsNotSaved):
            self.say('cli.settings_not_saved', error=event.error)
        elif isinstance(event, LogLine):
            self._last.append(event.text)           # not printed: the log file has every line
        elif isinstance(event, Finished):
            return event
        return None

    def _report(self, f):
        self.out(cancel_report(f) if f.outcome is Outcome.CANCELLED else text(f'outcome.{f.outcome.value}'))
        if f.app:
            self.say('cli.app', app=f.app)
        if f.error:
            self.say('cli.error', error=f.error)
        if f.outcome in (Outcome.FAILED, Outcome.INCOMPLETE) and self._last:
            self.say('cli.last_lines', n=len(self._last))
            for line in self._last:
                self.out(line)
        if f.log_path:
            self.say('cli.log', log=f.log_path)
        return EXIT_CODES[f.outcome]

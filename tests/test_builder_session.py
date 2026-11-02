"""tools/builder/session.py (S1.7a): BuildSession on plain lists of S1.4 events, and the outcome of finish().
S1.8b: the cancel report (termination, signals_sent, cancel_compatible, observed)."""
import signal
import unittest

from tools.builder.progress import parse_line
from tools.builder.session import (BuildSession, Finished, GameMismatch, Heartbeat, LogLine, Outcome, StageChanged)

T = '[12:00:00] '


def feed_all(session, lines):
    out = []
    for line in lines:
        out += session.feed(parse_line(line))
    return out


class Feed(unittest.TestCase):
    def test_each_event_type(self):
        s = BuildSession('totk')
        self.assertEqual(s.feed(parse_line(T + '2/4 data: extracting')),
                         [StageChanged(2, 4, 'data', 'extracting'), LogLine(T + '2/4 data: extracting')])
        self.assertEqual(s.feed(parse_line(T + '   ... still running (4 min)')), [Heartbeat(4)])
        for line in (T + 'work folder /w/TOTK_build (400 GB free on its volume)',
                     T + 'update identified by verified ExeFS hashes: 1.4.3',
                     T + 'done in 9 min: /A/Tears of the Kingdom.app', 'ERROR: x', 'plain text'):
            with self.subTest(line=line):
                self.assertEqual(s.feed(parse_line(line)), [LogLine(line)])

    def test_totk_prints_code_twice(self):
        out = feed_all(BuildSession('totk'), [T + '1/4 code: base', T + '1/4 code: update', T + '2/4 data: x'])
        self.assertEqual([(e.index, e.text) for e in out if isinstance(e, StageChanged)],
                         [(1, 'base'), (1, 'update'), (2, 'x')])

    def test_versions(self):
        totk = BuildSession('totk')
        feed_all(totk, [T + 'work folder /w (1 GB free on its volume)'])
        self.assertEqual((totk.game, totk.version), ('totk', '1.0.0'))
        feed_all(totk, [T + 'update identified by verified ExeFS hashes: 1.2.1'])
        self.assertEqual(totk.version, '1.2.1')
        botw = BuildSession('botw')
        feed_all(botw, [T + 'Breath of the Wild 1.6.0; work folder /w'])
        self.assertEqual((botw.game, botw.version), ('botw', '1.6.0'))
        self.assertIsNone(BuildSession('totk').version)

    def test_mismatch_reported_once(self):
        botw_line = T + 'Breath of the Wild 1.6.0; work folder /w'
        out = feed_all(BuildSession('totk'), [botw_line, botw_line])
        self.assertEqual([e for e in out if isinstance(e, GameMismatch)], [GameMismatch('totk', 'botw')])
        self.assertEqual(sum(isinstance(e, LogLine) for e in out), 2)

    def test_multi_line_error(self):
        s = BuildSession('totk')
        feed_all(s, ['', 'ERROR: rewrite failed (exit 1); full output: /w/logs/r.log', 'line a', 'line b'])
        self.assertEqual(s.finish(1, False).error, 'rewrite failed (exit 1); full output: /w/logs/r.log\nline a\nline b')


class Outcomes(unittest.TestCase):
    MISMATCH = T + 'Breath of the Wild 1.6.0; work folder /w'
    DONE = T + 'done in 9 min: /A/T.app'

    def session(self, *lines):
        s = BuildSession('totk')
        feed_all(s, lines)
        return s

    def test_priority_order(self):
        both = lambda: self.session(self.MISMATCH, self.DONE, 'ERROR: e')
        self.assertEqual(both().finish(0, True, start_error=OSError('no python')).outcome, Outcome.START_FAILED)
        self.assertEqual(both().finish(0, True, output_complete=False).outcome, Outcome.GAME_MISMATCH)
        self.assertEqual(self.session(self.DONE).finish(0, True, output_complete=False).outcome, Outcome.CANCELLED)
        self.assertEqual(self.session(self.DONE).finish(0, False, output_complete=False).outcome, Outcome.INCOMPLETE)
        self.assertEqual(self.session(self.DONE).finish(0, False).outcome, Outcome.SUCCESS)
        self.assertEqual(self.session('ERROR: e').finish(1, False).outcome, Outcome.FAILED)

    def test_success_needs_done_and_exit_zero(self):                        # S1.8b: termination is now filled
        self.assertEqual(self.session(self.DONE).finish(0, False, log_path='/l.log'),
                         Finished(Outcome.SUCCESS, 0, '/A/T.app', None, '/l.log', termination='exit 0'))
        self.assertEqual(self.session().finish(0, False),
                         Finished(Outcome.FAILED, 0, None, 'exit code 0', None, termination='exit 0'))
        self.assertEqual(self.session(self.DONE).finish(1, False).outcome, Outcome.FAILED)

    def test_incomplete_after_done_carries_the_read_error(self):
        f = self.session(self.DONE).finish(0, False, output_complete=False, read_error=OSError('EIO'))
        self.assertEqual((f.outcome, f.error, f.app, f.exit_code), (Outcome.INCOMPLETE, 'EIO', None, 0))

    def test_failure_texts(self):
        self.assertEqual(self.session().finish(2, False).error, 'exit code 2')         # no ERROR: line
        self.assertEqual(self.session('ERROR: e').finish(1, False).error, 'e')
        self.assertEqual(self.session(self.MISMATCH).finish(-9, True).error, 'expected totk, found botw')
        self.assertEqual(self.session().finish(None, False, start_error=FileNotFoundError('python3.14')).error,
                         'python3.14')


class CancelReport(unittest.TestCase):
    MISMATCH, DONE, session = Outcomes.MISMATCH, Outcomes.DONE, Outcomes.session    # not inherited: no rerun
    INT, KILL = signal.SIGINT, signal.SIGKILL

    def test_termination_texts(self):
        cases = [(0, 'exit 0'), (1, 'exit 1'), (130, 'exit 130'), (-2, 'signal SIGINT'), (-9, 'signal SIGKILL'),
                 (-11, 'signal SIGSEGV'), (-99, 'signal 99')]
        for code, text in cases:
            with self.subTest(code=code):
                self.assertEqual(self.session().finish(code, False).termination, text)
        self.assertIsNone(self.session().finish(None, True).termination)

    def test_cancel_compatible_pairs(self):
        cases = [(-2, (self.INT,), True), (130, (self.INT,), True), (-9, (self.INT, self.KILL), True),
                 (-11, (self.INT,), False), (130, (), False), (130, (self.KILL,), False), (-9, (self.INT,), False),
                 (0, (self.INT,), False), (-2, (), False), (-9, (), False)]
        for code, sent, compatible in cases:
            with self.subTest(code=code, sent=sent):
                f = self.session().finish(code, True, signals_sent=sent)
                self.assertEqual((f.outcome, f.cancel_compatible, f.signals_sent), (Outcome.CANCELLED, compatible, sent))

    def test_observed_is_the_outcome_without_the_request(self):
        f = self.session(self.DONE).finish(0, True, signals_sent=(self.INT,))
        self.assertEqual((f.outcome, f.cancel_compatible, f.observed, f.app),
                         (Outcome.CANCELLED, False, Outcome.SUCCESS, None))
        self.assertEqual(self.session('ERROR: e').finish(1, True).observed, Outcome.FAILED)
        self.assertEqual(self.session(self.DONE).finish(0, True, output_complete=False).observed, Outcome.INCOMPLETE)

    def test_no_cancel_fills_only_the_facts(self):
        f = self.session(self.DONE).finish(0, False, signals_sent=(self.INT,))
        self.assertEqual((f.outcome, f.termination, f.signals_sent), (Outcome.SUCCESS, 'exit 0', (self.INT,)))
        self.assertIsNone(f.cancel_compatible)
        self.assertIsNone(f.observed)

    def test_mismatch_is_never_judged(self):
        for player_cancel in (False, True):
            with self.subTest(player_cancel=player_cancel):
                f = self.session(self.MISMATCH).finish(-2, player_cancel, signals_sent=(self.INT,))
                self.assertEqual((f.outcome, f.termination, f.signals_sent),
                                 (Outcome.GAME_MISMATCH, 'signal SIGINT', (self.INT,)))
                self.assertIsNone(f.cancel_compatible)
                self.assertIsNone(f.observed)

    def test_cancelled_before_anything_ran(self):
        f = self.session().finish(None, True)
        self.assertEqual((f.outcome, f.termination, f.signals_sent), (Outcome.CANCELLED, None, ()))
        self.assertIsNone(f.cancel_compatible)
        self.assertIsNone(f.observed)


if __name__ == '__main__':
    unittest.main()

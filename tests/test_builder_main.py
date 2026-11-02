"""tools/builder/__main__.py (S2.1b, H1-H3; S2.2, W4): the SIGINT flag, ask(), the arguments and the branches. The signal tests send a real
SIGINT to this process; the handler is restored by addCleanup before any assertion."""
import contextlib
import io
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools.builder.__main__ import SigintFlag, ask, main, parse_args, say
from tools.builder.config import PATH_FIELDS
from tools.builder.strings import text


class Flag(unittest.TestCase):
    def installed(self):
        self.addCleanup(signal.signal, signal.SIGINT, signal.getsignal(signal.SIGINT))
        flag = SigintFlag()
        flag.install()
        return flag

    def test_a_sigint_only_sets_the_flag(self):
        flag = self.installed()
        try:
            os.kill(os.getpid(), signal.SIGINT)
        except KeyboardInterrupt:                               # g3: a FAIL, not an aborted suite
            self.fail('the SIGINT handler raised')
        self.assertRaises(KeyboardInterrupt, flag)
        try:
            flag()                                              # cleared: a second call does not raise
        except KeyboardInterrupt:                               # g3: a FAIL, not an aborted suite
            self.fail('the flag was not cleared')

    def test_a_closed_terminal_or_sigterm_sets_the_same_flag(self):           # S3.4, B1
        for sig in (signal.SIGHUP, signal.SIGTERM):
            with self.subTest(sig=sig):
                for s in SigintFlag.SIGNALS:
                    self.addCleanup(signal.signal, s, signal.getsignal(s))
                flag = SigintFlag()
                flag.install()
                try:
                    os.kill(os.getpid(), sig)
                except KeyboardInterrupt:                       # g3: a FAIL, not an aborted suite
                    self.fail('the handler raised')
                self.assertRaises(KeyboardInterrupt, flag)
                flag.restore()

    def test_say_survives_a_closed_terminal(self):                            # S3.4, B1
        with patch('builtins.print', side_effect=OSError(5, 'Input/output error')):
            say('a line')                                       # no exception

    def test_restore_puts_the_previous_handler_back(self):
        before = {s: signal.getsignal(s) for s in SigintFlag.SIGNALS}
        for s in SigintFlag.SIGNALS:
            self.addCleanup(signal.signal, s, before[s])
        self.installed().restore()
        self.assertEqual({s: signal.getsignal(s) for s in SigintFlag.SIGNALS}, before)


class Ask(unittest.TestCase):
    def setUp(self):
        self.addCleanup(signal.signal, signal.SIGINT, signal.getsignal(signal.SIGINT))
        self.flag = SigintFlag()
        self.flag.install()

    def test_python_handles_ctrl_c_at_the_prompt(self):
        seen = []
        def fake_input(prompt):
            seen.append(signal.getsignal(signal.SIGINT))
            return 'y'
        with patch('builtins.input', fake_input):
            self.assertEqual(ask('Build now? '), 'y')
        self.assertEqual(seen, [signal.default_int_handler])
        self.assertEqual(signal.getsignal(signal.SIGINT), self.flag._handler)

    def test_end_of_input_is_no(self):
        with patch('builtins.input', side_effect=EOFError):
            self.assertEqual(ask('Build now? '), '')
        self.assertEqual(signal.getsignal(signal.SIGINT), self.flag._handler)


class Arguments(unittest.TestCase):
    def test_only_the_paths_given(self):
        args = parse_args(['--game', 'totk', '--game-file', '/g', '--keys', '/k'])
        self.assertEqual((args.game, args.paths), ('totk', {'game': '/g', 'keys': '/k'}))

    def test_every_flag_in_the_messages_is_an_option(self):
        argv = ['--game', 'botw']
        for field in PATH_FIELDS:
            argv += [text(f'cli.flag.{field}'), f'/{field}']
        self.assertEqual(parse_args(argv).paths, {f: f'/{f}' for f in PATH_FIELDS})


class Branches(unittest.TestCase):                                              # S2.2, W4
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.addCleanup(signal.signal, signal.SIGINT, signal.getsignal(signal.SIGINT))
        self.home, self.out, self.err = Path(tmp.name), io.StringIO(), io.StringIO()

    def main(self, *argv):
        with contextlib.redirect_stdout(self.out), contextlib.redirect_stderr(self.err):
            return main(list(argv), home=self.home)

    def test_the_text_mode_needs_a_game(self):
        with self.assertRaises(SystemExit) as caught:
            self.main('--cli')
        self.assertEqual(caught.exception.code, 2)
        self.assertIn('--game is required for the text mode', self.err.getvalue())

    def test_the_window_gets_the_game_and_the_paths(self):
        try:
            import tkinter                                      # noqa: F401  c1: only Tk itself may be missing
        except ImportError:
            self.skipTest('this Python has no Tk')
        seen = []
        def opened(controller, game, paths):                    # c2: the cleanup is registered when it is called
            self.addCleanup(controller.close)
            seen.append((game, paths))
            return 0
        with patch('tools.builder.gui.open_window', side_effect=opened):
            self.assertEqual(self.main('--game', 'botw', '--keys', '/k'), 0)
        self.assertEqual(seen, [('botw', {'keys': '/k'})])

    def test_without_tk_the_text_mode_runs(self):
        with patch.dict(sys.modules, {'tkinter': None}):
            code = self.main('--game', 'totk')
        self.assertEqual(code, 2)                               # no paths: the text mode lists them
        self.assertEqual(self.out.getvalue().splitlines()[:2],
                         [text('gui.no_tk'), 'Missing paths: --game-file, --keys.'])

    def test_without_tk_and_without_a_game(self):
        with patch.dict(sys.modules, {'tkinter': None}), self.assertRaises(SystemExit) as caught:
            self.main()
        self.assertEqual((caught.exception.code, self.out.getvalue().splitlines()), (2, [text('gui.no_tk')]))


if __name__ == '__main__':
    unittest.main()

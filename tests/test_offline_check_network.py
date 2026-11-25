"""The offline store's check denies the network itself (user 2026-11-25, asked how the check is to run now that the
storage guard is retired: "a", the check's own network-off run). Until then `offline_pack.py check` proved "nothing is
downloaded" only when someone ran it under storage_guard.py run --offline, wrote its record with the network on just
the same, and could not run at all on a Mac where the guard refuses every command.

Now: `check` starts itself again under macOS's sandbox with every outbound connection but loopback denied, and writes
its record (which a release requires, checked()) only after an outbound connection was really refused."""
import errno
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.deps import offline, offline_pack  # noqa: E402

SANDBOX = Path('/usr/bin/sandbox-exec')


class TheCheckDeniesTheNetworkItself(unittest.TestCase):
    def setUp(self):
        self.store = Path(tempfile.mkdtemp(prefix='offline-check-'))
        self.addCleanup(shutil.rmtree, self.store, True)
        (self.store / offline.MANIFEST).write_text('{"schema": 2, "files": {}}\n')

    def test_the_rule_denies_every_outbound_connection_but_loopback(self):
        rule = offline_pack.NO_NETWORK
        self.assertIn('(deny network-outbound (remote ip))', rule)
        self.assertIn('(allow network-outbound (remote ip "localhost:*"))', rule)
        self.assertLess(rule.index('(deny network-outbound'), rule.index('(allow network-outbound'))   # the last match wins
        command = offline_pack.without_network(['python3', '-c', 'pass'])
        self.assertEqual(command[:3], [str(SANDBOX), '-p', rule])
        self.assertEqual(command[3:], ['python3', '-c', 'pass'])

    def test_denied_is_a_refused_connection_not_an_unreachable_one(self):
        def raising(error):
            def connect():
                raise error
            return connect
        self.assertTrue(offline_pack.network_denied(raising(PermissionError(errno.EPERM, 'Operation not permitted'))))
        self.assertTrue(offline_pack.network_denied(raising(OSError(errno.EPERM, 'Operation not permitted'))))
        self.assertFalse(offline_pack.network_denied(raising(OSError(errno.ENETUNREACH, 'Network is unreachable'))))   # no Wi-Fi is not proof
        self.assertFalse(offline_pack.network_denied(raising(TimeoutError('timed out'))))
        self.assertFalse(offline_pack.network_denied(raising(ConnectionRefusedError(errno.ECONNREFUSED, 'refused'))))
        self.assertFalse(offline_pack.network_denied(lambda: None))   # it connected

    def test_with_the_network_on_nothing_is_checked_and_no_record_is_written(self):
        with mock.patch.object(offline_pack, 'network_denied', return_value=False), \
             mock.patch.object(offline_pack, 'run', side_effect=AssertionError('the check ran with the network on')):
            with self.assertRaises(RuntimeError) as refused:
                offline_pack.check(self.store, sys.executable)
        self.assertIn('network', str(refused.exception))
        self.assertFalse((self.store / offline_pack.CHECKED).exists())
        self.assertFalse(offline_pack.checked(self.store))

    def test_check_starts_itself_again_with_the_network_denied(self):
        with mock.patch.dict(os.environ), mock.patch.object(offline_pack.subprocess, 'call', return_value=7) as call, \
             mock.patch.object(offline_pack, 'check', side_effect=AssertionError('checked outside the rule')):
            os.environ.pop(offline_pack.INSIDE, None)
            self.assertEqual(offline_pack.main(['check', str(self.store), '--python', '/some/python3.14']), 7)
        (command,), options = call.call_args
        self.assertEqual(command[:3], [str(SANDBOX), '-p', offline_pack.NO_NETWORK])
        self.assertEqual(command[3:], [sys.executable, '-B', '-m', 'tools.deps.offline_pack', 'check', str(self.store.resolve()),
                                       '--python', '/some/python3.14'])
        self.assertEqual(options['env'][offline_pack.INSIDE], '1')
        self.assertEqual(Path(options['cwd']), REPO)

    def test_inside_the_rule_the_check_itself_runs(self):
        with mock.patch.dict(os.environ, {offline_pack.INSIDE: '1'}), mock.patch.object(offline_pack.subprocess, 'call') as call, \
             mock.patch.object(offline_pack, 'check') as check:
            self.assertEqual(offline_pack.main(['check', str(self.store), '--python', '/some/python3.14']), 0)
        call.assert_not_called()
        check.assert_called_once_with(self.store.resolve(), '/some/python3.14')

    def test_building_the_store_is_not_put_under_the_rule(self):
        with mock.patch.dict(os.environ), mock.patch.object(offline_pack.subprocess, 'call') as call, \
             mock.patch.object(offline_pack, 'build') as build:
            os.environ.pop(offline_pack.INSIDE, None)
            self.assertEqual(offline_pack.main(['build', str(self.store)]), 0)   # it downloads: that is its work
        call.assert_not_called()
        build.assert_called_once()

    @unittest.skipUnless(sys.platform == 'darwin' and SANDBOX.exists(), 'macOS\'s sandbox-exec')
    def test_the_rule_really_refuses_a_connection_on_this_mac(self):
        probe = [sys.executable, '-B', '-c',
                 'import sys; sys.path.insert(0, sys.argv[1]); from tools.deps import offline_pack; print(offline_pack.network_denied())',
                 str(REPO)]
        inside = subprocess.run(offline_pack.without_network(probe), capture_output=True, text=True, timeout=60)
        self.assertEqual(inside.stdout.strip(), 'True', inside.stderr[-300:])
        outside = subprocess.run(probe, capture_output=True, text=True, timeout=60)
        self.assertEqual(outside.stdout.strip(), 'False', 'a connection counted as denied with no rule applied: ' + outside.stderr[-300:])

    def test_the_retired_guard_is_no_part_of_it(self):
        self.assertNotIn('storage_guard', (REPO / 'tools/deps/offline_pack.py').read_text())


if __name__ == '__main__':
    unittest.main()

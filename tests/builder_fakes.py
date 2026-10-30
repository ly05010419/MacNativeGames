"""Test helpers for the builder's checks: a runner that answers from a script and records every argv."""
from tools.builder.ports import CommandResult, Outcome

SELECTION = ('/usr/bin/env', '-u', 'DEVELOPER_DIR', '/usr/bin/xcode-select', '-p')


def ok(stdout=''):
    return CommandResult(Outcome.OK, 0, stdout, '', 0.0, 1, True)


def fail(outcome, stderr='err', code=1):
    return CommandResult(outcome, code if outcome is Outcome.NONZERO else None, '', stderr, 0.0,
                         None if outcome in (Outcome.NOT_FOUND, Outcome.NOT_EXECUTABLE) else 1,
                         outcome is not Outcome.TIMEOUT)


class FakeRunner:
    """script maps an argv tuple (or a callable taking the argv) to a CommandResult or an exception to raise.
    An argv missing from the script raises KeyError, so an unexpected command fails the test."""

    def __init__(self, script=None):
        self.script = dict(script or {})
        self.calls = []

    def run(self, argv, timeout_s=10.0, kill_wait_s=2.0):
        argv = tuple(str(a) for a in argv)
        self.calls.append(argv)
        answer = self.script[argv] if argv in self.script else self._match(argv)
        if callable(answer) and not isinstance(answer, CommandResult):
            answer = answer(argv)
        if isinstance(answer, BaseException):
            raise answer
        return answer

    def _match(self, argv):
        for key, answer in self.script.items():
            if callable(key) and key(argv):
                return answer
        raise KeyError(f'unscripted command: {argv}')

    def ran(self, word):
        return [c for c in self.calls if any(word in part for part in c)]

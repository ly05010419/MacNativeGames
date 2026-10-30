"""A process the player can stop (S5.5 B2, moved out of mod_archive for E6 S6.3). The model: no Tk.

run(argv, cancel, timeout_s, env=None) starts argv (a list, never a shell) in its own session with stdin closed and
polls it every POLL_S; when cancel is set or the timeout passes, the whole process group is killed (SIGKILL) and
waited for, and Stopped('cancelled') or Stopped('timeout') is raised; a program that cannot start raises
Stopped('not_started'). Otherwise it returns (return code, stdout bytes, stderr text)."""
import os
import signal
import subprocess
import tempfile
import time

POLL_S = 0.2


class Stopped(Exception):
    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}')
        self.reason, self.detail = reason, detail


def run(argv, cancel, timeout_s, env=None):
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        try:
            proc = subprocess.Popen([str(a) for a in argv], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                    start_new_session=True, env=env)
        except OSError as e:                    # missing or not executable
            raise Stopped('not_started', str(e)) from None
        deadline, why = time.monotonic() + timeout_s, None
        while proc.poll() is None:
            if cancel is not None and cancel.is_set():
                why = 'cancelled'
            elif time.monotonic() > deadline:
                why = 'timeout'
            if why:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:      # it ended between poll() and here
                    pass
                proc.wait()
                raise Stopped(why)
            time.sleep(POLL_S)
        out.seek(0)
        err.seek(0)
        return proc.returncode, out.read(), err.read().decode(errors='replace')


def last_line(stderr):
    return (stderr.strip().splitlines() or [''])[-1][-200:]

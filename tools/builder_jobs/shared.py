"""One build or update at a time on this Mac, whoever starts it (MNG Launcher's window, `tools/mng.py` in a terminal, an
AI helping the player), and its progress where every one of them can read it (user 2026-11-12: "纯用ai打包 也和launcher
能统一使用方法"):

  <home>/Library/Application Support/MacNativeGames/Builder/jobs/
    build.lock       held (flock) while a job runs; a second job is refused with who holds it
    current.json     {pid, game, app, kind, source, started, events}: the running job, removed when it ends
    current.ndjson   its events (events.py), as the window reads them; kept as last.ndjson when it ends

A window that did not start the job follows current.ndjson and shows the same progress; it cancels the job by sending
SIGINT to its pid (build_job forwards it to the build's process group)."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import time

from tools.builder_jobs.text import t


def folder(home=None):
    return Path(home or os.environ.get('MNG_HOME') or Path.home()) / 'Library/Application Support/MacNativeGames/Builder/jobs'


class Busy(Exception):
    """Another job holds the lock; the message names it."""


def current(home=None):
    """The running job's record, or None (none, or its process is gone)."""
    try:
        record = json.loads((folder(home) / 'current.json').read_text())
        os.kill(int(record['pid']), 0)
        return record
    except (OSError, ValueError, KeyError, TypeError):
        return None


class Tee:
    """A stream that writes each line to the job's own output and to current.ndjson."""

    def __init__(self, stream, path):
        self.stream, self.file = stream, open(path, 'a', encoding='utf-8')

    def write(self, text):
        self.stream.write(text)
        self.file.write(text)

    def flush(self):
        self.stream.flush()
        self.file.flush()

    def close(self):
        self.file.close()


@contextmanager
def claim(stream, source, home=None):
    """Hold the Mac's one job slot while the body runs; yields the stream to emit events into. Busy when it is held."""
    jobs = folder(home)
    jobs.mkdir(parents=True, exist_ok=True)
    lock = open(jobs / 'build.lock', 'a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        other = current(home) or {}
        raise Busy(t(f'another build or update is running on this Mac ({other.get("game", "?")}, started from '
                     f'{other.get("source", "?")}): wait for it, or stop it in MNG Launcher',
                     f'这台 Mac 上已经有一个打包或更新在进行（{other.get("game", "?")}，从 {other.get("source", "?")} 开始）：'
                     f'等它结束，或在 MNG Launcher 里停止它')) from None
    events = jobs / 'current.ndjson'
    events.write_text('')
    record = {'pid': os.getpid(), 'source': source, 'started': round(time.time()), 'events': str(events)}
    (jobs / 'current.json').write_text(json.dumps(record))
    tee = Tee(stream, events)
    try:
        yield tee, record
    finally:
        tee.close()
        (jobs / 'current.json').unlink(missing_ok=True)
        try:
            events.replace(jobs / 'last.ndjson')
        except OSError:
            pass
        lock.close()


def describe(record, **fields):
    """Add what the job is (game, app, kind) to the running record, once its command is known."""
    path = Path(record['events']).with_name('current.json')
    record.update(fields)
    path.write_text(json.dumps(record))

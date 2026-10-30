"""The builder's own Python environment for drawing button icons (E6 S6.3, C4 option A). The model: no Tk.

make_button_icons.py needs fontTools, which the launcher's Python has not. After the player agrees (the view asks,
naming the package, its version and pypi.org), install() makes <Builder>/icons-venv with the launcher's own Python (`-m
venv`) in a temporary folder, installs fonttools 4.67.0 there with pip run as `<venv>/bin/python -I -m pip` (never
bin/pip: a moved venv's scripts keep the old path), isolated from pip's config files and PIP_* variables, from pypi.org
only, wheels only, no dependencies, and only files whose SHA-256 is pinned below (read from pypi.org on 2026-10-27),
then moves it into place and checks that it imports fontTools 4.67.0. A failure leaves no venv behind. status() runs the
same check before each use, so a venv broken by an upgrade of Python is found and can be made again. Every process runs
through cancellable.run (its own session, a timeout, killed on cancel)."""
import os
from pathlib import Path
import shutil
import sys

from tools.builder import cancellable

VERSION = '4.67.0'
HASHES = ('846982e89b1861d6c9d7fcd6567aec3fa5a10ad313e7f2076045fcd339cfbd8e',   # cp314 macosx_10_15_universal2
          '952eb091689545d86d16e40f719ed7bb086dd810a07dcc9ea2ca0a81004810a3',   # cp314 macosx_10_15_x86_64
          '4304f03ed7f4ba000a8dcc941ad854bfa52e2f3b6112b8f099b6f431cf98e701')   # py3-none-any
INDEX = 'https://pypi.org/simple'
VENV_S, PIP_S, CHECK_S = 120.0, 600.0, 60.0
CHECK = 'import fontTools; print(fontTools.version)'


class EnvError(Exception):
    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}')
        self.reason, self.detail = reason, detail


def venv(builder_dir):
    return Path(builder_dir) / 'icons-venv'


def python(folder):
    return Path(folder) / 'bin/python'


def requirements():
    return f'fonttools=={VERSION} ' + ' '.join(f'--hash=sha256:{h}' for h in HASHES) + '\n'


def pip_argv(folder, requirements_file):
    return [python(folder), '-I', '-m', 'pip', 'install', '--isolated', '--disable-pip-version-check', '--no-input',
            '--index-url', INDEX, '--no-deps', '--only-binary=:all:', '--require-hashes', '-r', requirements_file]


def pip_env():
    """The environment without any PIP_* variable (they could point pip at another index)."""
    return {k: v for k, v in os.environ.items() if not k.startswith('PIP_')}


def _check(folder, cancel=None):
    try:
        code, out, _ = cancellable.run([python(folder), '-I', '-c', CHECK], cancel, CHECK_S)
    except cancellable.Stopped as stop:
        if stop.reason == 'cancelled':
            raise EnvError('cancelled') from None
        return False
    return code == 0 and out.decode(errors='replace').strip() == VERSION


def status(builder_dir, cancel=None):
    """'ready', 'missing' or 'broken' (there but not importing fontTools 4.67.0)."""
    folder = venv(builder_dir)
    if not folder.exists():
        return 'missing'
    return 'ready' if _check(folder, cancel) else 'broken'


def _step(argv, cancel, timeout_s, what, env=None):
    try:
        code, _, err = cancellable.run(argv, cancel, timeout_s, env)
    except cancellable.Stopped as stop:
        raise EnvError('cancelled' if stop.reason == 'cancelled' else what, stop.reason) from None
    if code != 0:
        raise EnvError(what, cancellable.last_line(err))


def install(builder_dir, cancel=None):
    """Makes the venv (after the player's yes); EnvError with the step that failed. Returns its folder."""
    target = venv(builder_dir)
    partial = target.with_name(target.name + '.partial')
    shutil.rmtree(partial, ignore_errors=True)
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        _step([sys.executable, '-I', '-m', 'venv', partial], cancel, VENV_S, 'venv')
        wanted = partial / 'requirements.txt'
        wanted.write_text(requirements())
        _step(pip_argv(partial, wanted), cancel, PIP_S, 'pip', pip_env())
        if target.exists() or target.is_symlink():
            shutil.rmtree(target)                   # a broken one, which the player agreed to make again
        os.replace(partial, target)
        if not _check(target, cancel):
            shutil.rmtree(target, ignore_errors=True)
            raise EnvError('check', f'fontTools {VERSION} does not import from {target}')
        return target
    finally:
        shutil.rmtree(partial, ignore_errors=True)

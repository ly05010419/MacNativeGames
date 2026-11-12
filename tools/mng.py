#!/usr/bin/env python3
"""mng: MNG Launcher's engine in a terminal, for a player or the AI helping them. The same jobs and rules as the window
(tools/builder_jobs; commands: tools/builder_jobs/text_cli.py), one build at a time on this Mac shared with the window,
which shows a build started here and can stop it.

  python3 tools/mng.py status
  python3 tools/mng.py build "<game file>" "<update>" --keys "<prod.keys>"
  python3 tools/mng.py update totk
  python3 tools/mng.py --help

Any python3 starts it (macOS's own is fine): it runs the jobs with MNG Launcher's own Python 3.14 when the Launcher is
made (tools/builder_app/make_app.py), else with a Python 3.14 from python.org or Homebrew."""
import os
from pathlib import Path
import shutil
import sys

REPO = Path(__file__).resolve().parents[1]
LAUNCHER_PYTHON = Path.home() / 'Applications/MNG Launcher.app/Contents/Resources/python/bin/python3.14'


def python():
    if os.access(str(LAUNCHER_PYTHON), os.X_OK):
        return str(LAUNCHER_PYTHON)
    if sys.version_info >= (3, 14):
        return sys.executable
    look = os.pathsep.join(['/usr/local/bin', '/opt/homebrew/bin', os.environ.get('PATH', '')])
    found = next((p for p in (shutil.which(n, path=look) for n in ('python3.14', 'python3.15')) if p), None)
    if not found:
        sys.exit('mng needs MNG Launcher (python3 tools/builder_app/make_app.py, which carries its own Python) or Python 3.14 '
                 'from https://www.python.org/downloads/macos/')
    return found


def main():
    env = dict(os.environ, PYTHONPATH=str(REPO), PYTHONDONTWRITEBYTECODE='1')
    if 'MNG_LANG' not in env:   # Chinese texts when the terminal's language is Chinese, as the window follows the Mac's
        env['MNG_LANG'] = 'zh' if env.get('LANG', '').lower().startswith('zh') else 'en'
    os.chdir(str(REPO))
    py = python()
    os.execve(py, [py, '-m', 'tools.builder_jobs.text_cli', *sys.argv[1:]], env)


if __name__ == '__main__':
    main()

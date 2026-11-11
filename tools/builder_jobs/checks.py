"""What this Mac still needs before a build, each with the command that fixes it (build_app.py prerequisites()).

Contract: checks(work_root) returns every check, passed or not, in the order a player fixes them; it never raises.
A check build_app.py makes is restated here so the window says it before the build; test_builder_jobs keeps the two
in step (it reads prerequisites() and fails when a tool named there is missing here)."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from tools.builder_jobs.text import t

BREW = Path('/opt/homebrew')
BREW_PACKAGES = ('opus', 'libvpx')      # build_app.py prerequisites(): linked into the runtime
FREE_GB = 60                            # warn below this; the measured sizes are in docs/builder-app.md


def _ok(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, timeout=60, **kw).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _free_gb(path):
    path = Path(path).expanduser()
    while not path.exists() and path != path.parent:
        path = path.parent
    try:
        return shutil.disk_usage(path).free / 2**30
    except OSError:
        return None


def checks(work_root):
    out = []
    add = lambda id, ok, text, fix=None, action=None: out.append(dict(id=id, ok=bool(ok), text=text, fix=fix, action=action))
    major = int((platform.mac_ver()[0] or '0').split('.')[0])
    add('mac', platform.machine() == 'arm64' and major >= 26,
        t(f'a Mac with Apple silicon and macOS 26 or later (this one: {platform.machine()}, macOS {platform.mac_ver()[0]})',
          f'Apple 芯片的 Mac，macOS 26 或更新（这台：{platform.machine()}，macOS {platform.mac_ver()[0]}）'))
    clang = _ok(['xcrun', '--find', 'clang'])
    add('clt', clang, t('Xcode command line tools', 'Xcode 命令行工具'), 'xcode-select --install', 'install-clt')
    cxx = clang and _ok(['xcrun', 'clang++', '-std=c++11', '-x', 'c++', '-fsyntax-only', '-'], input=b'#include <string>\n')
    add('cxx', cxx, t('C++ headers of the developer tools', '开发工具里的 C++ 头文件'),
        'sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install')
    brew = (BREW / 'bin/brew').is_file()
    add('brew', brew, t('Homebrew', 'Homebrew（软件包管理器）'), '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"')
    missing = [p for p in BREW_PACKAGES if not (BREW / 'opt' / p / 'lib').is_dir()]
    add('brew-packages', not missing, t('Homebrew packages ', 'Homebrew 软件包 ') + ' + '.join(BREW_PACKAGES),
        f'brew install {" ".join(missing or BREW_PACKAGES)}', 'brew-install' if brew else None)
    add('git', shutil.which('git', path=os.pathsep.join(['/opt/homebrew/bin', '/usr/bin'])), 'git', 'xcode-select --install')
    add('python', sys.version_info >= (3, 14), t(f'Python 3.14 (running {platform.python_version()})', f'Python 3.14（现在是 {platform.python_version()}）'), 'brew install python@3.14')
    free = _free_gb(work_root)
    add('disk', free is not None and free >= FREE_GB,
        t(f'{FREE_GB} GB free for the work folder ({"unknown" if free is None else f"{free:.0f} GB"} free at {work_root})',
          f'工作目录要 {FREE_GB} GB 空间（{work_root} 剩 {"未知" if free is None else f"{free:.0f} GB"}）'),
        t('choose a work folder on a disk with more space', '换一个空间更大的盘放工作目录'))
    return out

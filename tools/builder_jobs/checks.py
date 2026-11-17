"""What this Mac still needs before a build, each with the command that fixes it (build_app.py prerequisites()).

Contract: checks(work_root, game=None) returns every check, passed or not, in the order a player fixes them; it never
raises. `game` is the game the dropped files make (classify.report's plan, 'oot' and the like): a check only one game's
build needs is that game's alone (metal_compiler), and no developer tool is probed for a game that does not need it, so a
player whose game builds fine is never told this Mac is not. A check build_app.py makes is restated here so the window
says it before the build; test_builder_jobs keeps the two in step (it reads prerequisites() and fails when a tool named
there is missing here)."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

# No game's build takes a library or tool from Homebrew: opus, libvpx and dav1d are built from pinned sources by
# build_app.py (codec_libraries), and Ocarina of Time needs no llvm or ffmpeg since 2026-11-12.
FREE_GB = 60                            # warn below this; the measured sizes are in docs/builder-app.md

# The games whose build compiles the game's shaders with Apple's offline Metal compiler, which only an Xcode's optional
# Metal toolchain carries: games/oot/build.py runs games/oot/scripts/build_shader_libs.py, which stops without it. The
# games build_app.py builds go on without it (step_shader_libs puts the startup shaders into macOS's Metal cache at the
# end of the build), so there it is a speed-up and an app that can be moved, not a requirement.
METAL_GAMES = ('oot',)


def _ok(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, timeout=60, **kw).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _out(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ''


def _free_gb(path):
    path = Path(path).expanduser()
    while not path.exists() and path != path.parent:
        path = path.parent
    try:
        return shutil.disk_usage(path).free / 2**30
    except OSError:
        return None


def checks(work_root, game=None):
    out = []
    add = lambda id, ok, text, fix=None, action=None: out.append(dict(id=id, ok=bool(ok), text=text, fix=fix, action=action))
    major = int((platform.mac_ver()[0] or '0').split('.')[0])
    add('mac', platform.machine() == 'arm64' and major >= 26,
        t(f'a Mac with Apple silicon and macOS 26 or later (this one: {platform.machine()}, macOS {platform.mac_ver()[0]})',
          f'Apple 芯片的 Mac，macOS 26 或更新（这台：{platform.machine()}，macOS {platform.mac_ver()[0]}）'))
    if compile_free():   # build_app.compile_free: the Launcher's source has the release's prebuilt programs
        # a game it does not cover says so at its build (classify.PREBUILT_GAMES, tool_blocks)
        add('tools', True, t('developer tools: not needed for Zelda, Donkey Kong Bananza and Pokopia, this Launcher brings '
                             'the programs their build compiles (a game that needs them says so at the build)',
                             '开发工具：塞尔达、大金刚、Pokopia 不需要，这个 Launcher 自带打包要编的程序（需要的游戏在第三步会说明）'))
    else:
        developer_tools(add)
    metal_compiler(add, game)   # Ocarina of Time's alone, and needed with the prebuilt programs too
    add('python', sys.version_info >= (3, 14), t(f'Python 3.14 (running {platform.python_version()})', f'Python 3.14（现在是 {platform.python_version()}）'), t('install it from https://www.python.org/downloads/macos/', '从 https://www.python.org/downloads/macos/ 安装'))
    free = _free_gb(work_root)
    add('disk', free is not None and free >= FREE_GB,
        t(f'{FREE_GB} GB free for the work folder ({"unknown" if free is None else f"{free:.0f} GB"} free at {work_root})',
          f'工作目录要 {FREE_GB} GB 空间（{work_root} 剩 {"未知" if free is None else f"{free:.0f} GB"}）'),
        t('choose a work folder on a disk with more space', '换一个空间更大的盘放工作目录'))
    return out


def compile_free():
    """Every program a Mac build compiles is the release's (tools/prebuilt.py): no developer tools are checked, and
    none of xcrun, git or make is run, since on a Mac without them each opens Apple's install dialog."""
    from tools import prebuilt
    from tools.builder_jobs import repo
    return prebuilt.complete(repo.REPO)


def metal_compiler(add, game):
    """Ocarina of Time compiles its shader libraries with Apple's offline Metal compiler, which only an Xcode's optional
    Metal toolchain carries (games/oot/build.py runs games/oot/scripts/build_shader_libs.py, which stops without it);
    the games build_app.py builds put their startup shaders into macOS's Metal cache instead (step_shader_libs). Its own
    search (build_app.metal_compiler), so an Xcode in /Applications counts while xcode-select still points at the
    CommandLineTools. Only that game asks: for the others no xcrun runs, which on a Mac without the developer tools would
    open Apple's install dialog (compile_free), and the build's own block (classify.oot_needs_metal) says it too."""
    if game not in METAL_GAMES:
        return
    found = repo.build_app().metal_compiler()
    if found:
        text = t(f"Xcode's Metal toolchain, the offline shader compiler this game's build needs (this one: {found[1].split(' (')[0]})",
                 f'Xcode 的 Metal 工具链，这个游戏打包要用它编着色器（这台是：{found[1].split(" (")[0]}）')
    else:
        text = t("Xcode's Metal toolchain, which Ocarina of Time's build compiles the game's shaders with (the command line "
                 'tools do not include it)',
                 'Xcode 的 Metal 工具链，时之笛打包要用它编译游戏的着色器（命令行工具里没有）')
    add('metal', found is not None, text,
        t('xcodebuild -downloadComponent MetalToolchain, or Xcode > Settings > Components > Metal Toolchain once Xcode is '
          'installed from the App Store',
          'xcodebuild -downloadComponent MetalToolchain，或从 App Store 装好 Xcode 后在 Xcode > Settings > Components 里'
          '下载 Metal Toolchain'))


def developer_tools(add):
    """A source without the prebuilt programs (a checkout) compiles them: the command line tools and their SDK."""
    clang = _ok(['xcrun', '--find', 'clang'])
    add('clt', clang, t('Xcode command line tools', 'Xcode 命令行工具'), 'xcode-select --install', 'install-clt')
    sdk = _out(['xcrun', '--show-sdk-version']) if clang else ''
    head = sdk.split('.')[0]
    add('sdk', head.isdigit() and int(head) >= 26,
        t(f'the macOS 26 SDK or later, with Metal 4 (the developer tools use: {sdk or "?"})',
          f'macOS 26 或更新的 SDK，带 Metal 4（开发工具现在用的是：{sdk or "?"}）'),
        t('update Xcode from the App Store, or use the command line tools: sudo xcode-select -s /Library/Developer/CommandLineTools',
          '在 App Store 更新 Xcode，或改用命令行工具：sudo xcode-select -s /Library/Developer/CommandLineTools'))
    cxx = clang and _ok(['xcrun', 'clang++', '-std=c++11', '-x', 'c++', '-fsyntax-only', '-'], input=b'#include <string>\n')
    add('cxx', cxx, t('C++ headers of the developer tools', '开发工具里的 C++ 头文件'),
        'sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install')
    add('git', shutil.which('git', path=os.pathsep.join(['/opt/homebrew/bin', '/usr/bin'])), 'git', 'xcode-select --install')

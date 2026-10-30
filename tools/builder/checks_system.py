"""The Mac's system checks, in build_app.py's order of prerequisites (S1.2a). Each rule mirrors build_app.py's own
check, with these differences on purpose: apple_silicon asks the hardware (the Python that will run the build is checked
by build_python); homebrew is a check of its own, so its fix can be the install command; the effective developer folder
is validated before any xcrun; metal_toolchain does not search /Volumes or Spotlight, which can be slow, and its WARN
says build_app.py still looks there.

Runner outcomes: OK is read by each check. NONZERO has a meaning only where a check gives it one (sysctl,
xcode-select, clang++); elsewhere, and for NOT_FOUND, NOT_EXECUTABLE and TIMEOUT, the result is command_failed.
metal_toolchain moves on to its next candidate instead. No check catches exceptions: run_checks does (K4)."""
import os
from pathlib import Path
import shutil
import tempfile

from tools.builder.checks import CheckResult, Status
from tools.builder.ports import Outcome

BREW_INSTALL = '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
SELECTION_ARGV = ['/usr/bin/env', '-u', 'DEVELOPER_DIR', '/usr/bin/xcode-select', '-p']


def _run(ctx, argv):
    return ctx.runner.run(argv, timeout_s=ctx.command_timeout_s)


def _tail(text, limit=300):
    text = text.strip()
    return text if len(text) <= limit else '...' + text[-limit:]


def _failed(check_id, result):
    return CheckResult(check_id, Status.BLOCK, 'command_failed', f'{result.outcome.name}: {_tail(result.stderr)}')


def _ok(check_id, detail=''):
    return CheckResult(check_id, Status.OK, 'ok', detail)


def _valid_developer_dir(path):
    return path is not None and (path / 'usr/bin').is_dir()


def developer_dirs(ctx):
    """(override or None, selection or None, the xcode-select result). The selection is read without the
    override, because xcode-select -p prints DEVELOPER_DIR even when that path does not exist."""
    override = ctx.environ.get('DEVELOPER_DIR') or None
    result = _run(ctx, SELECTION_ARGV)
    selection = result.stdout.strip() if result.outcome is Outcome.OK else ''
    return (Path(override) if override else None), (Path(selection) if selection else None), result


class AppleSilicon:
    id, requires = 'apple_silicon', ()

    def run(self, ctx):
        r = _run(ctx, ['/usr/sbin/sysctl', '-n', 'hw.optional.arm64'])
        if r.outcome is Outcome.OK and r.stdout.strip() == '1':
            return _ok(self.id, '1')
        if r.outcome in (Outcome.OK, Outcome.NONZERO):      # 0, or no such key: an Intel Mac
            return CheckResult(self.id, Status.BLOCK, 'intel_mac', _tail(r.stdout or r.stderr))
        return _failed(self.id, r)


class MacosVersion:
    id, requires = 'macos_version', ()

    def run(self, ctx):
        version = ctx.mac_version()
        try:
            major = int(version.split('.')[0])
        except ValueError:
            return CheckResult(self.id, Status.BLOCK, 'macos_unknown', repr(version))
        if major < 26:
            return CheckResult(self.id, Status.BLOCK, 'macos_too_old', version)
        return _ok(self.id, version)


class CommandLineTools:
    id, requires = 'command_line_tools', ()

    def run(self, ctx):
        override, selection, r = developer_dirs(ctx)
        if override:
            if _valid_developer_dir(override):
                return _ok(self.id, f'{override} (from DEVELOPER_DIR)')
            return CheckResult(self.id, Status.BLOCK, 'developer_dir_override_invalid',
                               f'DEVELOPER_DIR={override}; xcode-select: {selection or r.outcome.name}',
                               'unset DEVELOPER_DIR')
        if selection is None:
            if r.outcome in (Outcome.OK, Outcome.NONZERO):   # nothing selected (exit 2) or an empty answer
                return CheckResult(self.id, Status.BLOCK, 'command_line_tools_missing', _tail(r.stderr),
                                   'xcode-select --install')
            return _failed(self.id, r)
        if not _valid_developer_dir(selection):
            return CheckResult(self.id, Status.BLOCK, 'developer_dir_selection_invalid', str(selection),
                               'sudo xcode-select --reset')
        return _ok(self.id, f'{selection} (from xcode-select)')


class CxxHeaders:
    id, requires = 'cxx_headers', ('command_line_tools',)

    def run(self, ctx):
        override, selection, _ = developer_dirs(ctx)
        with tempfile.TemporaryDirectory() as tmp:      # removed even when the runner raises
            probe = Path(tmp) / 'probe.cpp'
            probe.write_text('#include <string>\n')
            r = _run(ctx, ['/usr/bin/xcrun', 'clang++', '-std=c++11', '-fsyntax-only', str(probe)])
        if r.outcome is Outcome.OK:
            return _ok(self.id)
        if r.outcome is Outcome.NONZERO:
            return CheckResult(self.id, Status.BLOCK, 'cxx_headers_needs_diagnosis',
                               f'{override or selection}: {_tail(r.stderr)}')
        return _failed(self.id, r)


class Homebrew:
    id, requires = 'homebrew', ()

    def run(self, ctx):
        brew = ctx.brew_prefix / 'bin/brew'
        if brew.exists():
            return _ok(self.id, str(brew))
        return CheckResult(self.id, Status.BLOCK, 'homebrew_missing', str(brew), BREW_INSTALL)


class BrewPackages:
    id, requires = 'brew_packages', ('homebrew',)
    PACKAGES = ('opus', 'libvpx')

    def run(self, ctx):
        missing = [p for p in self.PACKAGES if not (ctx.brew_prefix / 'opt' / p / 'lib').is_dir()]
        if missing:
            return CheckResult(self.id, Status.BLOCK, 'brew_packages_missing', ' '.join(missing),
                               'brew install ' + ' '.join(missing))
        return _ok(self.id, ' '.join(self.PACKAGES))


class Git:
    id, requires = 'git', ()

    def run(self, ctx):
        git = shutil.which('git', path=os.pathsep.join([*ctx.tool_paths, '/usr/bin']))
        if git:
            return _ok(self.id, git)
        return CheckResult(self.id, Status.BLOCK, 'git_missing', '', 'xcode-select --install')


class BuildPython:
    id, requires = 'build_python', ()

    def run(self, ctx):
        path = os.pathsep.join(ctx.tool_paths)
        python = next((p for p in (shutil.which(n, path=path) for n in ('python3.14', 'python3.15')) if p), None)
        if not python:
            return CheckResult(self.id, Status.BLOCK, 'build_python_missing', path, 'brew install python@3.14')
        r = _run(ctx, [python, '-c', 'import platform; print(platform.machine())'])
        if r.outcome is not Outcome.OK:
            return _failed(self.id, r)
        machine = r.stdout.strip()
        if machine != 'arm64':
            return CheckResult(self.id, Status.BLOCK, 'python_not_arm64', f'{python}: {machine}')
        return _ok(self.id, python)


class MetalToolchain:
    id, requires = 'metal_toolchain', ('command_line_tools',)

    def run(self, ctx):
        tried = []
        for folder in self.candidates(ctx):
            tried.append(str(folder))
            env = ['/usr/bin/env', f'DEVELOPER_DIR={folder}']
            found = _run(ctx, env + ['/usr/bin/xcrun', '--find', 'metal'])
            if found.outcome is not Outcome.OK or not found.stdout.strip():
                continue
            version = _run(ctx, env + [found.stdout.strip(), '--version'])
            if version.outcome is Outcome.OK and version.stdout.strip():
                return _ok(self.id, f'{folder}: {version.stdout.strip().splitlines()[0]}')
        return CheckResult(self.id, Status.WARN, 'metal_toolchain_not_found',
                           f'tried {", ".join(tried) or "nothing"}; build_app.py also searches /Volumes and Spotlight',
                           'xcodebuild -downloadComponent MetalToolchain')

    @staticmethod
    def candidates(ctx):
        """DEVELOPER_DIR, the selection, then Xcode*.app in each applications folder: build_app.py's
        developer_dirs without /Volumes and Spotlight. Duplicates are asked once."""
        override, selection, _ = developer_dirs(ctx)
        found = [override, selection]
        for apps in ctx.applications_dirs:
            found += [app / 'Contents/Developer' for app in sorted(Path(apps).glob('Xcode*.app'))]
        return list(dict.fromkeys(f for f in found if f))


SYSTEM_CHECKS = (AppleSilicon(), MacosVersion(), CommandLineTools(), CxxHeaders(), Homebrew(), BrewPackages(),
                 Git(), BuildPython(), MetalToolchain())

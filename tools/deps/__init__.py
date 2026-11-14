"""Every build dependency a player would otherwise install from Homebrew, made from pinned sources once per Mac into
~/Library/Caches/MacNativeGames/deps (MNG_DEPS_CACHE moves it), shared by build_app.py and every games/<name> build.

    from tools.deps import prefix, program, env, path
    prefix('opus')               # Path of an installed static library (lib/, include/), made the first time
    program('xcodegen')          # Path of a program: zstd, spirv-cross, glslangValidator, xcodegen, dotnet, meson, ninja, cmake
    env('opus', 'libvpx')        # {'OPUS_PREFIX': …, 'VPX_PREFIX': …} for a make (Makefiles take these with ?=)
    path('cmake', 'xcodegen')    # the folders holding those programs, to put first on PATH

From a Makefile or a shell: python3 tools/deps prefix opus (see __main__.py). Versions and digests: pins.py.
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

from .core import DepsError, cache, fetch, git_fetch, install, jobs, locked, run, say, unpack
from .pins import BUILD_TOOLS, SOURCES

# A build copies a game's sources into its work folder and runs scripts there; they find this repository (and so
# tools/deps) through MNG_REPO, which every process that imported tools/deps passes on to the ones it starts.
os.environ.setdefault('MNG_REPO', str(Path(__file__).resolve().parents[2]))

__all__ = ['DepsError', 'cache', 'env', 'path', 'prefix', 'program', 'build_tools', 'python_environment', 'LIBRARIES', 'PROGRAMS']

# library or tool name: (folder under the cache, the file whose presence means it is installed)
def _folder(name):
    version, url, pin = SOURCES[name]
    commit = f'-{pin[:12]}' if url.endswith('.git') else ''
    return {'ogg': 'libogg', 'vorbis': 'libvorbis', 'dotnet': 'dotnet-sdk'}.get(name, name) + f'-{version}{commit}'


MARKER = {'opus': 'lib/libopus.a', 'libvpx': 'lib/libvpx.a', 'dav1d': 'lib/libdav1d.a', 'ogg': 'lib/libogg.a',
          'vorbis': 'lib/libvorbis.a', 'zstd': 'bin/zstd', 'spirv-cross': 'bin/spirv-cross',
          'glslang': 'bin/glslangValidator', 'xcodegen': 'bin/xcodegen', 'dotnet': 'dotnet'}
LIBRARIES = {'opus': 'OPUS_PREFIX', 'libvpx': 'VPX_PREFIX', 'dav1d': 'DAV1D_PREFIX', 'ogg': 'OGG_PREFIX',
             'vorbis': 'VORBIS_PREFIX', 'dotnet': 'DOTNET_ROOT'}
PROGRAMS = {'zstd': ('zstd', 'bin/zstd'), 'spirv-cross': ('spirv-cross', 'bin/spirv-cross'),
            'glslangValidator': ('glslang', 'bin/glslangValidator'), 'xcodegen': ('xcodegen', 'bin/xcodegen'),
            'dotnet': ('dotnet', 'dotnet')}
# glslang is built with cmake and ninja; the set with meson too is the one iPhone / iPad builds already make.
CMAKE_SET = ('cmake', 'meson', 'ninja')


def package(name):
    """A pinned package consumed by an isolated build's offline dependency feed."""
    from .pins import PACKAGES
    from .core import fetch
    filename, url, digest = PACKAGES[name]
    with locked():
        return fetch(url, digest, filename)


def python_environment(name):
    """An isolated CPython 3.14 tool environment from checked, locally installed wheels."""
    import platform
    from .pins import PYTHON_ENVIRONMENTS
    from .core import fetch
    if name not in PYTHON_ENVIRONMENTS:
        raise DepsError(f'unknown Python tool environment: {name}')
    if sys.platform != 'darwin' or platform.machine() != 'arm64':
        raise DepsError(f'{name} requires an Apple silicon Mac')
    wheels = PYTHON_ENVIRONMENTS[name]
    identity = hashlib.sha256(repr(wheels).encode()).hexdigest()[:16]
    folder = cache() / f'python-{name}-{identity}'
    python, done = folder/'bin/python', folder/'.done'
    if done.is_file() and python.is_file():
        return python
    with locked():
        if done.is_file() and python.is_file():
            return python
        host = sys.executable if sys.version_info[:2] == (3, 14) else shutil.which('python3.14', path=os.pathsep.join([
            '/usr/local/bin', os.environ.get('PATH', '')]))
        if not host:
            raise DepsError('Python 3.14 is required: https://www.python.org/downloads/macos/')
        archives = [fetch(url, digest, filename) for filename, url, digest in wheels]
        if folder.exists():
            shutil.rmtree(folder)
        run([host, '-m', 'venv', folder], f'{name}-venv.log')
        run([python, '-m', 'pip', 'install', '--no-index', '--no-deps', '--disable-pip-version-check', *archives], f'{name}-pip.log')
        done.touch()
    return python


def prefix(name):
    """The installed folder of a pinned dependency (SOURCES), made from its source the first time."""
    if name not in SOURCES:
        raise DepsError(f'unknown dependency {name!r}: one of {", ".join(sorted(SOURCES))}')
    folder = cache() / _folder(name)
    if (folder / MARKER[name]).exists():
        return folder
    with locked():
        if not (folder / MARKER[name]).exists():
            version = SOURCES[name][0]
            say(f'making {name} {version} from its pinned source, once for this Mac (log: {cache() / "logs"})')
            install(folder, lambda stage: RECIPES[name](stage))
    return folder


def program(name):
    """The path of a pinned program."""
    if name in BUILD_TOOLS:
        return build_tools(CMAKE_SET if name == 'cmake' else ('meson', 'ninja')) / name
    if name not in PROGRAMS:
        raise DepsError(f'unknown program {name!r}: one of {", ".join(sorted([*PROGRAMS, *BUILD_TOOLS]))}')
    source, file = PROGRAMS[name]
    return prefix(source) / file


def env(*names):
    """Environment variables naming the installed dependencies (OPUS_PREFIX, VPX_PREFIX, …; DOTNET_ROOT for dotnet)."""
    return {LIBRARIES[name]: str(prefix(name)) for name in names}


def path(*programs):
    """The folders of these programs, in order and without repeats, to put first on PATH."""
    out = []
    for name in programs:
        folder = str(program(name).parent)
        if folder not in out:
            out.append(folder)
    return out


def build_tools(names):
    """A folder holding only the named PyPI tools (links into their virtual environment, no python), so putting it first
    on PATH changes nothing else. The name is a hash of the pins asked for (byte-identical to what build_app.py used:
    build-tools-7678300352e8 for meson + ninja); the environment is made where it stays, .done marks it complete."""
    pins = {name: BUILD_TOOLS[name] for name in sorted(names)}
    venv = cache() / f'build-tools-{hashlib.sha256(repr(pins).encode()).hexdigest()[:12]}'
    tools, done = venv / 'tools', venv / '.done'
    if done.is_file():
        return tools
    with locked():
        if done.is_file():
            return tools
        say(f'installing {", ".join(f"{n} {v}" for n, (v, _) in pins.items())} from pinned wheels, once for this Mac')
        if venv.exists():
            shutil.rmtree(venv)
        run([venv_python(), '-m', 'venv', venv], 'build-tools-venv.log')
        wanted = venv / 'requirements.txt'
        wanted.write_text(''.join(f'{n}=={v} --hash=sha256:{h}\n' for n, (v, h) in pins.items()))
        run([venv / 'bin/python', '-m', 'pip', 'install', '--disable-pip-version-check', '--no-input', '--require-hashes',
             '--only-binary=:all:', '--no-deps', '-r', wanted], 'build-tools-pip.log')
        tools.mkdir()
        for name in pins:
            (tools / name).symlink_to(venv / 'bin' / name)
        done.touch()
    return tools


def venv_python():
    """The Python the build tools' environment is made with: this one when it is 3.10 or later, else the player's 3.14
    (meson 1.12 needs 3.10; a Makefile may run this module with macOS's /usr/bin/python3, which is 3.9)."""
    if sys.version_info >= (3, 10):
        return sys.executable
    look = os.pathsep.join(['/usr/local/bin', '/opt/homebrew/bin', os.environ.get('PATH', '')])
    found = next((p for p in (shutil.which(n, path=look) for n in ('python3.14', 'python3.15')) if p), None)
    if not found:
        raise DepsError('Python 3.14 is needed to install the build tools: https://www.python.org/downloads/macos/')
    return found


# ---- recipes: each installs into stage (install() renames it to the prefix when it finished) ----
def _source(name):
    """A fresh unpacked copy of a release archive: the folder it holds (one top folder) or the unpacked folder."""
    version, url, want = SOURCES[name]
    archive = fetch(url, want, url.rsplit('/', 1)[1] if name != 'xcodegen' else f'xcodegen-{version}.zip')
    folder = unpack(archive, cache() / 'build' / _folder(name))
    inside = [p for p in folder.iterdir() if not p.name.startswith('.')]
    return inside[0] if len(inside) == 1 and inside[0].is_dir() else folder


def _checkout(name):
    version, url, commit = SOURCES[name]
    return git_fetch(url, commit, cache() / 'src' / _folder(name))


def _autotools(name, stage, *options, env=None):
    src = _source(name)
    run(['./configure', '--disable-dependency-tracking', '--disable-shared', '--enable-static', f'--prefix={stage}',
         'CC=clang', *options], f'{name}-configure.log', cwd=src, env=env)
    run(['make', f'-j{jobs()}', 'install'], f'{name}-make.log', cwd=src, env=env)


def _opus(stage):
    _autotools('opus', stage, '--disable-doc', '--disable-extra-programs')


def _libvpx(stage):
    src = _checkout('libvpx')
    build = cache() / 'build' / 'libvpx'
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)
    run([src / 'configure', '--disable-dependency-tracking', '--disable-examples', '--disable-tools', '--disable-docs',
         '--disable-unit-tests', '--enable-pic', '--disable-shared', '--enable-static', '--enable-vp9-highbitdepth',
         f'--prefix={stage}'], 'libvpx-configure.log', cwd=build)
    run(['make', f'-j{jobs()}', 'install'], 'libvpx-make.log', cwd=build)


def _dav1d(stage):
    """A static library with its arm64 assembly, the options Homebrew's build used (release, no tools or tests)."""
    src = _source('dav1d')
    tools = build_tools(('meson', 'ninja'))
    build = cache() / 'build' / 'dav1d-build'
    if build.exists():
        shutil.rmtree(build)
    env = dict(os.environ, PATH=f'{tools}{os.pathsep}{os.environ.get("PATH", "")}', CC='clang')
    run([tools / 'meson', 'setup', build, src, f'--prefix={stage}', '--libdir=lib', '--buildtype=release',
         '--default-library=static', '-Db_ndebug=true', '-Denable_tools=false', '-Denable_tests=false',
         '-Denable_examples=false', '-Denable_docs=false'], 'dav1d-meson.log', env=env)
    run([tools / 'ninja', '-C', build, 'install'], 'dav1d-ninja.log', env=env)


def _ogg(stage):
    _autotools('ogg', stage)


def _vorbis(stage):
    ogg = prefix('ogg')
    src = _source('vorbis')
    # libvorbis 1.3.7's configure passes -force_cpusubtype_ALL on Darwin, which clang refuses (Homebrew removes it too)
    configure = src / 'configure'
    configure.write_text(configure.read_text().replace(' -force_cpusubtype_ALL', ''))
    run(['./configure', '--disable-dependency-tracking', '--disable-shared', '--enable-static', f'--prefix={stage}',
         f'--with-ogg={ogg}', '--disable-oggtest', 'CC=clang'], 'vorbis-configure.log', cwd=src)
    run(['make', f'-j{jobs()}', 'install'], 'vorbis-make.log', cwd=src)


def _zstd(stage):
    src = _source('zstd')
    run(['make', f'-j{jobs()}', '-C', 'programs', 'zstd-release'], 'zstd-make.log', cwd=src)
    (stage / 'bin').mkdir(parents=True)
    shutil.copy2(src / 'programs/zstd', stage / 'bin/zstd')


def _cmake_install(name, src, stage, *options, extra_env=None):
    """A CMake project built with the pinned cmake and ninja, installed into stage."""
    tools = build_tools(CMAKE_SET)
    build = cache() / 'build' / f'{name}-build'
    if build.exists():
        shutil.rmtree(build)
    env = dict(os.environ, PATH=f'{tools}{os.pathsep}{os.environ.get("PATH", "")}', **(extra_env or {}))
    run([tools / 'cmake', '-S', src, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={stage}',
         '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++', *options], f'{name}-cmake.log', env=env)
    run([tools / 'cmake', '--build', build, '--parallel', jobs()], f'{name}-build.log', env=env)
    run([tools / 'cmake', '--install', build], f'{name}-install.log', env=env)


def _spirv_cross(stage):
    """With CMake, which writes the commit (git describe) into --revision: nxnative keys its shader cache on that line."""
    src = _checkout('spirv-cross')
    # Its "Timestamp:" is the build time unless SOURCE_DATE_EPOCH is set: the commit's own time keeps the line the same
    # on every Mac and every rebuild, so the shader cache keyed on it is not thrown away for nothing.
    when = subprocess.run(['git', '-C', src, 'log', '-1', '--format=%ct'], capture_output=True, text=True, check=True).stdout.strip()
    _cmake_install('spirv-cross', src, stage, '-DSPIRV_CROSS_CLI=ON', '-DSPIRV_CROSS_STATIC=ON',
                   '-DSPIRV_CROSS_SHARED=OFF', '-DSPIRV_CROSS_ENABLE_TESTS=OFF', extra_env={'SOURCE_DATE_EPOCH': when})


def _glslang(stage):
    _cmake_install('glslang', _checkout('glslang'), stage, '-DENABLE_OPT=OFF', '-DGLSLANG_TESTS=OFF', '-DBUILD_SHARED_LIBS=OFF',
                   '-DENABLE_GLSLANG_BINARIES=ON', f'-DPython3_EXECUTABLE={venv_python()}')


def _xcodegen(stage):
    src = _source('xcodegen')                     # the zip holds xcodegen/{bin,share,LICENSE}
    if not (src / 'bin/xcodegen').is_file():
        raise DepsError(f'{src}: no bin/xcodegen in the XcodeGen release zip')
    shutil.copytree(src, stage, symlinks=True)


def _dotnet(stage):
    src = _source('dotnet')
    if not (src / 'dotnet').is_file():
        raise DepsError(f'{src}: no dotnet in the .NET SDK tarball')
    shutil.copytree(src, stage, symlinks=True)


RECIPES = {'opus': _opus, 'libvpx': _libvpx, 'dav1d': _dav1d, 'ogg': _ogg, 'vorbis': _vorbis, 'zstd': _zstd,
           'spirv-cross': _spirv_cross, 'glslang': _glslang, 'xcodegen': _xcodegen, 'dotnet': _dotnet}

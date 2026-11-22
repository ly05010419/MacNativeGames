"""python3 tools/deps <command> …, for Makefiles and shell scripts (Python 3.9 or later).

    prefix NAME           the installed folder of a library or tool (opus, libvpx, dav1d, ogg, vorbis, zstd, …)
    program NAME          the path of a program (zstd, spirv-cross, glslangValidator, xcodegen, dotnet, meson, ninja, cmake)
    python NAME           isolated tool Python (ui-pack), with pinned, verified wheels
    fonts                 the folder of the six open-source shared fonts (open_fonts.py), made the first time
    env NAME…             KEY=VALUE lines (OPUS_PREFIX=…, DOTNET_ROOT=…)
    path PROGRAM…         the folders of those programs joined with ':' (to put first on PATH)
    all                   make everything in pins.py (to fill a cache ahead of time)

What is made the first time is reported on stderr; stdout holds only the answer. Exit 1 with the reason on failure.
"""
import sys
from pathlib import Path

if __package__ in (None, ''):                      # run as a folder (python3 tools/deps): import it as tools.deps
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    __package__ = 'tools.deps'

from tools.deps import BUILD_TOOLS, PROGRAMS, DepsError, env, path, prefix, program, python_environment  # noqa: E402
from tools.deps.pins import SOURCES  # noqa: E402


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    command, names = argv[0], argv[1:]
    try:
        if command == 'prefix' and len(names) == 1:
            print(prefix(names[0]))
        elif command == 'program' and len(names) == 1:
            print(program(names[0]))
        elif command == 'python' and len(names) == 1:
            print(python_environment(names[0]))
        elif command == 'fonts' and not names:
            from tools.deps.open_fonts import open_fonts
            print(open_fonts())
        elif command == 'env' and names:
            for key, value in env(*names).items():
                print(f'{key}={value}')
        elif command == 'path' and names:
            print(':'.join(path(*names)))
        elif command == 'all' and not names:
            from tools.deps.offline import NOT_CARRIED, store
            for name in SOURCES:
                if name in NOT_CARRIED and store() is not None:   # no game a released Launcher builds uses it
                    continue
                print(f'{name} {prefix(name)}')
            for name in BUILD_TOOLS:
                print(f'{name} {program(name)}')
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except DepsError as e:
        print(f'[deps] {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

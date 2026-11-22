"""What MNG Launcher carries of this repository (its Contents/Resources/source.tar.gz, MNG-Launcher make_app.py
source_archive): the code its jobs run and the folders of the games it builds, nothing else (user 2026-11-22: "123 做").
Until then it carried the whole repository, 494 MB unpacked: some 260 MB of it were games the Launcher does not build, and
37.8 MB an iPhone / iPad library, which a Mac Launcher never makes.

    python3 tools/launcher_source.py pathspecs <commit>   git pathspecs of what goes in, one per line (git archive)

Left out: every games/<folder> no game of tools/builder_jobs/repo.py GAMES reads (repo.GAME_FOLDERS), the iPhone / iPad
parts of the ones kept (ios/, prebuilt/ios-arm64/), the tests and the Windows ports. tests/test_launcher_source.py checks
that nothing kept reads what is left out. Python 3.9 safe."""
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LEFT_OUT = ('tests', 'windows_games')   # top-level folders no job of the Launcher reads
IOS = ('games/*/ios/**', 'games/*/prebuilt/ios-arm64/**')   # a Mac Launcher builds Mac apps only


def kept_games():
    """The games/ folders the Launcher's games read."""
    sys.path.insert(0, str(REPO))
    from tools.builder_jobs.repo import GAME_FOLDERS, GAMES
    return sorted({folder for game in GAMES for folder in GAME_FOLDERS[game]})


def game_folders(commit='HEAD'):
    out = subprocess.run(['git', '-C', str(REPO), 'ls-tree', '--name-only', f'{commit}:games'], capture_output=True,
                         text=True, check=True).stdout
    return sorted(out.split())


def left_out_games(commit='HEAD'):
    kept = set(kept_games())
    return [name for name in game_folders(commit) if name not in kept]


def pathspecs(commit='HEAD'):
    """For git archive <commit> -- <pathspecs>: everything but what is left out."""
    return (['.'] + [f':(exclude)games/{name}' for name in left_out_games(commit)]
            + [f':(exclude){name}' for name in LEFT_OUT] + [f':(exclude,glob){pattern}' for pattern in IOS])


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'pathspecs':
        print('\n'.join(pathspecs(sys.argv[2])))
    else:
        sys.exit(__doc__)

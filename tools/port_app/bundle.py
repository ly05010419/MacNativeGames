"""The packaging plumbing of the ports MNG Launcher builds through their own build.py (tools/port_app/__init__.py): Super
Mario Odyssey (scripts/package_native_app.py), Animal Crossing (scripts/package_native_app.py) and Echoes of Wisdom
(scripts/package_app.py). Each packager keeps what its app carries, its checks of it and its Info.plist values; these
are the steps every one of them takes the same way.

The signing and the update keys are the ones build_app.py gives the Zelda games (sign_app, record_update_source);
tests/test_port_app.py checks both say the same. build_app.py is not imported here: it would bring games/totk's scripts
into these ports' packaging."""
from contextlib import contextmanager
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
LSREGISTER = Path('/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister')
CLANG = ('xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror')


def clone_file(source, target):
    """An APFS copy-on-write clone of one file (no extra space; a link is followed), a plain copy where cp -c cannot."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['/bin/cp', '-c', str(source), str(target)], capture_output=True).returncode != 0:
        shutil.copy2(source, target)


def clone_tree(source, target):
    """A file or a whole folder as APFS clones (cp -cRp: links inside stay links; on another volume cp copies)."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['/bin/cp', '-cRp', str(source), str(target)], check=True)


def compile_launcher(source, output, *, defines=(), frameworks=()):
    """The app launcher (its C file includes shared/app/app_launcher.h through a link beside it), warnings as errors."""
    subprocess.run([*CLANG, *(f'-D{d}' for d in defines), str(source), *(a for f in frameworks for a in ('-framework', f)),
                    '-o', str(output)], check=True)


def icon(source, resources, name):
    """The app icon as Contents/Resources/<name>.icns: an .icns cloned, any other image converted (sips). The name for
    CFBundleIconFile, or None when there is no source."""
    if not source or not Path(source).is_file():
        return None
    target = Path(resources) / f'{name}.icns'
    if Path(source).suffix.lower() == '.icns':
        clone_file(source, target)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['sips', '-s', 'format', 'icns', str(source), '--out', str(target)], check=True,
                       stdout=subprocess.DEVNULL)
    return name


def bundle_info(*, executable, bundle_id, name, display_name, version, build, minimum_system, icon_file=None):
    """The Info.plist keys every port's app has."""
    info = {
        'CFBundleDevelopmentRegion': 'en', 'CFBundleDisplayName': display_name, 'CFBundleExecutable': executable,
        'CFBundleIdentifier': bundle_id, 'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleName': name,
        'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': version, 'CFBundleVersion': build,
        'LSApplicationCategoryType': 'public.app-category.games', 'LSMinimumSystemVersion': minimum_system,
        'NSHighResolutionCapable': True,
    }
    return {**info, 'CFBundleIconFile': icon_file} if icon_file else info


def deployment_target():
    """The macOS the app targets: MACOSX_DEPLOYMENT_TARGET of the build, else 26.0."""
    return os.environ.get('MACOSX_DEPLOYMENT_TARGET', '26.0')


def source_commit(repo=REPO):
    """The checkout's commit (MNGSourceCommit: MNG Launcher's cards show it), None outside a Git checkout."""
    done = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else None


def update_record(work, repo=REPO, python=None):
    """What made the app, for MNG Launcher's update (update_runtime.py reads them; build_app.record_update_source writes
    the same keys): this checkout, the work folder and the build's Python."""
    return {'MNGSourceRepo': str(repo), 'MNGWorkFolder': str(work), 'MNGBuildPython': python or sys.executable}


def write_info(app, info):
    (Path(app) / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))


def sign(app, bundle_id, *, deep):
    """Ad hoc, with the bundle ID as the designated requirement (build_app.sign_app): macOS keeps the player's answer to
    "files on a removable volume" by that requirement; the ad hoc default (this build's code hash) asked again after
    every build. deep: nested code signed too (Animal Crossing and Echoes of Wisdom did; Odyssey's app never was)."""
    done = subprocess.run(['codesign', '--force', *(['--deep'] if deep else []), '--sign', '-', f'-r=designated => identifier "{bundle_id}"', str(app)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if done.returncode:
        raise subprocess.CalledProcessError(done.returncode, done.args, stderr=done.stderr)


def verify_signature(app):
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)


def check_launcher(executable):
    """The launcher's own check of the app it is in (--check: the game is not started)."""
    subprocess.run([str(executable), '--check'], check=True)


@contextmanager
def staged(output):
    """The app assembled as <output>.partial beside it (MNG Launcher never lists a .partial), which replaces the output
    only when the block ends without an error; on an error it is removed and the app there before stays."""
    output = Path(output)
    stage = output.with_name(output.name + '.partial')
    if stage.exists():
        shutil.rmtree(stage)
    try:
        yield stage
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    if output.exists():
        shutil.rmtree(output)
    stage.rename(output)


def register(app):
    """Finder and the Dock keep the icon they cached for this path (reported in play, PR #1305: the app showed the old,
    blank icon in Finder while the running app's was right): a new modification date and Launch Services registration
    make them read the new bundle's."""
    os.utime(app)
    if LSREGISTER.is_file():
        subprocess.run([str(LSREGISTER), '-f', str(app)], check=False)

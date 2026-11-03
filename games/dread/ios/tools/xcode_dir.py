"""Find the Xcode that builds the iOS app (its Developer folder, for DEVELOPER_DIR).

Order: DEVELOPER_DIR if set (it must be an Xcode with the iOS SDK), the Xcode `xcode-select` points at, Xcode*.app in
/Applications, then any Xcode Spotlight knows about (one kept outside /Applications, for example next to the project).
"""
import os
import subprocess
from pathlib import Path


def has_ios_sdk(developer):
    return (Path(developer) / 'Platforms/iPhoneOS.platform').is_dir()


def developer_dir():
    """The Developer folder of an Xcode with the iOS SDK; raises with what to do when there is none."""
    given = os.environ.get('DEVELOPER_DIR')
    if given:
        if has_ios_sdk(given): return given
        raise RuntimeError(f'DEVELOPER_DIR={given} has no iOS SDK: point it at <Xcode.app>/Contents/Developer')
    candidates = []
    try: candidates.append(subprocess.check_output(['xcode-select', '-p'], text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError): pass
    apps = sorted(Path('/Applications').glob('Xcode*.app'), key=lambda p: (p.name != 'Xcode.app', p.name))
    candidates += [str(p / 'Contents/Developer') for p in apps]
    try:
        found = subprocess.check_output(['mdfind', "kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'"], text=True,
                                        stderr=subprocess.DEVNULL)
        candidates += [line + '/Contents/Developer' for line in found.splitlines() if line.endswith('.app')]
    except (OSError, subprocess.CalledProcessError): pass
    for candidate in candidates:
        if candidate and has_ios_sdk(candidate): return candidate
    raise RuntimeError('no Xcode with the iOS SDK found: install Xcode, or set DEVELOPER_DIR=<Xcode.app>/Contents/Developer')

"""The game apps this Mac already has, for MNG Launcher's list (user 2026-11-12: "目录规范要和 launcher 统一 这样用launcher的
时候 就可以极速打包了"): every Mac app of a game this repository builds that macOS knows (LaunchServices) and that is still
there, plus those in the Launcher's apps folder, each with the update it needs (update_runtime.py --plan): an installed
game is updated in place, with the work folder it records, by the fastest update that fits.

Contract: apps(apps_dir) returns a list of dicts {app, name, game, version, kind, reason, work, commit}; it never raises.
kind is express, fast, full or error (update_runtime.plan)."""
import json
import os
import plistlib
import re
import subprocess
import sys
from pathlib import Path

from tools.builder_jobs import repo

LSREGISTER = ('/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister')
OOT_BUNDLE = 'org.macnativegames.oot'   # games/oot/build.py


def bundle_ids():
    """The Mac bundle IDs of the games (build_app.APPS, and Ocarina of Time's)."""
    return {bundle for _, bundle, _ in repo.build_app().APPS.values()} | {OOT_BUNDLE}


def registered(ids):
    """Paths LaunchServices has for these bundle IDs (lsregister -dump), as it lists them."""
    try:
        dump = subprocess.run([LSREGISTER, '-dump'], capture_output=True, text=True, errors='replace', timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    found = []
    for block in re.split(r'\n-{10,}\n', dump):
        ident = re.search(r'^identifier:\s+(\S+)', block, re.M)
        path = re.search(r'^path:\s+(.+?)(?: \(0x[0-9a-f]+\))?$', block, re.M)
        if ident and path and ident.group(1) in ids:
            found.append(path.group(1))
    return found


def usable(path):
    """An installed Mac app, not a half-made or replaced copy (build_app.install_app's .partial / .replaced-N) or one in
    the Trash."""
    p = Path(path)
    return (p.suffix == '.app' and '.partial' not in p.name and '.replaced-' not in p.name and '/.Trash' not in str(p)
            and (p / 'Contents/Info.plist').is_file())


def plan(app, python=sys.executable, work=None):
    """update_runtime.py --plan for one app (with --work when given), or an 'error' plan saying why it could not be asked."""
    try:
        done = subprocess.run([python, str(repo.UPDATE_RUNTIME), str(app), '--plan'] + (['--work', str(work)] if work else []),
                              cwd=repo.REPO, capture_output=True,
                              text=True, timeout=300)
        return json.loads(done.stdout.strip().splitlines()[-1])
    except (OSError, subprocess.TimeoutExpired, ValueError, IndexError) as e:
        return {'app': str(app), 'kind': 'error', 'reason': f'update_runtime.py --plan failed: {e}'}


def apps(apps_dir=None, python=sys.executable):
    ids = bundle_ids()
    candidates = registered(ids)
    if apps_dir and Path(apps_dir).expanduser().is_dir():
        candidates += [str(p) for p in sorted(Path(apps_dir).expanduser().glob('*.app'))]
    seen, out = set(), []
    for path in candidates:
        if not usable(path):
            continue
        real = os.path.realpath(path)
        try:
            info = plistlib.loads((Path(real) / 'Contents/Info.plist').read_bytes())
        except Exception:   # unreadable or not a property list (plistlib raises expat's errors too)
            continue
        if real in seen or info.get('CFBundleIdentifier') not in ids:
            continue
        seen.add(real)
        p = plan(real, python)
        out.append({'app': real, 'name': info.get('CFBundleDisplayName') or info.get('CFBundleName') or Path(real).stem,
                    'game': p.get('game'), 'version': p.get('display_version') or info.get('CFBundleShortVersionString'),
                    'kind': p.get('kind', 'error'), 'reason': p.get('reason', ''), 'work': p.get('work'),
                    'commit': info.get('MNGSourceCommit')})
    return sorted(out, key=lambda a: (a['name'], a['app']))

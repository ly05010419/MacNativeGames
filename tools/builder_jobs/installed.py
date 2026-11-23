"""The game apps this Mac already has, for MNG Launcher's list (user 2026-11-12: "目录规范要和 launcher 统一 这样用launcher的
时候 就可以极速打包了"): every Mac app of a game this repository builds that macOS knows (LaunchServices) and that is still
there, plus those in the Launcher's apps folder, each with the update it needs (update_runtime.py --plan): an installed
game is updated in place, with the work folder it records, by the fastest update that fits.

Contract: apps(apps_dir) returns a list of dicts {app, name, game, version, kind, reason, work, commit, shaders}; it never
raises. shaders is the App's MNGShaders: 'title' for a quick App (only the title's shaders), else 'full'.
kind is express, fast, full or error (update_runtime.plan)."""
from concurrent.futures import ThreadPoolExecutor
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
ACNH_BUNDLE = 'org.macnativegames.acnh'   # games/acnh/build.py
EOW_BUNDLE = 'org.macnativegames.eow'     # games/eow/build.py
ODYSSEY_BUNDLE = 'org.macnativegames.odyssey'   # games/odyssey/build.py


def bundle_ids():
    """The Mac bundle IDs of the games the Launcher builds (build_app.APPS, and Ocarina of Time's; not repo.NOT_IN_LAUNCHER's)."""
    return {bundle for _, bundle, _ in repo.build_app().APPS.values()} | {OOT_BUNDLE, 'org.macnativegames.dread', 'org.macnativegames.tmnt.private', ACNH_BUNDLE, EOW_BUNDLE, ODYSSEY_BUNDLE}


# LaunchServices asked by bundle ID (NSWorkspace URLsForApplicationsWithBundleIdentifier, macOS 12+) through osascript's
# Objective-C bridge: about 0.05 s, where `lsregister -dump` writes out every app this Mac knows (4.9 s, 2026-11-12).
BY_BUNDLE_ID = """ObjC.import("AppKit");
function run(argv) {
  var out = [];
  argv.forEach(function (id) {
    var urls = $.NSWorkspace.sharedWorkspace.URLsForApplicationsWithBundleIdentifier(id);
    for (var i = 0; i < urls.count; i++) out.push(ObjC.unwrap(urls.objectAtIndex(i).path));
  });
  return out.join("\\n");
}"""


def registered(ids):
    """Paths LaunchServices has for these bundle IDs; the full dump only when the quick question cannot be asked."""
    try:
        done = subprocess.run(['/usr/bin/osascript', '-l', 'JavaScript', '-e', BY_BUNDLE_ID, *sorted(ids)],
                              capture_output=True, text=True, errors='replace', timeout=30)
        if done.returncode == 0:
            return [line for line in done.stdout.splitlines() if line]
    except (OSError, subprocess.TimeoutExpired):
        pass
    return dumped(ids)


def dumped(ids):
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


def plan(app, python=sys.executable, work=None, env=None):
    """update_runtime.py --plan for one app (with --work when given, in env when given: a quick start asks with
    OOT_SHADERS=title), or an 'error' plan saying why it could not be asked."""
    try:
        done = subprocess.run([python, str(repo.UPDATE_RUNTIME), str(app), '--plan'] + (['--work', str(work)] if work else []),
                              cwd=repo.REPO, capture_output=True, env=env,
                              text=True, timeout=300)
        return json.loads(done.stdout.strip().splitlines()[-1])
    except (OSError, subprocess.TimeoutExpired, ValueError, IndexError) as e:
        return {'app': str(app), 'kind': 'error', 'reason': f'update_runtime.py --plan failed: {e}'}


# The Launcher's own list of the game apps it has found or built (user 2026-11-18: "launcher 要自己 记住 不是靠 macOS 的
# 应用索引 ... 优先自己记住，然后 靠 macOS 的应用索引"): read first; macOS only adds apps the list does not have yet.
BUILDER = Path.home() / 'Library/Application Support/MacNativeGames/Builder'
KNOWN = BUILDER / 'known-apps.json'
LAST_LIST = BUILDER / 'installed.json'   # the window's last list (Model.rememberedInstalled): seeds a first known-apps.json


def known_apps(path=KNOWN, last_list=LAST_LIST):
    """The app paths the Launcher remembers; the apps of its last list when it has kept none yet; [] when neither reads."""
    try:
        return [str(p) for p in json.loads(Path(path).read_text())['apps'] if p]
    except (OSError, ValueError, KeyError, TypeError):
        pass
    try:
        return [str(a['app']) for a in json.loads(Path(last_list).read_text())['apps'] if a.get('app')]
    except (OSError, ValueError, KeyError, TypeError):
        return []


def gone(path):
    """The app was deleted or moved: its folder is there and it is not. An app on a disk that is not connected (its
    folder missing too) stays remembered for when the disk is back."""
    p = Path(path)
    return p.parent.is_dir() and not usable(path)


def remember(paths, path=KNOWN):
    """Add these apps to the Launcher's list and drop the remembered ones that are gone; written whole (a temporary
    file renamed), never raises."""
    try:
        kept = []
        for app in known_apps(path) + [str(p) for p in paths]:
            if app not in kept and not gone(app):
                kept.append(app)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        part = Path(path).with_suffix('.partial')
        part.write_text(json.dumps({'apps': kept}, indent=1, ensure_ascii=False) + '\n')
        os.replace(part, path)
    except OSError:
        pass


def neighbours(paths):
    """The apps in the folders these apps are in: games built into one folder are found together (2026-11-18:
    LaunchServices named Tears of the Kingdom but not the Odyssey, Animal Crossing and Echoes of Wisdom next to it)."""
    out = []
    for folder in sorted({str(Path(p).parent) for p in paths}):
        try:
            out += [str(p) for p in sorted(Path(folder).glob('*.app'))]
        except OSError:
            continue
    return out


def apps(apps_dir=None, python=sys.executable, known_file=KNOWN):
    """The Launcher's own list first, then its apps folder, then what macOS knows (LaunchServices), then the apps next
    to any of those; what is found is remembered."""
    ids = bundle_ids()
    candidates = known_apps(known_file)
    if apps_dir and Path(apps_dir).expanduser().is_dir():
        candidates += [str(p) for p in sorted(Path(apps_dir).expanduser().glob('*.app'))]
    candidates += registered(ids)
    candidates += neighbours([c for c in candidates if usable(c)])
    found = {}   # real path: Info.plist, each app once
    for path in candidates:
        if not usable(path):
            continue
        real = os.path.realpath(path)
        try:
            info = plistlib.loads((Path(real) / 'Contents/Info.plist').read_bytes())
        except Exception:   # unreadable or not a property list (plistlib raises expat's errors too)
            continue
        if real not in found and info.get('CFBundleIdentifier') in ids:
            found[real] = info
    # each app's plan is its own process and reads only its own app: asked at the same time
    with ThreadPoolExecutor(max_workers=max(1, min(8, len(found)))) as pool:
        plans = dict(zip(found, pool.map(lambda real: plan(real, python), found)))
    out = [{'app': real, 'name': info.get('CFBundleDisplayName') or info.get('CFBundleName') or Path(real).stem,
            'game': plans[real].get('game'),
            'version': plans[real].get('display_version') or info.get('CFBundleShortVersionString'),
            'kind': plans[real].get('kind', 'error'), 'reason': plans[real].get('reason', ''), 'work': plans[real].get('work'),
            'commit': info.get('MNGSourceCommit'),
            # 'title': a quick App (games/oot/build.py shader_mode), offered 补全着色器 on its card
            'shaders': info.get('MNGShaders') or 'full'} for real, info in found.items()]
    remember(list(found), known_file)
    return sorted(out, key=lambda a: (a['name'], a['app']))

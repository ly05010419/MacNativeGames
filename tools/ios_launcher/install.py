#!/usr/bin/env python3
"""Build the iPhone / iPad MNG Launcher, signed with your Apple team, and install it on a device over the cable.

  python3 tools/ios_launcher/install.py --work <a folder outside the checkout> [--device <name>] [--team <id>]

The Launcher on the device shows the games as the Mac Launcher's Cover Flow of game cases (the same SwiftUI files,
SHARED below, copied in from tools/builder_app) and starts the installed ones: each game's app answers its own URL scheme
(games/*/ios/tools/build_ipa.py url_scheme). Games are still built on the Mac and installed with
games/totk/ios/tools/install_device.py.

The cases' pictures are the player's own: the icons of the game apps this Mac's Launcher remembers
(tools/builder_jobs/installed.py known_apps), written as PNGs into this build only (Covers/); the repository carries no
game art. A game with no Mac app gets a plain case.

Signing: a provisioning profile already on this Mac that covers the app and the device (an Xcode-made team wildcard
profile, for one) signs it by hand; without one, Xcode's automatic signing is asked (-allowProvisioningUpdates; on this
Mac the command line sees no Xcode account, 2026-11-19, so a new team needs one run from Xcode's window first). A free
Apple ID signs for 7 days: run this again to renew.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
MAC_UI = REPO / 'tools/builder_app/Sources/MNGBuilder'
# The Mac Launcher's files the iPhone / iPad Launcher compiles as they are (tests/test_ios_launcher.py checks they use
# AppKit only inside `#if os(macOS)`)
SHARED = ('CoverFlow.swift', 'CaseDetail.swift', 'CaseAmounts.swift', 'CoverArt.swift', 'CoverFlowTrace.swift',
          'GameStrings.swift', 'DepthScene.swift', 'DepthSurface.swift', 'DepthLayers.swift')
ICON = REPO / 'tools/builder_app/Resources/AppIcon-iOS.png'   # the project icon (icon.png) filling 1024 px, opaque: iOS cuts its corners
RESIZE = REPO / 'tools/builder_app/resize_icon.js'        # icns -> PNG with AppKit (no sips: the build sandbox)
GAMES = ('oot', 'totk', 'botw', 'dkb', 'pkp', 'dread', 'tmnt')   # with an iPhone / iPad build: GameStrings.swift GameCatalog.onIPhone
# every case, both Launchers ("统一 b 两边数量一样", "顺序也一样"): GameStrings.swift GameCatalog.all
CASES = ('oot', 'totk', 'botw', 'eow', 'odyssey', 'dkb', 'acnh', 'pkp', 'dread', 'tmnt')
# A Mac game app's bundle identifier (build_app.py APPS, games/*/build.py) -> its game
MAC_BUNDLES = {'org.macnativegames.totk': 'totk', 'org.macnativegames.botw': 'botw', 'org.macnativegames.oot': 'oot',
               'org.macnativegames.bananza': 'dkb', 'org.macnativegames.pokopia': 'pkp',
               'org.macnativegames.dread': 'dread', 'org.macnativegames.tmnt.private': 'tmnt'}
BUNDLE = 'org.macnativegames.launcher'
PROFILES = Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles'
CATALOG = {'info': {'author': 'xcode', 'version': 1}}


def device_tools():
    """games/totk/ios/tools/install_device.py: teams, devices and the pinned XcodeGen, shared with the games' install."""
    spec = importlib.util.spec_from_file_location('mng_install_device', REPO / 'games/totk/ios/tools/install_device.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def known_apps():
    """The game apps the Mac Launcher remembers (tools/builder_jobs/installed.py)."""
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    from tools.builder_jobs.installed import known_apps as remembered
    return remembered()


def run(cmd, **kw):
    print('$ ' + ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def game_of(app):
    """The game of a Mac game app by its bundle identifier (MAC_BUNDLES; a longer one that extends it with a '.' too,
    as BotW 1.9's), else None."""
    try:
        bid = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier', '')
    except (OSError, ValueError):
        return None
    for known, game in MAC_BUNDLES.items():
        if bid == known or bid.startswith(known + '.') or re.fullmatch(re.escape(known) + r'\d+', bid):
            return game
    return None


def icon_of(app):
    """The icon file the app's Info.plist names (else AppIcon.icns), as the Mac Launcher reads it (HomePage picturePath)."""
    info = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())
    name = info.get('CFBundleIconFile') or 'AppIcon'
    return Path(app) / 'Contents/Resources' / (name if Path(name).suffix else name + '.icns')


def official_covers(folder):
    """Covers/<game>.jpg: each game's official key art (tools/builder_app/Resources/covers.json, user 2026-11-19),
    fetched on this Mac through the deps cache and checked against its SHA-256, into this phone's build only (the
    repository holds the addresses alone). A picture that cannot be fetched is left out: that game's case shows its
    app's icon (covers). Returns the games put in."""
    sys.path.insert(0, str(REPO))
    try:
        from tools.deps.core import fetch
    finally:
        sys.path.remove(str(REPO))
    folder.mkdir(parents=True, exist_ok=True)
    source = REPO / 'tools/builder_app/Resources/covers.json'
    shutil.copy2(source, folder / 'covers.json')   # where each picture is cut (Games.center)
    spec = json.loads(source.read_text())['covers']
    done = []
    for game in CASES:
        cover = spec.get(game)
        if not cover:
            continue
        try:
            picture = fetch(cover['url'], cover['sha256'], f'cover-{game}-{cover["sha256"][:12]}.jpg')
        except Exception as e:   # offline: the app's icon stands in
            print(f'official cover {game}: not fetched ({e})', flush=True)
            continue
        shutil.copy2(picture, folder / f'{game}.jpg')
        done.append(game)
    return done


def covers(folder, apps):
    """Covers/<game>.png from the first remembered Mac app of each game; returns {game: source app}."""
    folder.mkdir(parents=True, exist_ok=True)
    found = {}
    for app in apps:
        game = game_of(app)
        if game not in GAMES or game in found or not icon_of(app).is_file():
            continue
        run(['/usr/bin/osascript', '-l', 'JavaScript', RESIZE, icon_of(app), '1024', folder / f'{game}.png'])
        found[game] = app
    return found


def stage(work, apps):
    """A copy of this folder in the work folder with the shared Mac files, the icon and the covers: nothing is
    generated in the checkout."""
    project = work / 'ios_launcher'
    shutil.rmtree(project, ignore_errors=True)
    shutil.copytree(HERE, project, ignore=shutil.ignore_patterns('install.py', '__pycache__', '*.xcodeproj'))
    (project / 'Shared').mkdir()
    for name in SHARED:
        shutil.copy2(MAC_UI / name, project / 'Shared' / name)
    assets = project / 'Assets.xcassets'
    assets.mkdir()
    (assets / 'Contents.json').write_text(json.dumps(CATALOG))
    for name, entry in (('AppIcon.appiconset', {'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}),
                        ('LauncherIcon.imageset', {'idiom': 'universal'})):
        (assets / name).mkdir()
        shutil.copy2(ICON, assets / name / 'AppIcon.png')
        (assets / name / 'Contents.json').write_text(json.dumps({'images': [dict(entry, filename='AppIcon.png')], **CATALOG}))
    # the case pictures' depth model (DepthScene.swift), as it is: Core ML compiles it on the phone when first used
    sys.path.insert(0, str(REPO))
    try:
        from tools.deps.depth_model import depth_model
        from tools.deps.pins import DEPTH_MODEL_PACKAGE
    finally:
        sys.path.remove(str(REPO))
    model = depth_model()
    shutil.copytree(model / DEPTH_MODEL_PACKAGE, project / 'Models' / DEPTH_MODEL_PACKAGE)
    for name in ('LICENSE', 'README.md'):
        shutil.copy2(model / name, project / 'Models' / f'depth-anything-v2-small-{name}')
    official = official_covers(project / 'Covers')
    print('official covers: ' + (', '.join(official) or 'none (offline): the apps\' icons'), flush=True)
    found = covers(project / 'Covers', apps)
    for game in CASES:
        print(f'cover {game}: ' + (f'from {found[game]}' if game in found else 'none (no Mac app of it): a plain case'), flush=True)
    return project


def profile_for(team, bundle, udid):
    """A provisioning profile on this Mac for this app on this device, not expiring within a day: an explicit one
    first, then the team's wildcard; (path, its plist), or None (automatic signing then)."""
    best = None
    for path in PROFILES.glob('*.mobileprovision'):
        try:
            raw = subprocess.run(['openssl', 'smime', '-inform', 'der', '-verify', '-noverify', '-in', str(path)],
                                 check=True, capture_output=True).stdout
            info = plistlib.loads(raw)
        except (subprocess.CalledProcessError, ValueError):
            continue
        app_id = info.get('Entitlements', {}).get('application-identifier', '')
        expires = info.get('ExpirationDate')
        if (udid not in info.get('ProvisionedDevices', []) or not expires
                or (expires.replace(tzinfo=timezone.utc) - datetime.now(timezone.utc)).days < 1):
            continue
        rank = 2 if app_id == f'{team}.{bundle}' else 1 if app_id == f'{team}.*' else 0
        if rank and (best is None or rank > best[0]):
            best = (rank, path, info)
    return best[1:] if best else None


def identity_for(info):
    """The SHA-1 of a code-signing identity in the keychain whose certificate the profile names."""
    have = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'], check=True, capture_output=True,
                          text=True).stdout
    for der in info.get('DeveloperCertificates', []):
        sha1 = hashlib.sha1(der).hexdigest().upper()
        if sha1 in have:
            return sha1
    raise SystemExit("none of the profile's certificates is in the keychain: add your Apple ID in Xcode > Settings > Accounts")


def sign(app, team, bundle, profile):
    """Sign the unsigned app with the profile (an Xcode-managed wildcard cannot go through xcodebuild's manual signing:
    "is Xcode managed, but signing settings require a manually managed profile"), as install_device.py signs a game."""
    path, info = profile
    shutil.copy2(path, app / 'embedded.mobileprovision')
    granted = info.get('Entitlements', {})
    ents = {'application-identifier': f'{team}.{bundle}', 'com.apple.developer.team-identifier': team,
            'get-task-allow': bool(granted.get('get-task-allow', True))}
    entfile = app.parent / 'launcher.entitlements'
    entfile.write_bytes(plistlib.dumps(ents))
    run(['codesign', '--force', '--sign', identity_for(info), '--entitlements', entfile, '--generate-entitlement-der', app])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True, help='build folder, outside the source checkout')
    ap.add_argument('--device', help='device name (as in Finder) or identifier; needed when several are paired')
    ap.add_argument('--team', help='Apple team ID (default: the only team with an Apple Development certificate)')
    ap.add_argument('--bundle-id', default=BUNDLE, help=f'default {BUNDLE}; another team may need its own')
    ap.add_argument('--wifi', action='store_true', help='install on a device that is not on the cable')
    a = ap.parse_args()
    work = a.work.expanduser().resolve()
    if work == REPO or REPO in work.parents:
        raise SystemExit('--work must be outside the source checkout')
    work.mkdir(parents=True, exist_ok=True)
    tools = device_tools()
    found = tools.teams()
    team = a.team or (found[0] if len(found) == 1 else None)
    if not team:
        raise SystemExit(f'choose the team with --team: {", ".join(found)}' if found
                         else 'no Apple Development certificate: add your Apple ID in Xcode > Settings > Accounts')
    dev = tools.pick_device(a.device)
    if not a.wifi and dev.get('connectionProperties', {}).get('transportType') == 'localNetwork':
        raise SystemExit(f"{dev['deviceProperties'].get('name')} is connected over Wi-Fi: connect it with the cable (unlocked) "
                         'and run this again, or pass --wifi')
    udid = dev['hardwareProperties']['udid']
    print(f"team {team}, bundle {a.bundle_id}, device {dev['deviceProperties'].get('name')}", flush=True)
    project = stage(work, known_apps())
    env = dict(os.environ, MNG_LAUNCHER_BUNDLE_ID=a.bundle_id, MNG_LAUNCHER_TEAM=team)
    run([tools.xcodegen(), 'generate', '--spec', 'project.yml'], cwd=project, env=env)
    profile = profile_for(team, a.bundle_id, udid)
    signing = (['CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY='] if profile
               else ['-allowProvisioningUpdates'])
    print('signing: ' + (f'provisioning profile {profile[1]["UUID"]} on this Mac' if profile else "Xcode's automatic signing"),
          flush=True)
    derived = work / 'derived'
    run(['xcodebuild', '-project', 'MNGLauncherIOS.xcodeproj', '-scheme', 'MNGLauncher', '-configuration', 'Release',
         '-destination', 'generic/platform=iOS', '-derivedDataPath', derived, *signing], cwd=project, env=env)
    app = derived / 'Build/Products/Release-iphoneos/MNGLauncher.app'
    if not app.is_dir():
        raise SystemExit(f'Xcode did not produce {app}')
    if profile:
        sign(app, team, a.bundle_id, profile)
    run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', dev['identifier'], app])
    print('installed. First time on this device: Settings > General > VPN & Device Management > trust your Apple ID.')


if __name__ == '__main__':
    main()

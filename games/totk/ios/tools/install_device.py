#!/usr/bin/env python3
"""Sign an unsigned game IPA (Tears of the Kingdom or Breath of the Wild) with your own Xcode team and install it on a connected iPhone / iPad.

A free (Personal Team) Apple ID signs for 7 days, a paid membership for a year: run this again to renew. Reinstalling
with the same team and bundle identifier keeps the saves and the shader cache (they are in the app's Documents).

  python3 games/totk/ios/tools/install_device.py "Tears of the Kingdom.ipa" --device "Jack iPhone"

An IPA built with --ios-game-data documents carries no 17 GB game image: pass --game-data <work>/totk/extracted/v143/
romfs/romfs.bin and it is copied once into the app's Documents/GameData in 1 GiB parts (devicectl drops the connection on
one file that size); a later install skips it while the device holds the same image (romfs.sha256 there).

Xcode needs your Apple ID (Xcode > Settings > Accounts). The provisioning profile comes from a throwaway Xcode project
built with the same bundle identifier and entitlements (-allowProvisioningUpdates registers the device and the App ID).
"""
import argparse, hashlib, json, os, plistlib, re, shutil, subprocess, sys, tempfile, zipfile
from datetime import timezone
from pathlib import Path

ENTITLEMENT = 'com.apple.developer.kernel.increased-memory-limit'
CACHE = Path.home()/'Library/Caches/MacNativeGames/ios-signing'

def run(cmd, **kw): return subprocess.run([str(x) for x in cmd], check=True, **kw)
def out(cmd): return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, text=True).stdout

def teams():
    pems = out(['security','find-certificate','-a','-c','Apple Development','-p'])
    found = set()
    for pem in re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', pems, re.S):
        subject = subprocess.run(['openssl','x509','-noout','-subject'], input=pem, capture_output=True, text=True).stdout
        if m := re.search(r'OU\s*=\s*([A-Z0-9]{10})', subject): found.add(m.group(1))
    return sorted(found)

def devices():
    with tempfile.NamedTemporaryFile(suffix='.json') as f:
        run(['xcrun','devicectl','list','devices','--json-output',f.name], capture_output=True)
        return [d for d in json.load(open(f.name))['result']['devices']
                if d.get('hardwareProperties',{}).get('platform') == 'iOS']

def pick_device(name):
    devs = devices()
    match = [d for d in devs if name in (d['deviceProperties'].get('name'), d['identifier'], d['hardwareProperties'].get('udid'))] if name else devs
    if len(match) != 1:
        listed = '\n'.join(f"  {d['deviceProperties'].get('name')} ({d['hardwareProperties'].get('marketingName')})" for d in devs)
        sys.exit(f"choose the device with --device <name>:\n{listed}" if devs else 'no paired iPhone / iPad found: connect it with a cable and trust this Mac')
    return match[0]

def profile(team, bundle_id, udid):
    """Build an empty app with the game's bundle identifier and entitlements; Xcode makes the profile for it."""
    proj = CACHE/bundle_id; proj.mkdir(parents=True, exist_ok=True)
    (proj/'main.m').write_text('int main(void) { return 0; }\n')
    (proj/'TOTK.entitlements').write_bytes(plistlib.dumps({ENTITLEMENT: True}))
    (proj/'project.yml').write_text(f'''name: TOTKSigning
options: {{deploymentTarget: {{iOS: "26.0"}}}}
targets:
  TOTK:
    type: application
    platform: iOS
    sources: [main.m]
    settings:
      GENERATE_INFOPLIST_FILE: YES
      PRODUCT_BUNDLE_IDENTIFIER: {bundle_id}
      DEVELOPMENT_TEAM: {team}
      CODE_SIGN_STYLE: Automatic
      CODE_SIGN_ENTITLEMENTS: TOTK.entitlements
''')
    run(['xcodegen','generate','--quiet'], cwd=proj)
    derived = proj/'derived'
    log = proj/'xcodebuild.log'
    with log.open('w') as f:
        r = subprocess.run(['xcodebuild','-project','TOTKSigning.xcodeproj','-scheme','TOTK','-configuration','Debug',
                            '-destination',f'id={udid}','-derivedDataPath',str(derived),
                            '-allowProvisioningUpdates','-allowProvisioningDeviceRegistration','build'],
                           cwd=proj, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        errors = [l for l in log.read_text(errors='replace').splitlines() if 'error:' in l]
        sys.exit('Xcode could not make a provisioning profile:\n' + '\n'.join(errors[-8:]) + f'\nfull log: {log}')
    app = derived/'Build/Products/Debug-iphoneos/TOTK.app'
    return app/'embedded.mobileprovision', app

def identity_of(signed_app):
    """The SHA-1 of the certificate Xcode signed the empty app with: the same identity signs the game."""
    with tempfile.TemporaryDirectory() as d:
        run(['codesign','-d','--extract-certificates='+d+'/cert','-v',signed_app], capture_output=True)
        der = Path(d+'/cert0').read_bytes()
    return hashlib.sha1(der).hexdigest().upper()

def ipa_app(ipa):
    """The app's folder in the IPA (Payload/<name>.app) and its bundle identifier, read without unpacking (an unsigned
    .app folder: its own Info.plist)."""
    if ipa.is_dir(): return f'Payload/{ipa.name}', plistlib.loads((ipa/'Info.plist').read_bytes())['CFBundleIdentifier']
    with zipfile.ZipFile(ipa) as z:
        plists = [n for n in z.namelist() if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', n)]
        if len(plists) != 1: sys.exit(f'{ipa}: expected one app in Payload/, found {len(plists)}')
        return plists[0].removesuffix('/Info.plist'), plistlib.loads(z.read(plists[0]))['CFBundleIdentifier']

PART = 1 << 30
def device_files(dev, bundle_id, sub):
    """{name: size} of the app's Documents/<sub> on the device, {} when there is none."""
    with tempfile.NamedTemporaryFile(suffix='.json') as f:
        r = subprocess.run(['xcrun','devicectl','device','info','files','--device',dev,'--domain-type','appDataContainer',
                            '--domain-identifier',bundle_id,'--subdirectory',sub,'--json-output',f.name], capture_output=True)
        if r.returncode: return {}
        files = json.load(open(f.name)).get('result', {}).get('files', [])
    return {Path(x.get('relativePath', x.get('name', ''))).name: x.get('metadata', {}).get('size', 0) for x in files}   # devicectl's JSON

def push_game_data(dev, bundle_id, image):
    """The RomFS image into Documents/GameData as romfs.bin.000, .001, ... (ios/Sources/main.m reads it there), then
    romfs.sha256 naming it, so the next install skips it."""
    sha = json.loads((image.parent/'index.json').read_text())['romfs_sha256']
    size = image.stat().st_size; parts = (size + PART - 1) // PART
    have = device_files(dev, bundle_id, 'Documents/GameData')
    if have.get('romfs.sha256') == 64 + 1 and all(f'romfs.bin.{i:03d}' in have for i in range(parts)):
        with tempfile.TemporaryDirectory() as d:
            got = Path(d)/'romfs.sha256'
            r = subprocess.run(['xcrun','devicectl','device','copy','from','--device',dev,'--domain-type','appDataContainer',
                                '--domain-identifier',bundle_id,'--source','Documents/GameData/romfs.sha256','--destination',got], capture_output=True)
            if r.returncode == 0 and got.read_text().strip() == sha:
                print('game data: the device already has this image'); return
    print(f'game data: copying {size / 2**30:.1f} GiB in {parts} parts (once; later installs keep it)')
    with tempfile.TemporaryDirectory() as d, open(image, 'rb') as src:
        def copy(local, name):
            run(['xcrun','devicectl','device','copy','to','--device',dev,'--domain-type','appDataContainer','--domain-identifier',
                 bundle_id,'--source',local,'--destination',f'Documents/GameData/{name}'], capture_output=True)
        (Path(d)/'romfs.sha256').write_text('stale\n'); copy(Path(d)/'romfs.sha256', 'romfs.sha256')   # until every part is there
        for i in range(parts):
            part = Path(d)/f'romfs.bin.{i:03d}'
            part.write_bytes(src.read(PART)); copy(part, part.name); part.unlink()
            print(f'  part {i + 1}/{parts}', flush=True)
        (Path(d)/'romfs.sha256').write_text(sha + '\n'); copy(Path(d)/'romfs.sha256', 'romfs.sha256')

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ipa', type=Path, help='the unsigned IPA (or .app folder) from build_app.py --platform ios')
    ap.add_argument('--device', help='device name (as in Finder) or identifier; needed when several are paired')
    ap.add_argument('--team', help='Apple team ID (default: the only team with an Apple Development certificate)')
    ap.add_argument('--bundle-id', help="default: the IPA's own identifier + .<team> (org.macnativegames.totk.<team>, "
                                         "org.macnativegames.botw.<team>), so it is free to register on your team and each game keeps its own app")
    ap.add_argument('--game-data', type=Path, help='romfs.bin (index.json beside it) for an IPA built with --ios-game-data documents')
    a = ap.parse_args()
    team = a.team or (lambda t: t[0] if len(t) == 1 else sys.exit(
        f'choose the team with --team: {", ".join(t)}' if t else 'no Apple Development certificate: add your Apple ID in Xcode > Settings > Accounts'))(teams())
    app_dir, ipa_bundle = ipa_app(a.ipa)
    bundle_id = a.bundle_id or f'{ipa_bundle}.{team.lower()}'
    dev = pick_device(a.device)
    udid = dev['hardwareProperties']['udid']
    print(f"team {team}, bundle {bundle_id}, device {dev['deviceProperties'].get('name')} ({udid})")

    print('provisioning profile: asking Xcode')
    prov, signed = profile(team, bundle_id, udid)
    info = plistlib.loads(out(['security','cms','-D','-i',prov]).encode())
    ents = info['Entitlements']
    if not ents.get(ENTITLEMENT):
        print(f'warning: your team cannot grant {ENTITLEMENT}; iOS may close the game when it runs out of memory')
    expires = info['ExpirationDate'].replace(tzinfo=timezone.utc).astimezone()  # plistlib returns naive UTC
    print(f"provisioning profile valid until {expires:%Y-%m-%d %H:%M} local time (run this again before then)")

    # Unpack once per IPA; later runs only re-sign. 16 GiB of game data is signed in place.
    work = a.ipa.with_suffix('.signed'); stamp = work/'.ipa-mtime'
    app = work/app_dir
    if a.ipa.is_dir():   # an unsigned .app (build_app.py -o X.app): cloned, nothing to unpack
        print(f'cloning {a.ipa.name}')
        shutil.rmtree(work, ignore_errors=True); app.parent.mkdir(parents=True)
        import ctypes   # one APFS clone of the whole folder (~100,000 files), as build_app.py copy_tree
        libc = ctypes.CDLL(None); libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
        if libc.clonefile(os.fsencode(a.ipa), os.fsencode(app), 0) and subprocess.run(['/bin/cp', '-cR', a.ipa, app]).returncode:
            sys.exit(f'cannot copy {a.ipa}')
    elif not stamp.is_file() or stamp.read_text() != str(a.ipa.stat().st_mtime_ns):
        print(f'unpacking {a.ipa.name}')
        shutil.rmtree(work, ignore_errors=True); work.mkdir()
        run(['ditto','-x','-k',a.ipa,work])
        stamp.write_text(str(a.ipa.stat().st_mtime_ns))
    plist = app/'Info.plist'
    infoplist = plistlib.loads(plist.read_bytes()); infoplist['CFBundleIdentifier'] = bundle_id
    plist.write_bytes(plistlib.dumps(infoplist, fmt=plistlib.FMT_BINARY))
    shutil.copy2(prov, app/'embedded.mobileprovision')
    entfile = work/'entitlements.plist'
    entfile.write_bytes(plistlib.dumps({k: ents[k] for k in ('application-identifier','com.apple.developer.team-identifier',
                                                                'get-task-allow', ENTITLEMENT) if k in ents}))
    print('signing (a few minutes: every game file is hashed)')
    run(['codesign','--force','--sign',identity_of(signed),'--entitlements',entfile,'--generate-entitlement-der',app])
    print('installing (a few minutes over the cable)')
    run(['xcrun','devicectl','device','install','app','--device',dev['identifier'],app])
    if a.game_data: push_game_data(dev['identifier'], bundle_id, a.game_data)
    print('installed. First time on this device: Settings > General > VPN & Device Management > trust your Apple ID.')

if __name__ == '__main__': main()

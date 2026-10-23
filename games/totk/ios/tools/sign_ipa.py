#!/usr/bin/env python3
"""Sign an unsigned sideloadable IPA with an Ad Hoc / development provisioning profile.

The build (build_ipa.py) leaves the app unsigned. This wraps the whole bundle: the profile goes in as
embedded.mobileprovision, the profile's entitlements are applied to the app, and every nested code item is signed
before the app itself. The bundle ID must match the profile's application-identifier.

Example (the identity is found through the keychain search list; do not pass --keychain unless that keychain
also holds the Apple root):
  python3 ios/tools/sign_ipa.py --ipa TOTK.ipa --profile /path/to/profile.mobileprovision \
      --identity 'iPhone Distribution: Your Name (TEAMID)' --out TOTK-signed.ipa

A keychain made for signing needs the Apple WWDR intermediate and, in the keychain search list, the system roots:
  security create-keychain -p <password> /tmp/totk-sign.keychain-db
  security unlock-keychain -p <password> /tmp/totk-sign.keychain-db
  security import identity.p12 -k /tmp/totk-sign.keychain-db -P <p12 password> -T /usr/bin/codesign
  security import AppleWWDRCAG3.cer -k /tmp/totk-sign.keychain-db -T /usr/bin/codesign
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k <password> /tmp/totk-sign.keychain-db
  security list-keychains -d user -s /tmp/totk-sign.keychain-db \
      /System/Library/Keychains/SystemRootCertificates.keychain /Library/Keychains/System.keychain \
      $(security list-keychains -d user | tr -d '"')
Restore the search list afterwards if the keychain was only for this build. --keychain restricts chain building to
that keychain, so either leave it out (the search list above) or import the Apple root into it as well.
"""
import argparse
import datetime
import hashlib
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(args, **kwargs):
    print('+', ' '.join(str(x) for x in args))
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def load_profile(profile: Path) -> dict:
    raw = subprocess.check_output(['security', 'cms', '-D', '-i', str(profile)])
    return plistlib.loads(raw)


def identity_sha1(identity: str, keychain: str = '') -> str | None:
    """The SHA-1 of a codesigning identity's certificate, as `security find-identity -p codesigning` lists it.
    With --keychain the identity may live only there (a keychain made for signing is often not in the search list),
    so that keychain is searched first and the search list is still tried as a fallback. find-identity takes keychains
    as positional arguments, and listing one searches only that keychain."""
    searches = [[keychain]] if keychain else []
    searches.append([])
    for extra in searches:
        listing = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning', *extra],
                                 capture_output=True, text=True).stdout
        for line in listing.splitlines():
            m = re.match(r'\s*\d+\)\s+([0-9A-Fa-f]{40})\s+"(.+)"', line)
            if m and m.group(2) == identity:
                return m.group(1).lower()
    return None


def check_identity_allowed(profile: dict, identity: str, keychain: str = ''):
    """A profile authorizes specific developer certificates; signing with another one produces an IPA iOS refuses."""
    wanted = identity_sha1(identity, keychain)
    if wanted is None:
        where = f'{keychain} or the keychain search list' if keychain else 'the keychain search list'
        sys.exit(f'no codesigning identity named {identity!r} in {where}')
    allowed = {hashlib.sha1(bytes(c)).hexdigest() for c in profile.get('DeveloperCertificates') or []}
    if allowed and wanted not in allowed:
        sys.exit(f'the profile does not authorize {identity!r} (SHA-1 {wanted}); it lists '
                 + ', '.join(sorted(allowed)))


def check_profile_expiry(profile: dict):
    """An expired profile no longer authorizes installation; the IPA would sign cleanly and refuse to install."""
    expires = profile.get('ExpirationDate')
    if not isinstance(expires, datetime.datetime):
        return
    if expires.tzinfo is None:   # profile plist dates are UTC without a zone
        expires = expires.replace(tzinfo=datetime.timezone.utc)
    if expires <= datetime.datetime.now(datetime.timezone.utc):
        sys.exit(f'the profile expired on {expires:%Y-%m-%d %H:%M} UTC; get a fresh one')


def sign_item(path: Path, identity: str, keychain: str, entitlements: Path | None = None):
    args = ['codesign', '--force', '--sign', identity, '--timestamp=none', '--generate-entitlement-der']
    if keychain:
        args += ['--keychain', keychain]
    if entitlements:
        args += ['--entitlements', str(entitlements)]
    args.append(str(path))
    run(args)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ipa', type=Path, required=True, help='the unsigned IPA from build_ipa.py')
    p.add_argument('--profile', type=Path, required=True, help='the .mobileprovision to embed')
    p.add_argument('--identity', required=True, help='codesigning identity, e.g. "iPhone Distribution: Your Name (TEAMID)"')
    p.add_argument('--keychain', default='', help='keychain holding the identity (default: the search list)')
    p.add_argument('--out', type=Path, help='signed IPA (default: <ipa>-signed.ipa)')
    p.add_argument('--keep-work', action='store_true', help='keep the unzipped Payload next to the output')
    a = p.parse_args()

    ipa = a.ipa.resolve()
    out = (a.out or ipa.with_name(ipa.stem + '-signed.ipa')).resolve()
    if not ipa.is_file():
        sys.exit(f'missing IPA: {ipa}')
    if not a.profile.is_file():
        sys.exit(f'missing profile: {a.profile}')
    profile = load_profile(a.profile)
    check_profile_expiry(profile)
    ent = profile.get('Entitlements', {})
    app_id = ent.get('application-identifier', '')
    if '.' not in app_id:
        sys.exit('the profile has no application-identifier')
    wanted_bundle = app_id.split('.', 1)[1]
    wildcard = wanted_bundle.endswith('*')
    check_identity_allowed(profile, a.identity, a.keychain)
    if not ent.get('com.apple.developer.kernel.increased-memory-limit'):
        # build_ipa.py signs the executable ad hoc with this entitlement; --force replaces the signature's
        # entitlements with the profile's. Without it the game's 3.2 GiB pool falls back to the iOS default limit.
        print('warning: the profile does not carry com.apple.developer.kernel.increased-memory-limit; '
              'the signed app will not get the increased memory limit', file=sys.stderr)

    work = Path(tempfile.mkdtemp(prefix='totk-sign-', dir=out.parent))
    try:
        run(['ditto', '-x', '-k', ipa, work])
        apps = list((work / 'Payload').glob('*.app'))
        if len(apps) != 1:
            sys.exit(f'expected one .app in Payload, found {len(apps)}')
        app = apps[0]
        info = plistlib.loads((app / 'Info.plist').read_bytes())
        bundle = info.get('CFBundleIdentifier', '')
        if wildcard:
            if not bundle.startswith(wanted_bundle[:-1]):
                sys.exit(f'bundle ID {bundle!r} does not match the profile {wanted_bundle!r}; build with --bundle-id '
                         f'{wanted_bundle[:-1]}<something>')
            # The profile authorizes the pattern; the signature must carry the concrete app id, or iOS rejects it.
            ent = dict(ent, **{'application-identifier': f'{app_id.split(".", 1)[0]}.{bundle}'})
        elif bundle != wanted_bundle:
            sys.exit(f'bundle ID {bundle!r} does not match the profile {wanted_bundle!r}; build with --bundle-id '
                     f'{wanted_bundle} or make a profile for {bundle}')

        shutil.copy2(a.profile, app / 'embedded.mobileprovision')
        ent_path = work / 'signing-entitlements.plist'
        ent_path.write_bytes(plistlib.dumps(ent))

        # Nested code first: frameworks, dylibs and app extensions, deepest first.
        nested = []
        for root in (app, app / 'Frameworks', app / 'PlugIns'):
            if not root.is_dir():
                continue
            for item in root.rglob('*'):
                if item.is_dir() and item.suffix in ('.framework', '.appex', '.xpc'):
                    nested.append(item)
                elif item.is_file() and item.suffix in ('.dylib',):
                    nested.append(item)
        for item in sorted(set(nested), key=lambda x: len(x.parts), reverse=True):
            # Nested code never carries the app's entitlements: a framework has none, and an app extension needs its
            # own (this script signs the game app, which has neither).
            sign_item(item, a.identity, a.keychain)

        sign_item(app, a.identity, a.keychain, ent_path)
        run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', app])

        if out.exists():
            out.unlink()
        run(['zip', '-qry', '-0', '-y', out, 'Payload'], cwd=work)
        print(f'signed IPA: {out} ({out.stat().st_size / 2**30:.1f} GiB)')
        print(f'signed app: {app}')
        if a.keep_work:
            print(f'work dir kept: {work}')
            work = None
    finally:
        if work and work.exists():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
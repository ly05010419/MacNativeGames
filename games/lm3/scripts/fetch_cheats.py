#!/usr/bin/env python3
"""The cheats of this exact game version from switch-cheats-db (github.com/HamletDuFromage/switch-cheats-db, a daily
mirror of the GBAtemp cheat codes; credit to their authors), as one Atmosphère cheat file:
    python fetch_cheats.py --main extracted/v120/exefs/main --title 0100DCA0064A6000 --output build/cheats.txt
Cheats are keyed by the main module's build ID (NSO header +0x40). Without a match the output is removed, so the app
has no Cheats menu; a failed download does the same and does not stop the build."""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

URL = 'https://raw.githubusercontent.com/HamletDuFromage/switch-cheats-db/master/cheats_gbatemp/{}.json'


def build_id(main):
    return Path(main).read_bytes()[0x40:0x48].hex().upper()


def cheat_text(db, bid):
    """The Atmosphère text for build ID bid, or None when the database has no cheats for it."""
    entry = db.get(bid)
    if not isinstance(entry, dict) or not entry:
        return None
    return ''.join(v if v.endswith('\n') else v + '\n' for v in entry.values() if isinstance(v, str))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--main', required=True)
    ap.add_argument('--title', required=True)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    bid = build_id(a.main)
    try:
        with urllib.request.urlopen(URL.format(a.title.upper()), timeout=30) as r:
            db = json.load(r)
    except Exception as e:   # offline, or no file for this title
        db = {}
        print(f'switch-cheats-db: nothing for {a.title.upper()} ({e})', file=sys.stderr)
    text = cheat_text(db, bid)
    if text is None:
        a.output.unlink(missing_ok=True)
        print(f'switch-cheats-db: no cheats for build ID {bid}; the app has no Cheats menu')
        return
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(f'# github.com/HamletDuFromage/switch-cheats-db, build ID {bid} (GBAtemp cheat codes; credit to their authors)\n' + text)
    print(f'switch-cheats-db: {text.count(chr(10) + "[") + text.startswith("[")} cheats for build ID {bid} -> {a.output}')


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        db = {'AA': {'[a]': '[a]\n04000000 00000000 00000001\n\n', '[b]': '[b]\n04000000 00000004 00000002'}}
        assert cheat_text(db, 'BB') is None and cheat_text({'AA': {}}, 'AA') is None
        t = cheat_text(db, 'AA')
        assert t.startswith('[a]\n') and t.endswith('00000002\n') and t.count('[') == 2
        print('ok')
    else:
        main()

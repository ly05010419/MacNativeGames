"""python3.14 -m tools.ui_pack build (--game botw|totk-1.4.3 | --keys <tsv>) --harvest <dir> --out <pack dir>
       [--screen-height 1080] [--only k1,k2] [--all] [--force] [--dry-run] [--jobs N]
Exit codes: 0 done (a key that fails the gate is left out of the pack and listed in manifest.tsv and the summary; the game draws its own texture), 2 a bad key list, harvest or argument."""
import argparse
import os
from pathlib import Path
import sys

from tools.ui_pack.build import build
from tools.ui_pack.errors import UiPackError
from tools.ui_pack.keylist import read_keylist

REPO_ROOT = Path(__file__).resolve().parents[2]
GAMES = {'botw': 'games/botw/assets/ui_pack_keys.tsv', 'totk-1.4.3': 'games/totk/assets/ui_pack_keys_v143.tsv'}


def build_parser():
    ap = argparse.ArgumentParser(prog='python3.14 -m tools.ui_pack', description='Build the sharp UI texture pack from a harvest folder.')
    sub = ap.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build', help='trace the harvested UI masks and write a pack folder')
    g = b.add_mutually_exclusive_group(required=True)
    g.add_argument('--game', choices=sorted(GAMES), help='use the key list of this game (games/<game>/assets)')
    g.add_argument('--keys', type=Path, help='a key list tsv')
    b.add_argument('--harvest', type=Path, required=True, help='the harvest folder (harvest.tsv and the PNGs)')
    b.add_argument('--out', type=Path, required=True, help='the pack folder')
    b.add_argument('--screen-height', type=int, default=1080, help='screen height in px the pack is built for (default 1080)')
    b.add_argument('--only', help='only these keys, comma separated')
    b.add_argument('--all', action='store_true', help='also build the harvested keys that are not in the list, with the default options')
    b.add_argument('--force', action='store_true', help='build the pictures again whose manifest row is current')
    b.add_argument('--dry-run', action='store_true', help='print what would be built or deleted; write nothing')
    b.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1), help='keys built in parallel (default min(4, CPUs))')
    return ap


def main(argv=None):
    args = build_parser().parse_args(sys.argv[1:] if argv is None else argv)
    if args.screen_height <= 0 or args.jobs < 1:
        print('ui_pack: --screen-height and --jobs must be positive', file=sys.stderr)
        return 2
    try:
        keylist = read_keylist(args.keys if args.keys else REPO_ROOT / GAMES[args.game])
        build(args.harvest, args.out, keylist, args.screen_height, only=args.only.split(',') if args.only else None, all_keys=args.all,
                       force=args.force, dry_run=args.dry_run, jobs=args.jobs, log=lambda line: print(line, flush=True))
    except (UiPackError, OSError) as error:
        print(f'ui_pack: {error}', file=sys.stderr)
        return 2
    return 0

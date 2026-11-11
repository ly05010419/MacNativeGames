"""python -m tools.builder_jobs classify|checks|build ... (see __init__.py)."""
import argparse
import json
from pathlib import Path
import sys

HISTORY = Path.home() / 'Library/Application Support/MacNativeGames/Builder/history'


def main(argv):
    ap = argparse.ArgumentParser(prog='python -m tools.builder_jobs')
    sub = ap.add_subparsers(dest='job', required=True)
    c = sub.add_parser('classify', help='what the dropped files are, and the build they make (one JSON object)')
    c.add_argument('paths', nargs='+')
    c.add_argument('--work', help='the work folder the build will use (its extracted fonts count)')
    k = sub.add_parser('checks', help="what this Mac still needs (one JSON object)")
    k.add_argument('--work', default=str(Path.home()))
    b = sub.add_parser('build', help='build; one JSON event per line')
    b.add_argument('request', type=Path)
    b.add_argument('--history', type=Path, default=HISTORY)
    a = ap.parse_args(argv)
    if a.job == 'classify':
        from tools.builder_jobs.classify import report
        print(json.dumps(report(a.paths, a.work), ensure_ascii=False))
        return 0
    if a.job == 'checks':
        from tools.builder_jobs.checks import checks
        print(json.dumps({'checks': checks(a.work)}, ensure_ascii=False))
        return 0
    from tools.builder_jobs.build_job import main as build
    return build(a.request, a.history)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

"""python -m tools.builder_jobs classify|checks|apps|build ... (see __init__.py)."""
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
    i = sub.add_parser('apps', help='the game apps on this Mac and the update each needs (one JSON object)')
    i.add_argument('--apps-dir', help="the Launcher's apps folder, looked in besides what macOS knows")
    tb = sub.add_parser('tab', help='a page besides building (settings, mods, controls, storage, saves); one JSON object')
    tb.add_argument('tab')
    tb.add_argument('action')
    tb.add_argument('--app', required=True)
    tb.add_argument('--args', default='')
    s = sub.add_parser('shaders', help="what the game's newest shader list adds to an app, changing nothing (one JSON object)")
    s.add_argument('--app', required=True)
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
    if a.job == 'tab':
        from tools.builder_jobs.tabs import main as tab
        return tab(a.tab, a.action, a.app, a.args)
    if a.job == 'shaders':
        from tools.builder_jobs.shaders import main as shaders
        return shaders(a.app)
    if a.job == 'apps':
        from tools.builder_jobs.installed import apps
        print(json.dumps({'apps': apps(a.apps_dir)}, ensure_ascii=False))
        return 0
    from tools.builder_jobs.build_job import main as build
    return build(a.request, a.history)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

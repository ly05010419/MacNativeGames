#!/usr/bin/env python3
"""Remind iPhone / iPad players on their Mac to send the shaders and render pipelines the shared list does not have yet.

The iOS app notes them in Documents/shader-list-contribution.txt but never asks during play
(third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h). While a device with the game is connected to this Mac (cable or the
same network), `check` copies that file into the project folder the build ran from (<clone>/shader-list-contribution/, given
as --store by build_app.py; players find it there, and each reminder opens it in Finder) and,
every 100 new rows (shaders and pipelines together) per device, asks once on the Mac to send it in a GitHub issue. When the GitHub CLI (gh) is installed and
signed in, the question offers "Submit automatically" instead of "Remind me next time": that sends this file at once (a
secret gist and an issue that links it), without the browser. `install` runs `check` every 10 minutes as a LaunchAgent
(build_app.py --platform ios installs it); `uninstall` removes it.

usage: shader_contrib_watch.py check [--store <folder>] | install [--store <folder>] | uninstall"""
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

STEP = 100                     # as TOTK_CONTRIB_STEP in totk_shader_contrib.h
CHECK_EVERY = 10 * 60          # the LaunchAgent's interval
PULL_EVERY = 30 * 60           # one copy per device at most this often
ISSUE_URL = 'https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml'
APP_NAME = 'Tears of the Kingdom'
LABEL = 'org.macnativegames.totk.ios-shader-contrib'
DATA = Path.home() / 'Library/Application Support/MacNativeGames/TOTK'
WORK = DATA / 'ios-contributions'   # each device's copy by UDID and its reminder state (hidden: players do not need them)
STORE = WORK                        # main: --store, normally <clone>/shader-list-contribution, holding the one file players send
MERGED = 'shader-list-contribution.txt'   # the Mac's and every device's rows (app/launcher.m contrib_file writes the Mac's)
AGENT = Path.home() / f'Library/LaunchAgents/{LABEL}.plist'
LOG = Path.home() / 'Library/Logs/MacNativeGames/ios-shader-contrib.log'
REPO = 'ly05010419/MacNativeGames'
UPLOAD_NAME = 'shader-list-contribution.txt'   # the copy here is named after the device id, which is not sent
GH_CANDIDATES = ('/opt/homebrew/bin/gh', '/usr/local/bin/gh')   # the LaunchAgent's PATH has no Homebrew


def reachable_devices(listing):
    """(devicectl id, udid, name, iPhone or iPad) of the devices devicectl can reach now, from `list devices --json-output`."""
    out = []
    for dev in listing.get('result', {}).get('devices', []):
        hw, conn = dev.get('hardwareProperties', {}), dev.get('connectionProperties', {})
        if hw.get('platform') == 'iOS' and conn.get('pairingState') == 'paired' and conn.get('transportType'):
            out.append((dev['identifier'], hw.get('udid') or dev['identifier'], dev.get('deviceProperties', {}).get('name', ''),
                        'iPad' if hw.get('deviceType') == 'iPad' else 'iPhone'))
    return out


def game_bundle(apps):
    """The game's bundle identifier on a device, from `device info apps --json-output`, or None."""
    for app in apps.get('result', {}).get('apps', []):
        if app.get('name') in (APP_NAME, 'TOTK') or '.totk' in app.get('bundleIdentifier', ''):   # TOTK: the home screen name since 2026-10-22
            return app['bundleIdentifier']
    return None


def contribution_game(text):
    """The game and version the device's app wrote into its file ("# Game: TOTK 1.4.3", the last one), or None: an app built
    before 2026-10-29 writes none."""
    found = re.findall(r'^# Game: ([A-Za-z]+ \d+\.\d+\.\d+)$', text, re.M)
    return found[-1] if found else None


def contribution_rows(text):
    return len({line for line in text.splitlines() if line.startswith(('V\t', 'R\t', 'B\t'))})   # shaders and pipelines


def due(rows, reminded):
    return rows >= reminded + STEP


def chinese():
    try:
        langs = subprocess.run(['defaults', 'read', '-g', 'AppleLanguages'], capture_output=True, text=True).stdout
    except OSError:
        return False
    first = next((l.strip(' ",()') for l in langs.splitlines() if l.strip(' ",()')), '')
    return first.startswith('zh')


def message(rows, device, auto=False):
    """(title, text, the middle button, "never", "send"); with auto (gh ready) the middle button submits at once."""
    if chinese():
        return ('期待你的贡献', f'你的 {device} 上有 {rows} 条着色器和渲染管线可以贡献：玩的时候遇到的、公共着色器清单里还没有的。'
                f'它们只有着色器和管线的标识，不含游戏内容、存档或个人信息。发到 GitHub 加进公共清单后，以后大家打包的 App 都会带上它们，'
                f'玩到那里不再缺画面。\n\n'
                + ('点“自动提交”会用这台 Mac 上登录的 GitHub 命令行（gh）直接提交，不用打开网页。' if auto else '')
                + '点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。',
                '自动提交' if auto else '下次提醒', '不要提醒了', '发送')
    return ('We would love your contribution', f'Your {device} has {rows} shaders and render pipelines to contribute: ones the game met that the '
            f'shared shader list does not have yet. They are shader and pipeline identifiers only: no game data, saves or personal '
            f'information. Sent to GitHub and added to the shared list, they come with everyone\'s next build, and nothing '
            f'is missing there any more.\n\n'
            + ('"Submit automatically" sends it now with the GitHub CLI (gh) signed in on this Mac, without the browser. ' if auto else '')
            + '"Send" opens the GitHub form and shows the file in Finder: drag it into the form.',
            'Submit automatically' if auto else 'Remind me next time', "Don't remind me again", 'Send')


def gh_path():
    """The GitHub CLI when it is installed and signed in, else None."""
    for exe in dict.fromkeys([shutil.which('gh'), *GH_CANDIDATES]):
        if exe and os.access(exe, os.X_OK):
            try:
                return exe if subprocess.run([exe, 'auth', 'status'], capture_output=True, timeout=30).returncode == 0 else None
            except (OSError, subprocess.TimeoutExpired):
                return None
    return None


def gist_raw(gist_url):
    """The raw file link of a gist `gh gist create` made, or None."""
    m = re.search(r'https://gist\.github\.com/([\w-]+)/([0-9a-f]+)', gist_url or '')
    return f'https://gist.githubusercontent.com/{m[1]}/{m[2]}/raw/{UPLOAD_NAME}' if m else None


def issue_body(platform, link, game=None):
    """An issue laid out like the shader-list form, so scripts/collect_shader_issues.py reads it the same way. game: the
    device's "TOTK 1.4.3", whose version fills the form's field (an older app gives none: the field says how it came)."""
    version = game.split()[-1] if game else 'Sent automatically / 自动提交'
    return (f'### Game version / 游戏版本\n\n{version}\n\n### Device / 设备\n\n{platform}\n\n'
            f'### {UPLOAD_NAME}\n\n{link}\n\n### Where you played (optional) / 玩了哪些地方（可选）\n\n_No response_\n')


def submit(gh, path, rows, platform, game=None):
    """Sends the file with gh: a secret gist, then an issue linking its raw file. The issue's link, or None."""
    try:
        made = subprocess.run([gh, 'gist', 'create', '--filename', UPLOAD_NAME, '--desc', 'MacNativeGames shader list contribution', '-'],
                              input=path.read_text(errors='replace'), capture_output=True, text=True, timeout=120)
        link = gist_raw(made.stdout) if made.returncode == 0 else None
        if not link:
            return None
        issue = subprocess.run([gh, 'issue', 'create', '--repo', REPO, '--title', f'Shader list: {rows} shaders from {platform}' + (f' ({game})' if game else ''),
                                '--body', issue_body(platform, link, game)], capture_output=True, text=True, timeout=120)
        found = re.search(r'https://github\.com/\S+/issues/\d+', issue.stdout) if issue.returncode == 0 else None
        return found[0] if found else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def note(text):
    subprocess.run(['osascript', '-e', f'display dialog {quote(text)} buttons {{"OK"}} default button "OK" with icon note giving up after 600'],
                   capture_output=True)


def quote(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def ask(rows, device, path, platform='iPhone', game=None):
    """Shows the reminder; 'auto', 'send', 'never' or 'later' (also when it times out unanswered)."""
    gh = gh_path()
    title, text, middle, never, send = message(rows, device, auto=bool(gh))
    script = (f'display dialog {quote(text)} with title {quote(title)} buttons {{{quote(never)}, {quote(middle)}, {quote(send)}}} '
              f'default button {quote(send)} with icon note giving up after 3600')
    subprocess.run(['open', '-R', str(path)])   # the folder in Finder with the file selected, before the question (user 2026-10-22)
    answer = subprocess.run(['osascript', '-e', script], capture_output=True, text=True).stdout
    if gh and f'button returned:{middle}' in answer:
        link = submit(gh, path, rows, platform, game)
        zh = chinese()
        if link:
            note((f'感谢你的 shader 贡献，让我们一起让游戏更流畅！\n{link}' if zh else
                  f"Thank you for your shader contribution! Let's make the game smoother together!\n{link}"))
            return 'auto'
        note('自动提交没有成功，改为打开提交页面。' if zh else 'Submitting automatically did not work; opening the form instead.')
        answer = f'button returned:{send}'
    if f'button returned:{send}' in answer:
        subprocess.run(['open', ISSUE_URL])
        return 'send'
    return 'never' if f'button returned:{never}' in answer else 'later'


def reminded_at(state_text):
    """The row count of the last reminder from <udid>.txt.reminded, or None after "Don't remind me again"."""
    text = (state_text or '').strip()
    return None if text.startswith('never') else int(text or 0)


def devicectl(*args):
    """Runs `xcrun devicectl ... --json-output`; the parsed JSON, or None when the device or the tool is not there."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'out.json'
        try:
            r = subprocess.run(['xcrun', 'devicectl', *args, '--json-output', str(out)], capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode or not out.exists():
            return None
        return json.loads(out.read_text())


def merge_rows(src, dst):
    """The V / R / B rows of src that dst lacks, appended to dst (made with a header if missing); returns dst's row count."""
    have = dst.read_text(errors='replace').splitlines() if dst.exists() else []
    known = set(have)
    new = [l for l in src.read_text(errors='replace').splitlines() if l.startswith(('V\t', 'R\t', 'B\t')) and l not in known]
    if not dst.exists():
        dst.write_text('# Shaders and render pipelines this Mac and its iPhones / iPads met that the shared shader list does not have yet\n'
                       '# (docs/shader-list.md). Only shader keys, bind states and pipeline descriptors: no game data or personal information.\n'
                       f'# Send it in a GitHub issue: {ISSUE_URL}\n')
    if new:
        with dst.open('a') as f: f.write(''.join(l + '\n' for l in dict.fromkeys(new)))
    return contribution_rows(dst.read_text(errors='replace'))


def check():
    WORK.mkdir(parents=True, exist_ok=True); STORE.mkdir(parents=True, exist_ok=True)
    listing = devicectl('list', 'devices')
    for ident, udid, name, platform in reachable_devices(listing or {}):
        merged = STORE / MERGED
        state = STORE / f'.{MERGED}.reminded'   # shared with the Mac launcher (totk_shader_contrib.h totk_contrib_state)
        if reminded_at(state.read_text() if state.exists() else '') is None:
            continue   # the player chose "Don't remind me again" for this device
        stamp = WORK / f'{udid}.checked'
        if stamp.exists() and time.time() - stamp.stat().st_mtime < PULL_EVERY:
            continue
        stamp.touch()
        bundle = game_bundle(devicectl('device', 'info', 'apps', '--device', ident) or {})
        if not bundle:
            continue
        path = WORK / f'{udid}.txt'
        if devicectl('device', 'copy', 'from', '--device', ident, '--domain-type', 'appDataContainer', '--domain-identifier',
                     bundle, '--source', 'Documents/shader-list-contribution.txt', '--destination', str(path)) is None:
            continue
        game = contribution_game(path.read_text(errors='replace'))   # the device's game and version, for the issue
        rows = merge_rows(path, merged)   # into the one file players send, with the Mac's rows
        reminded = reminded_at(state.read_text() if state.exists() else '')
        print(f'{time.strftime("%Y-%m-%d %H:%M:%S")} {name}: {rows} rows (reminded at {reminded})', flush=True)
        if due(rows, reminded):
            state.write_text(f'{rows}\n')   # "next time": STEP rows later
            if ask(rows, name or platform, merged, platform, game) == 'never':
                state.write_text('never\n')


def agent_plist(python, script, developer_dir, store=None):
    return {'Label': LABEL, 'ProgramArguments': [python, str(script), 'check', *(['--store', str(store)] if store else [])], 'StartInterval': CHECK_EVERY,
            'RunAtLoad': False, 'ProcessType': 'Background', 'EnvironmentVariables': {'DEVELOPER_DIR': developer_dir},
            'StandardOutPath': str(LOG), 'StandardErrorPath': str(LOG)}


def install(store=None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from xcode_dir import developer_dir   # devicectl comes with the Xcode that built the iOS app
    script = DATA / 'tools' / Path(__file__).name   # a copy: the agent keeps working when the repository moves
    script.parent.mkdir(parents=True, exist_ok=True); LOG.parent.mkdir(parents=True, exist_ok=True); AGENT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(__file__, script)
    python = '/usr/bin/python3' if Path('/usr/bin/python3').exists() else sys.executable   # not a work folder's venv, which may go
    AGENT.write_bytes(plistlib.dumps(agent_plist(python, script, developer_dir(), store)))
    domain = f'gui/{os.getuid()}'
    subprocess.run(['launchctl', 'bootout', f'{domain}/{LABEL}'], capture_output=True)
    subprocess.run(['launchctl', 'bootstrap', domain, str(AGENT)], check=True, capture_output=True)
    print(f'installed {AGENT}: checks connected iPhones / iPads every {CHECK_EVERY // 60} minutes; contributions in {store or STORE}')


def uninstall():
    subprocess.run(['launchctl', 'bootout', f'gui/{os.getuid()}/{LABEL}'], capture_output=True)
    AGENT.unlink(missing_ok=True)
    (DATA / 'tools' / Path(__file__).name).unlink(missing_ok=True)
    print(f'removed {AGENT}')


if __name__ == '__main__':
    args = sys.argv[1:]
    command = args[0] if args else ''
    store = Path(args[2]).expanduser().resolve() if len(args) == 3 and args[1] == '--store' else None
    if command not in ('check', 'install', 'uninstall') or len(args) not in (1, 3) or (len(args) == 3 and not store):
        sys.exit(__doc__)
    if store: STORE = store
    {'check': check, 'install': lambda: install(store), 'uninstall': uninstall}[command]()

#!/usr/bin/env python3
"""Remind iPhone / iPad players on their Mac to send the shaders the shared list does not have yet.

The iOS app notes those shaders in Documents/shader-list-contribution.txt but never asks during play
(third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h). While a device with the game is connected to this Mac (cable or the
same network), `check` copies that file to ~/Library/Application Support/MacNativeGames/TOTK/ios-contributions/ and,
every 500 new rows per device, asks once on the Mac to send it in a GitHub issue. `install` runs `check` every
10 minutes as a LaunchAgent (build_app.py --platform ios installs it); `uninstall` removes it.

usage: shader_contrib_watch.py check | install | uninstall"""
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

STEP = 500                     # as TOTK_CONTRIB_STEP in totk_shader_contrib.h
CHECK_EVERY = 10 * 60          # the LaunchAgent's interval
PULL_EVERY = 30 * 60           # one copy per device at most this often
ISSUE_URL = 'https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml'
APP_NAME = 'Tears of the Kingdom'
LABEL = 'org.macnativegames.totk.ios-shader-contrib'
DATA = Path.home() / 'Library/Application Support/MacNativeGames/TOTK'
STORE = DATA / 'ios-contributions'
AGENT = Path.home() / f'Library/LaunchAgents/{LABEL}.plist'
LOG = Path.home() / 'Library/Logs/MacNativeGames/ios-shader-contrib.log'


def reachable_devices(listing):
    """(devicectl id, udid, name) of the iPhones / iPads devicectl can reach now, from `list devices --json-output`."""
    out = []
    for dev in listing.get('result', {}).get('devices', []):
        hw, conn = dev.get('hardwareProperties', {}), dev.get('connectionProperties', {})
        if hw.get('platform') == 'iOS' and conn.get('pairingState') == 'paired' and conn.get('transportType'):
            out.append((dev['identifier'], hw.get('udid') or dev['identifier'], dev.get('deviceProperties', {}).get('name', '')))
    return out


def game_bundle(apps):
    """The game's bundle identifier on a device, from `device info apps --json-output`, or None."""
    for app in apps.get('result', {}).get('apps', []):
        if app.get('name') == APP_NAME or app.get('bundleIdentifier', '').endswith('.totk'):
            return app['bundleIdentifier']
    return None


def contribution_rows(text):
    return len({line for line in text.splitlines() if line.startswith('V\t')})


def due(rows, reminded):
    return rows >= reminded + STEP


def chinese():
    try:
        langs = subprocess.run(['defaults', 'read', '-g', 'AppleLanguages'], capture_output=True, text=True).stdout
    except OSError:
        return False
    first = next((l.strip(' ",()') for l in langs.splitlines() if l.strip(' ",()')), '')
    return first.startswith('zh')


def message(rows, device):
    if chinese():
        return ('期待你的贡献', f'你的 {device} 上有 {rows} 个着色器可以贡献：玩的时候遇到的、公共着色器清单里还没有的。'
                f'它们只有着色器的标识，不含游戏内容、存档或个人信息。发到 GitHub 加进公共清单后，以后大家打包的 App 都会带上它们，'
                f'玩到那里不再缺画面。\n\n点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。', '下次提醒', '不要提醒了', '发送')
    return ('We would love your contribution', f'Your {device} has {rows} shaders to contribute: ones the game met that the '
            f'shared shader list does not have yet. They are shader identifiers only: no game data, saves or personal '
            f'information. Sent to GitHub and added to the shared list, they come with everyone\'s next build, and nothing '
            f'is missing there any more.\n\n"Send" opens the GitHub form and shows the file in Finder: drag it into the form.',
            'Remind me next time', "Don't remind me again", 'Send')


def ask(rows, device, path):
    """Shows the reminder; 'send', 'never' or 'later' (also when it times out unanswered)."""
    title, text, later, never, send = message(rows, device)
    quote = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    script = (f'display dialog {quote(text)} with title {quote(title)} buttons {{{quote(never)}, {quote(later)}, {quote(send)}}} '
              f'default button {quote(send)} with icon note giving up after 3600')
    answer = subprocess.run(['osascript', '-e', script], capture_output=True, text=True).stdout
    if f'button returned:{send}' in answer:
        subprocess.run(['open', ISSUE_URL])
        subprocess.run(['open', '-R', str(path)])
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


def check():
    STORE.mkdir(parents=True, exist_ok=True)
    listing = devicectl('list', 'devices')
    for ident, udid, name in reachable_devices(listing or {}):
        state = STORE / f'{udid}.txt.reminded'
        if reminded_at(state.read_text() if state.exists() else '') is None:
            continue   # the player chose "Don't remind me again" for this device
        stamp = STORE / f'{udid}.checked'
        if stamp.exists() and time.time() - stamp.stat().st_mtime < PULL_EVERY:
            continue
        stamp.touch()
        bundle = game_bundle(devicectl('device', 'info', 'apps', '--device', ident) or {})
        if not bundle:
            continue
        path = STORE / f'{udid}.txt'
        if devicectl('device', 'copy', 'from', '--device', ident, '--domain-type', 'appDataContainer', '--domain-identifier',
                     bundle, '--source', 'Documents/shader-list-contribution.txt', '--destination', str(path)) is None:
            continue
        rows = contribution_rows(path.read_text(errors='replace'))
        reminded = reminded_at(state.read_text() if state.exists() else '')
        print(f'{time.strftime("%Y-%m-%d %H:%M:%S")} {name}: {rows} rows (reminded at {reminded})', flush=True)
        if due(rows, reminded):
            state.write_text(f'{rows}\n')   # "next time": STEP rows later
            if ask(rows, name or 'iPhone', path) == 'never':
                state.write_text('never\n')


def agent_plist(python, script, developer_dir):
    return {'Label': LABEL, 'ProgramArguments': [python, str(script), 'check'], 'StartInterval': CHECK_EVERY,
            'RunAtLoad': False, 'ProcessType': 'Background', 'EnvironmentVariables': {'DEVELOPER_DIR': developer_dir},
            'StandardOutPath': str(LOG), 'StandardErrorPath': str(LOG)}


def install():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from xcode_dir import developer_dir   # devicectl comes with the Xcode that built the iOS app
    script = DATA / 'tools' / Path(__file__).name   # a copy: the agent keeps working when the repository moves
    script.parent.mkdir(parents=True, exist_ok=True); LOG.parent.mkdir(parents=True, exist_ok=True); AGENT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(__file__, script)
    python = '/usr/bin/python3' if Path('/usr/bin/python3').exists() else sys.executable   # not a work folder's venv, which may go
    AGENT.write_bytes(plistlib.dumps(agent_plist(python, script, developer_dir())))
    domain = f'gui/{os.getuid()}'
    subprocess.run(['launchctl', 'bootout', f'{domain}/{LABEL}'], capture_output=True)
    subprocess.run(['launchctl', 'bootstrap', domain, str(AGENT)], check=True, capture_output=True)
    print(f'installed {AGENT}: checks connected iPhones / iPads every {CHECK_EVERY // 60} minutes')


def uninstall():
    subprocess.run(['launchctl', 'bootout', f'gui/{os.getuid()}/{LABEL}'], capture_output=True)
    AGENT.unlink(missing_ok=True)
    (DATA / 'tools' / Path(__file__).name).unlink(missing_ok=True)
    print(f'removed {AGENT}')


if __name__ == '__main__':
    command = sys.argv[1] if len(sys.argv) == 2 else ''
    if command not in ('check', 'install', 'uninstall'):
        sys.exit(__doc__)
    {'check': check, 'install': install, 'uninstall': uninstall}[command]()

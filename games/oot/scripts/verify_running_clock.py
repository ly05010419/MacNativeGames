#!/usr/bin/env python3
"""Measure HUD world time in an existing, isolated Codex Mac test session.

Does not launch, move, pause, save or close the game. Requires the exact live
game-slot identity and stops if real user input is detected. HUD time advances
in five-minute steps, hence a minimum two-minute observation window. Stay
below one 24-minute game day so the two clock readings are unambiguous.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/dev'))
from game_slot import pid_start


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--slot', type=Path, required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--seconds', type=float, default=120)
    args = parser.parse_args()
    if not re.fullmatch('[A-Za-z0-9_-]+', args.prefix) or not 120 <= args.seconds <= 1200:
        parser.error('requires a simple prefix and 120..1200 seconds (less than one game day)')
    session = args.session.resolve()
    pid = int((session / 'pid').read_text())
    started = pid_start(pid)
    report_path = session / (args.prefix + '-clock.json')
    if report_path.exists():
        parser.error('report prefix already exists')
    report = dict(status='running', pid=pid, samples=[])
    input_log = (session / 'game.log').open('rb')
    log_tail = b''

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def check():
        nonlocal log_tail
        os.kill(pid, 0)
        holder = dict(line.split('=', 1) for line in (args.slot / 'holder').read_text().splitlines() if '=' in line)
        if not started or pid_start(pid) != started or holder.get('owner') != 'codex' or int(holder.get('game_pid', 0)) != pid or int(holder.get('game_start', 0)) != started:
            raise RuntimeError('the exact Codex test game no longer owns the slot')
        recent = log_tail + input_log.read()
        log_tail = recent[-32:]
        if b'[input] user input' in recent:
            raise RuntimeError('real user took over the test; measurement stopped without closing it')

    def capture(index):
        check()
        path = session / f'{args.prefix}-clock-{index}.png'
        if path.exists():
            raise RuntimeError('screenshot already exists')
        sent = time.monotonic()
        with (session / 'input.txt').open('a') as commands:
            # The fixed 2K swapchain can contain a top-left 1080p crop. The
            # presented window keeps the HUD at the OCR tool's normalized ROI.
            commands.write(f'wshot {path}\n')
        deadline = sent + 15
        text = ''
        while time.monotonic() < deadline:
            check()
            if path.exists():
                run = subprocess.run([str(ROOT / 'build/read-game-clock'), str(path)], capture_output=True, text=True)
                text = run.stdout
                match = re.search(r'\b(\d{1,2}):(\d{2})\b', text)
                if run.returncode == 0 and match and int(match[1]) < 24 and int(match[2]) < 60:
                    sample = dict(sent=sent, received=time.monotonic(), clock=match[0],
                                  minutes=int(match[1]) * 60 + int(match[2]), screenshot=str(path))
                    report['samples'].append(sample)
                    save()
                    print(json.dumps(sample), flush=True)
                    return sample
            time.sleep(0.1)
        raise RuntimeError('HUD clock not recognized: ' + text)

    save()
    try:
        first = capture(0)
        until = time.monotonic() + args.seconds
        while time.monotonic() < until:
            check()
            time.sleep(min(0.5, max(0, until - time.monotonic())))
        last = capture(1)
        wall = last['received'] - first['received']
        elapsed = (last['minutes'] - first['minutes']) % 1440
        error = abs(elapsed - wall) / wall
        report.update(status='pass' if error <= 0.05 else 'fail', wall_seconds=wall,
                      game_minutes=elapsed, relative_error=error,
                      scope='world HUD advances one game minute per real second; does not validate every animation or cutscene')
    except BaseException as error:
        report.update(status='interrupted', error=str(error))
        raise
    finally:
        save()
        input_log.close()
    print(json.dumps(report), flush=True)
    return report['status'] != 'pass'


if __name__ == '__main__':
    raise SystemExit(main())

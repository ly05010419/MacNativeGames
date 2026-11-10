#!/usr/bin/env python3
"""Observe one already-loaded private Mac test session, with bounded diagnostics.

Does not launch the game or modify saves. Requires a test-owned slot and stops
control on real input, process replacement, log rotation, or lost slot ownership.
The game remains open for inspection when this observer finishes or fails.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--test-root', type=Path, required=True)
    parser.add_argument('--duration', type=float, default=4*3600)
    parser.add_argument('--sample', type=float, default=60)
    parser.add_argument('--capture', type=float, default=600)
    parser.add_argument('--picture', type=float, default=900)
    parser.add_argument('--interpolation', type=float, default=3600)
    parser.add_argument('--initial-interpolation', choices=['off', '30+30', 'half-refresh', 'unlimited'], default='off',
                        help='already configured at launch; observer records requests, not detected mode')
    args = parser.parse_args()
    if min(args.duration, args.sample, args.capture, args.picture, args.interpolation) <= 0:
        parser.error('intervals and duration must be positive')
    root = args.test_root.expanduser().resolve()
    assert not (root/'soak-report.json').exists(), 'use a new directory; previous evidence is preserved'
    launch = json.loads((root/'launch.json').read_text())
    env = launch.get('env', launch.get('selected_env', {}))
    assert env['TOTK_SLOT_OWNER'] == 'codex', 'private test ownership required'
    assert Path(env['TOTK_PROFILE_DIR']).resolve() == root/'profile'
    assert not env.get('DM_NVN_TSV'), 'disable per-frame TSV before long sessions'
    commands = root/'input.txt'
    assert Path(env['TOTK_INPUT_FILE']).resolve() == commands
    slot = Path.home()/'Library/Application Support/MacNativeGames/TOTK/game-slot'
    log = Path.home()/'Library/Logs/MacNativeGames/totk.log'
    identity = log.stat().st_ino

    def holder():
        return dict(line.split('=', 1) for line in (slot/'holder').read_text().splitlines() if '=' in line)

    original = holder()
    assert original['owner'] == 'codex' and int(original['game_pid']) == args.pid
    assert original['cwd'].endswith('.app/Contents/Resources/totk')
    info = plistlib.loads((Path(original['cwd']).parents[1]/'Info.plist').read_bytes())
    assert info['CFBundleShortVersionString'] == '1.4.3'
    with Path(original['exe']).open('rb') as executable:
        runtime_sha256 = hashlib.file_digest(executable, 'sha256').hexdigest()
    started = time.monotonic()
    report = dict(status='running', pid=args.pid, started=time.time(), duration_requested=args.duration,
                  game_start=original['game_start'], token=original['token'], samples=[], actions=[],
                  runtime_sha256=runtime_sha256, display_version=info['CFBundleShortVersionString'],
                  screenshots=[], suspicious_log_lines=[], preexisting_diagnostic_lines=[], audio_reports=0,
                  frame_counter_granularity=300,
                  scope='one unpaused scene, periodic camera/picture/interpolation changes; not full-game or fair version performance comparison')
    latest_frame = 0
    first_frame = None
    frame_changed = started
    audio = None
    pending = b''
    initial_scan = True
    next_sample = next_capture = 0
    next_picture, next_interp = args.picture, args.interpolation
    picture_index = 0
    pictures = ['off', 'MetalFX', 'native-2K-canvas']
    interps = ['off', '30+30', 'half-refresh', 'unlimited']
    interp_index = interps.index(args.initial_interpolation)

    def save():
        report['elapsed_seconds'] = time.monotonic()-started
        temporary = root/'soak-report.json.tmp'
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
        temporary.replace(root/'soak-report.json')

    def guard():
        os.kill(args.pid, 0)
        current = holder()
        if any(current.get(k) != original[k] for k in ('owner', 'game_pid', 'game_start', 'token')):
            raise RuntimeError('slot changed; all scripted control stopped')
        if log.stat().st_ino != identity:
            raise RuntimeError('log rotated; all scripted control stopped')

    def send(text):
        guard()
        with commands.open('a') as stream:
            stream.write(text+'\n')
        report['actions'].append(dict(elapsed=time.monotonic()-started, command=text))

    with log.open('rb') as stream:
        try:
            while True:
                guard()
                pending += stream.read()
                lines = pending.split(b'\n')
                pending = lines.pop()
                for raw in lines:
                    line = raw.decode('utf-8', errors='replace')
                    if '[input] user input' in line:
                        raise RuntimeError('real user input; game left open, scripted control stopped')
                    match = re.search(r'\[nvn\] 帧 (\d+)：\d+ 个函数', line)
                    if match and int(match[1]) > latest_frame:
                        latest_frame = int(match[1])
                        frame_changed = time.monotonic()
                    match = re.search(r'output level: rms ([\d.]+) peak ([\d.]+).*underruns (\d+), overruns (\d+)', line)
                    if match:
                        audio = dict(rms=float(match[1]), peak=float(match[2]),
                                     underruns=int(match[3]), overruns=int(match[4]))
                        report['audio_reports'] += 1
                    if re.search(r'GPU fault|command buffer.*(?:failed|error)|integrity mismatch|\[fatal\]|runtime_fail|EXC_BAD_ACCESS|【红】', line, re.I):
                        field = 'preexisting_diagnostic_lines' if initial_scan else 'suspicious_log_lines'
                        if len(report[field]) < 200:
                            report[field].append(line)
                initial_scan = False
                if first_frame is None and latest_frame:
                    first_frame = latest_frame
                elapsed = time.monotonic()-started
                if elapsed >= next_sample:
                    ps = subprocess.run(['ps', '-p', str(args.pid), '-o', 'rss=,%cpu=,stat='],
                                        capture_output=True, text=True, check=True).stdout.split()
                    if 'Z' in ps[2]:
                        raise RuntimeError('game is a zombie process')
                    row = dict(elapsed=elapsed, frame=latest_frame, rss_kib=int(ps[0]),
                               cpu_percent=float(ps[1]), audio=audio,
                               picture=pictures[picture_index], interpolation=interps[interp_index],
                               frame_progress_age_seconds=time.monotonic()-frame_changed)
                    report['samples'].append(row)
                    save()
                    print(json.dumps(row, ensure_ascii=False), flush=True)
                    next_sample += args.sample
                if elapsed >= args.duration:
                    break
                if time.monotonic()-frame_changed > 90:
                    raise RuntimeError('no periodic frame progress for 90 seconds; game left open for inspection')
                if elapsed >= next_capture:
                    shot = root/f'soak-{round(elapsed):05d}.png'
                    send('wshot '+str(shot))
                    report['screenshots'].append(dict(elapsed=elapsed, path=str(shot)))
                    if next_capture:
                        # Short alternating turns exercise visibility without moving the player.
                        send('key '+('124' if len(report['screenshots']) % 2 else '123')+' 250')
                    next_capture += args.capture
                if elapsed >= next_picture:
                    send('key 20 150')
                    picture_index = (picture_index+1) % len(pictures)
                    next_picture += args.picture
                if elapsed >= next_interp:
                    send('key 19 150')
                    interp_index = (interp_index+1) % len(interps)
                    next_interp += args.interpolation
                time.sleep(1)
            report.update(status='finished-awaiting-visual-review', ended=time.time(),
                          first_frame=first_frame, last_frame=latest_frame, game_left_open=True)
        except Exception as error:
            report.update(status='stopped-for-inspection', ended=time.time(), error=str(error), game_left_open=True)
            raise
        finally:
            save()


if __name__ == '__main__':
    main()

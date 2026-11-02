#!/usr/bin/env python3
"""Measure how long a first session stops for shaders: one fixed route on a copy of your data with an empty shader cache.

    python3 games/kirby/scripts/measure_shader_stalls.py --app Kirby.app --data "Kirby Data" --out <new folder>
    python3 games/kirby/scripts/measure_shader_stalls.py --summary <folder>/data/last-run.log

The copy keeps the save (profile/save) and nothing the game learned about shaders (no shader-cache, no pso_user.tsv), so
the run is the first session of a new install. The route: 30 s after the first frame, A on the title (Continue, the
newest save); 10 s later Kirby moves forward for 60 s, jumping and floating every half second; then the game is
closed. The same inputs at the same times on every run, so runs of two builds compare. The summary reads the runtime's own lines in last-run.log:
 - [长帧] (long frame): every frame over the frame budget, with the time spent creating pipelines and shader libraries
   on the submit thread in that frame (nvn_mtl.m mtl_create_ns);
 - 着色器库：预编译加载 a 个，现编 b 个 (shader libraries: a loaded precompiled, b compiled from source), logged at the
   1st, 201st, ... library, so the counts are those of the last such line;
 - [shader] translated n: shaders the translator had to translate in play (logged for the first 8 and every 100th, so
   the count is a floor: 8 means 8 to 99).
macOS keeps its own cache of compiled Metal code (getconf DARWIN_USER_CACHE_DIR, com.apple.metal), shared by every app:
a shader this Mac compiled before costs less again. Compare runs on the same state of that cache.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

LONG = re.compile(r'\[长帧\] 帧 (\d+)（([\d.]+) s）整帧 ([\d.]+) ms.*?新建管线 (\d+) 条 \+ 着色器库 (\d+) 个共 ([\d.]+) ms')
LIBS = re.compile(r'着色器库：预编译加载 (\d+) 个，现编 (\d+) 个')
TRANSLATED = re.compile(r'\[shader\] translated (\d+)')
FLOAT = 60   # seconds of W (Mac key code 13, the left stick up) held with J (38, A) tapped


def summary(log):
    text = Path(log).read_text(errors='replace')
    frames = [tuple(map(float, m.groups())) for m in LONG.finditer(text)]
    libs = LIBS.findall(text)
    translated = TRANSLATED.findall(text)
    stalls = [f for f in frames if f[2] >= 100]
    out = {
        'long frames': len(frames),
        'time in long frames (s)': round(sum(f[2] for f in frames) / 1000, 2),
        'frames over 100 ms': len(stalls),
        'time in frames over 100 ms (s)': round(sum(f[2] for f in stalls) / 1000, 2),
        'longest frame (ms)': max((f[2] for f in frames), default=0),
        'pipeline + library creation in long frames (s)': round(sum(f[5] for f in frames) / 1000, 2),
        'pipelines / libraries created in long frames': (int(sum(f[3] for f in frames)), int(sum(f[4] for f in frames))),
        'libraries precompiled / compiled (last count line)': tuple(map(int, libs[-1])) if libs else None,
        'shaders translated in play (last count line)': int(translated[-1]) if translated else 0,
    }
    for name, value in out.items():
        print(f'{name}: {value}')
    return out


def wait_for(path, text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if path.is_file() and text in path.read_text(errors='replace'):
            return
        time.sleep(1)
    raise SystemExit(f'{text!r} did not appear in {path} within {timeout} s')


def run(app, data, out):
    if out.exists():
        raise SystemExit(f'{out} exists: choose a new folder')
    copy = out / 'data'
    shutil.copytree(data / 'profile', copy / 'profile', ignore=shutil.ignore_patterns('pso_user.tsv', 'cache', 'temp'))
    (copy / 'shader-cache').mkdir()
    launcher = app / 'Contents/MacOS/Kirby'
    with (out / 'launcher.out').open('w') as f:
        game = subprocess.Popen([str(launcher)], env=dict(os.environ, KIRBY_DATA_DIR=str(copy)), stdout=f, stderr=subprocess.STDOUT)
    log, commands = copy / 'last-run.log', copy / 'commands.txt'
    wait_for(log, 'first presented frame', 120)
    time.sleep(30)

    def send(line):
        with commands.open('a') as f:
            f.write(line + '\n')
    send(f'wshot {out / "title.png"}')
    send('key 38 150')   # A: Continue (the title selects the newest save)
    time.sleep(10)
    send(f'wshot {out / "start.png"}')
    send('down 13')      # forward, jumping and floating every half second
    for _ in range(FLOAT * 2):
        send('key 38 200')
        time.sleep(0.5)
    send('up 13')
    send(f'wshot {out / "end.png"}')
    time.sleep(3)
    send('close')
    game.wait(timeout=120)
    return summary(log)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--app', type=Path, help='the Kirby.app to measure')
    parser.add_argument('--data', type=Path, help='a "Kirby Data" folder whose save the run starts from (only read)')
    parser.add_argument('--out', type=Path, help='a new folder for the copy, its log and two screenshots')
    parser.add_argument('--summary', type=Path, help='only summarize this last-run.log')
    args = parser.parse_args(argv)
    if args.summary:
        summary(args.summary)
    elif args.app and args.data and args.out:
        run(args.app.resolve(), args.data.resolve(), args.out.resolve())
    else:
        parser.error('give --app, --data and --out, or --summary')


if __name__ == '__main__':
    sys.exit(main())

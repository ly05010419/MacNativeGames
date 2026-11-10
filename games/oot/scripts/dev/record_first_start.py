#!/usr/bin/env python3
"""Record the shaders and render pipelines of a first start on a fresh profile, for the shared shader list.

The packaged App warms the pipelines games/oot/assets/shader_list.tsv names; everything else is built in play. The rows
so far came from play in the world (the village, the forest). A player's first start runs through the title, the file
select, the opening and Link's house first, and none of that was recorded. This script runs the App the way a new player
does (an empty profile, an empty runtime shader cache, its own contribution file), drives it with the input file, and
leaves the recordings in the run folder: `profile/pso_user.tsv` (S / C / R / T rows, this build's names), the
contribution file (V rows with bind states, portable R / T rows) and the runtime cache folders (variant indexes that let
`shader_list.py merge` rename the rows). With --merge the rows go into the list at once.

The route is scripted by time, and needs no knowledge of how long loading takes: A every few seconds (J = 38, the input
script's fixed layout, runtime/input.c) passes the title and New Game and advances the opening and the dialogue in Link's
house; after WALK_FROM seconds the left stick (W = 13) and the camera (right arrow = 124) move as well, to see more of
the scene. A screenshot every 30 s shows where it got. Nothing here touches the player's profile: TOTK_PROFILE_DIR,
TOTK_SHADER_CACHE and TOTK_SHADER_CONTRIB point into the run folder, and TOTK_CLOUD_SAVE_DIR=0 keeps the launcher from
asking about save sync.

    python3.14 games/oot/scripts/dev/record_first_start.py --out <new folder outside the project> --app <the packaged .app> [--minutes 10] [--watchdog <script>] [--merge]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = HERE.parents[1]                          # games/oot
LOG = Path.home() / 'Library/Logs/MacNativeGames/ocarina.log'
KEY_A, KEY_W, KEY_RIGHT = 38, 13, 124
A_FROM, A_EVERY, SHOT_EVERY, WALK_FROM, WALK_EVERY, WALK_FOR = 30, 4, 30, 300, 20, 3   # seconds


def rows(path):
    """frames.tsv (DM_NVN_TSV): frame -> {column: value}, the last 300 KB."""
    out = {}
    if not path.exists():
        return out
    with path.open('rb') as f:
        f.seek(max(0, path.stat().st_size - 300000))
        for line in f.read().decode(errors='replace').splitlines():
            try:
                k, n, v = line.split('\t'); out.setdefault(int(k), {})[n] = float(v)
            except ValueError:
                continue
    return out


def count_rows(path):
    counts = {}
    if path.is_file():
        for line in path.read_text(errors='replace').splitlines():
            if line and not line.startswith('#'):
                counts[line[0]] = counts.get(line[0], 0) + 1
    return counts


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, required=True, help='the run folder, which must not exist yet; keep it outside the project')
    p.add_argument('--app', type=Path, required=True, help='the packaged App (its Contents/MacOS executable is run)')
    p.add_argument('--minutes', type=float, default=10.0, help='when to close')
    p.add_argument('--watchdog', type=Path, help='a script run as <script> <pid> <input file> <activity file>: closes a game with no activity')
    p.add_argument('--merge', action='store_true', help='merge the recordings into games/oot/assets/shader_list.tsv')
    a = p.parse_args()
    exe = next((a.app / 'Contents/MacOS').iterdir())
    seed = a.app / 'Contents/Helpers/ShaderData-oot.bundle/Contents/Resources/shader-seed'
    D = a.out
    D.mkdir(parents=True, exist_ok=False)
    profile, cache, shots = D / 'profile', D / 'shader-cache', D / 'shots'
    for d in (profile, cache, shots):
        d.mkdir()
    I, F, contrib, activity = D / 'input.txt', D / 'frames.tsv', D / 'shader-list-contribution.txt', D / 'activity'
    sys.path.insert(0, str(HERE))
    from game_slot import GameSlot
    os.environ.setdefault('TOTK_GAME_SLOT_DIR', str(Path.home() / 'Library/Application Support/MacNativeGames/TOTK/game-slot'))
    result = dict(out=str(D), app=str(a.app), binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), sent=[])

    def send(cmd):
        with I.open('a') as f:
            f.write(cmd + '\n')

    with GameSlot(owner='claude-oot', purpose=f'OOT first-start recording ({D.name})', expect_seconds=int(a.minutes * 60) + 120) as slot:
        env = slot.env({k: v for k, v in os.environ.items() if not k.startswith(('OOT_', 'TOTK_', 'DM_'))})
        env.update(TMPDIR=subprocess.check_output(['getconf', 'DARWIN_USER_TEMP_DIR'], text=True).strip(),
                   TOTK_PROFILE_DIR=str(profile), TOTK_SHADER_CACHE=str(cache / 'rt'), TOTK_SHADER_CONTRIB=str(contrib),
                   TOTK_INPUT_FILE=str(I), TOTK_IGNORE_USER_INPUT='1', TOTK_ASSUME_FOCUS='1', TOTK_CLOUD_SAVE_DIR='0',
                   TOTK_DEBUG_LOG='1', TOTK_INPUT_LOG='1', DM_NVN_TSV=str(F), DM_NVN_TSV_CALLS='0', TOTK_FILE_HEAP='0')
        result['env'] = {k: v for k, v in env.items() if k.startswith(('OOT_', 'TOTK_', 'DM_'))}
        I.write_text('')
        if LOG.exists():
            LOG.write_text('')
        activity.touch()
        start = time.monotonic(); closed = False; last_frame, last_progress = -1, start
        next_a, next_shot, next_walk, walking_until = A_FROM, A_FROM, WALK_FROM, 0.0
        with (D / 'launcher.log').open('w') as out:
            proc = slot.popen([str(exe)], env=env, cwd=GAME, stdout=out, stderr=subprocess.STDOUT)
            result['pid'] = proc.pid; print(D.name, 'pid', proc.pid, flush=True)
            if a.watchdog:
                subprocess.Popen([str(a.watchdog), str(proc.pid), str(I), str(activity)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while proc.poll() is None:
                e = time.monotonic() - start
                frame = max(rows(F), default=-1)
                if frame != last_frame:
                    last_frame, last_progress = frame, time.monotonic(); activity.touch()
                if not closed:
                    if e >= next_a:
                        next_a = e + A_EVERY; send(f'key {KEY_A} 150')
                    if e >= next_shot:
                        next_shot = e + SHOT_EVERY; send(f'wshot {shots / f"{int(e):04d}s.png"}'); result['sent'].append((round(e, 1), 'shot', frame)); print(f'{e:6.1f}s F{frame}', flush=True)
                    if e >= next_walk:
                        next_walk = e + WALK_EVERY; walking_until = e + WALK_FOR
                        send(f'down {KEY_W}'); send(f'down {KEY_RIGHT}'); result['sent'].append((round(e, 1), 'walk', frame))
                    if walking_until and e >= walking_until:
                        walking_until = 0.0; send(f'up {KEY_W}'); send(f'up {KEY_RIGHT}')
                    stalled = e > 120 and time.monotonic() - last_progress > 150
                    if e >= a.minutes * 60 or stalled:
                        reason = 'no frame progress for 150 s' if stalled else 'time'
                        send('close'); closed = True; result['closed'] = dict(at=round(e, 1), frame=frame, reason=reason); print('close:', reason, flush=True)
                elif time.monotonic() - start > a.minutes * 60 + 90:
                    proc.terminate(); result['terminated'] = True
                time.sleep(0.5)
            result['exit'] = proc.returncode
    if LOG.exists():
        shutil.copy2(LOG, D / 'game.log')
    result['pso_user'] = count_rows(profile / 'pso_user.tsv'); result['contribution'] = count_rows(contrib)
    result['cache_dirs'] = sorted(d.name for d in cache.iterdir() if d.is_dir()) if cache.exists() else []
    print('recorded: pso_user', result['pso_user'], 'contribution', result['contribution'], flush=True)
    if a.merge:
        lst = GAME / 'assets/shader_list.tsv'
        caches = [str(d) for d in cache.iterdir() if d.is_dir() and any(d.glob('*.variants'))]
        cmd = [sys.executable, '-I', str(GAME / 'scripts/shader_list.py'), 'merge', str(lst), *caches, str(seed), str(contrib), str(profile / 'pso_user.tsv')]
        merged = subprocess.run(cmd, capture_output=True, text=True)
        result['merge'] = dict(returncode=merged.returncode, out=merged.stdout.strip()[-800:], err=merged.stderr.strip()[-800:])
        print('merge:', merged.stdout.strip().splitlines()[-1:] or merged.stderr.strip()[-300:], flush=True)
    (D / 'result.json').write_text(json.dumps(result, indent=1, ensure_ascii=False) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())

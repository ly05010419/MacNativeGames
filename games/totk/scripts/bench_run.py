#!/usr/bin/env python3
"""bench_run.py: the NX Optimizer benchmark on the built 1.4.3 app: the camera flies the UltraCam benchmark route.

NX Optimizer's benchmark (UltraCam, its in-game mod) puts Link at each scene's start and flies the camera along that
scene's keyframes: Kakariko, Lookout Landing, Great Sky Island, Korok Forest, Goron City, the Depths and Zora's Domain.
This runs the same route: an agent test session (a copy of the player's profile and shader cache, the player's game
slot shared so a game the player starts goes first) loads the newest save, then for each scene holds Link at its start
while the world loads and flies the camera through the keyframes (survey route, world_survey.c, through the look-at
camera override the 1.4.3 patches install in survey sessions). Every game frame's time is recorded (TOTK_BENCH_FRAMES);
the report gives, like NX Optimizer, each scene's frames, average FPS and 1% / 0.1% lows, plus the worst frame and the
long frames with their cause from the game's own long-frame log.

  bench_run.py --app "<Tears of the Kingdom.app>" [--binary <game binary>] [--out <dir>] [--scenes Kakariko,Depths]

The route is read from ultracam-bench-route.json in the work folder (taken from the player's own NX Optimizer UltraCam
files; MaxLastBreath's data, so it is not kept in the repository)."""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path

DATA = Path.home() / 'Library/Application Support/MacNativeGames/TOTK'
def _chain():
    """The shader cache folder of the current translation chain: rt-<DMSH_CHAIN> (dmsh_chain.h), so a new chain needs no
    change here."""
    header = Path(__file__).resolve().parents[1] / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
    m = re.search(r'#define\s+DMSH_CHAIN\s+"([^"]+)"', header.read_text())
    if not m: raise SystemExit(f'no DMSH_CHAIN in {header}')
    return 'rt-' + m.group(1)
CHAIN = _chain()
# The save the benchmark loads (--save, or TOTK_BENCH_SAVE): one slot folder (progress.sav inside), installed as the only
# slot, slot_00, before every run, so the time of day and the world state match from run to run. It must be past the
# Great Sky Island tutorial; before it, leaving the island fades to white and the game pulls Link back.
# Each scene's time of day as UltraCam sets it at the scene's start (its scene table, subsdk3 bench scene +0x2c).
SCENE_HOUR = {'Kakariko': 12, 'Lookout Landing': 11, 'Great Sky Island': 20, 'Korok Forest': 3, 'Goron City': 20,
              'Depths': 20, 'Zora Domain': 15}
# UltraCam's "All" benchmark stops after its sixth scene (an off-by-one in mode 4), so Zora's Domain never plays there:
# the FullBenchmark results cover the first six scenes, 323 s of flight. --scenes all adds Zora's Domain.
ALL_SCENES = 6


def find_clock(s, attempts=8):
    """survey clockfind until the group found holds the game's own clock (a value 0xd4428 into a 1 MiB-aligned heap
    block in 1.2.1, world_survey.c): other timers of the same rate give a wrong sky; one scan in ~3 found one of them
    (2026-10-24: 11669 s / 0 values instead of ~24800 s in p-pp-* runs, pose 040 at noon instead of 20:00). The last
    group found stays set if none of the attempts finds the game's clock (reported)."""
    found = None
    for attempt in range(attempts):
        s.send('survey clockfind')
        addrs = []
        while True:
            line = s.wait(r'\[survey\] (clock value at|time of day: )', 90)
            if not line or 'time of day: ' in line: break
            addrs.append(int(line.split(' at ')[1].split()[0], 16))
        found = line
        if any((a & 0xfffff) == 0xd4428 for a in addrs):
            return found
        print(f'clock: attempt {attempt + 1} found {len(addrs)} values without the game clock; again', flush=True)
        time.sleep(2)
    print('clock: WARNING the game clock was not identified; scene hours may be wrong', flush=True)
    return found


class Session:
    def __init__(self, app, binary, out, env_extra, save=None, cache=None):
        self.out = out
        self.input = out / 'input.txt'
        self.log = out / 'game.log'
        self.frames = out / 'frames.txt'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        shutil.copytree(DATA / 'profile', out / 'profile', symlinks=True)
        if save:   # this session's profile copy gets that one slot as slot_00, every other slot removed (as for a citron run)
            for user in (out / 'profile/save').iterdir():
                if user.is_dir() and any(user.glob('slot_*')):
                    for slot in user.glob('slot_*'):
                        shutil.rmtree(slot)
                    shutil.copytree(save, user / 'slot_00')
        if cache:   # a benchmark-only cache kept between runs (warmed once), used in place
            Path(cache).mkdir(parents=True, exist_ok=True)
            cache_dir = Path(cache)
        else:       # a copy of the player's own cache
            subprocess.run(['cp', '-Rc', str(DATA / 'shader-cache' / CHAIN), str(out / 'cache')], check=True)
            cache_dir = out / 'cache'
        self.input.write_text('')
        root = Path(app) / 'Contents/Resources/totk'
        env = dict(os.environ)
        env.update(TOTK_PROFILE_DIR=str(out / 'profile'), TOTK_SHADER_CACHE=str(cache_dir),
                   TOTK_GAME_SLOT_DIR=str(DATA / 'game-slot'), TOTK_ROMFS_PROOF_DIR=str(DATA),
                   TOTK_SHARED_FONTS='extracted/firmware/shared-fonts', TOTK_INPUT_FILE=str(self.input),
                   TOTK_SHADER_CONTRIB=str(out / 'contrib.txt'), TOTK_SLOT_OWNER='claude', TOTK_SLOT_PURPOSE='benchmark',
                   TOTK_AUTOLOAD='1', TOTK_SURVEY='1', TOTK_BENCH_FRAMES=str(self.frames),
                   TOTK_GPU_ALLOW_SUBMIT='1', TOTK_GPU_WIDEN='1', TOTK_GPU_BLOCKRT='1', TOTK_GPU_MIDCLEAR='1',
                   # UltraCam's [Resolution] in the citron runs: the scene rendered at 1920x1080, shown at 1920x1080
                   TOTK_RENDER_RES='1920x1080', TOTK_OUTPUT_RES='1920x1080')
        env.update(env_extra)
        version = next(p.name for p in sorted((root / 'rewritten').iterdir()) if p.is_dir())   # the app's one version
        self.proc = subprocess.Popen([str(binary or root / 'build/totk-native'), f'rewritten/{version}', 'boot'], cwd=root, env=env,
                                     stdout=open(self.log, 'w'), stderr=subprocess.STDOUT)
        self.pos = 0

    def send(self, line):
        with open(self.input, 'a') as f:
            f.write(line + '\n')

    def wait(self, pattern, timeout):
        """The first log line from here on that matches; None after timeout or when the game exits."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                return None
            with open(self.log, errors='replace') as f:
                f.seek(self.pos)
                chunk = f.read()
            for line in chunk.splitlines(keepends=True):
                self.pos += len(line.encode(errors='replace'))
                if not line.endswith('\n'):
                    self.pos -= len(line.encode(errors='replace'))
                    break
                if rx.search(line):
                    return line
                if '[survey] cancelled: user input' in line:
                    raise SystemExit('stopped: a key, the mouse or a controller was used in the game window '
                                     '(the session gives way to the player); leave it alone while the benchmark runs')
            time.sleep(0.2)
        return None

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def survey_frame(line):
    m = re.search(r'frame=(\d+)', line or '')
    return int(m.group(1)) if m else None


def run(args):
    work = Path(args.app).resolve().parent / 'TOTK_build/totk/build'
    scenes = json.loads(Path(args.route_file or work / 'ultracam-bench-route.json').read_text())
    if args.poses:   # [{"pose": id, "scene": name, "t": seconds}, ...]: one held camera frame each, screenshots only
        poses = json.loads(Path(args.poses).read_text())
        scenes = [dict(sc, poses=sorted((p for p in poses if p['scene'] == sc['name']), key=lambda p: p['t']))
                  for sc in scenes]
        scenes = [sc for sc in scenes if sc['poses']]
    elif args.scenes and args.scenes != 'all':
        want = [w.strip().lower() for w in args.scenes.split(',')]
        scenes = [sc for sc in scenes if any(w in sc['name'].lower() for w in want)]
    elif not args.scenes:
        scenes = scenes[:ALL_SCENES]
    out = Path(args.out).resolve()
    chosen = args.save or os.environ.get('TOTK_BENCH_SAVE')
    if not chosen:
        raise SystemExit('which save? --save <slot folder with progress.sav> (or TOTK_BENCH_SAVE), or --save own for the player\'s own')
    save = None if chosen == 'own' else Path(chosen).expanduser()
    if save and not (save / 'progress.sav').is_file():
        raise SystemExit(f'no benchmark save at {save}: --save <slot folder with progress.sav> (or --save own for the player\'s own)')
    print('save:', save or "the player's own", flush=True)
    s = Session(args.app, args.binary, out, dict(e.split('=', 1) for e in args.env), save, args.cache)
    segments = []
    try:
        if not s.wait(r'benchmark camera: look-at', 120):
            raise SystemExit('the game has no benchmark camera (a 1.2.1 or 1.4.3 build with TOTK_SURVEY=1 is needed; see game.log)')
        if not s.wait(r'\[autoload\] load started', 1800):   # a first start warms the shader seed (minutes)
            raise SystemExit('the save did not load')
        for _ in range(300):   # the player actor appears once the world is in (at 0, -0.15, 0 while the save still loads)
            s.send('survey pos start')
            line = s.wait(r'\[survey\] tag=start status=(position|no_player)', 10)
            if line and 'status=position' in line and 'xyz=0.000000,-0.150390,0.000000' not in line:
                break
            time.sleep(1)
        else:
            raise SystemExit('the player never appeared in the world')
        t_in = time.time()
        for c in args.cmd:   # extra input-file commands for the session (e.g. "flag <name> 1" for an A/B)
            s.send(c)
        time.sleep(3)
        found = find_clock(s)
        print('clock:', found.strip() if found else 'not found', flush=True)
        time.sleep(max(0.0, args.start_delay - (time.time() - t_in)))
        for sc in scenes:
            tag = re.sub(r'[^A-Za-z0-9_-]', '_', sc['name'])
            x, y, z = sc['player_start']
            path = out / f'route-{tag}.txt'
            rows = [f'start {x} {y} {z}', f"hour {SCENE_HOUR[sc['name']]}"] + [f"freeze {p['t']}" for p in sc.get('poses', [])]
            for k in sc['keyframes']:
                rows.append('k {} {} {} {} {} {} {} {} {} {} {}'.format(k['time'], *k['pos'], *k['forward'], *k['up'], k['fov']))
            path.write_text('\n'.join(rows) + '\n')
            s.send(f'survey route {tag} {path}')
            begin = s.wait(rf'\[survey\] tag={tag} status=(route_begin|busy|disabled|no_camera|bad_route|no_player|invalid_rotation)', 30)
            if not begin or 'route_begin' not in begin:
                raise SystemExit(f"{sc['name']}: route not accepted: {begin}")
            measure = s.wait(rf'\[survey\] tag={tag} status=(route_measure|route_done|no_player|invalid_rotation)', 120)
            if not measure or 'route_measure' not in measure:
                raise SystemExit(f"{sc['name']}: the camera never started (Link did not land at the start?): {measure}")
            t0 = time.time()
            if 'poses' in sc:   # screenshots for the comparison: the camera holds at each t, the world settles 3 s first
                for i, p in enumerate(sc['poses']):
                    held = s.wait(rf'\[survey\] tag={tag} status=(route_frozen pose={i} |route_done|no_player)', 120)
                    if not held or 'route_frozen' not in held:
                        raise SystemExit(f"{sc['name']}: never held at {p['t']} s: {held}")
                    s.send(f"wshot {out}/pose-{p['pose']}.png")
                s.wait(rf'\[survey\] tag={tag} status=(route_done|no_player)', 120)
                print(f"  {sc['name']}: {len(sc['poses'])} poses", flush=True)
                continue
            ts = [k['time'] for k in sc['keyframes']]
            length = ts[-2] + 1.5 * (ts[-1] - ts[-2])
            end = s.wait(rf'\[survey\] tag={tag} status=(route_done|no_player|invalid_rotation)', length + 60)
            if not end or 'route_done' not in end:
                raise SystemExit(f"{sc['name']}: route did not finish: {end}")
            segments.append(dict(name=sc['name'], begin=survey_frame(measure), end=survey_frame(end), wall=t0))
            print(f"  {sc['name']}: {length:.0f} s, frames {segments[-1]['begin']}..{segments[-1]['end']}", flush=True)
    finally:
        time.sleep(2)
        s.close()
    if not args.poses:
        report(out, segments)


def report(out, segments):
    times = {}
    for line in open(out / 'frames.txt'):
        f, ns = line.split()
        times[int(f)] = int(ns)
    long_frames = []
    rx = re.compile(r'\[长帧\] 帧 (\d+)（[\d.]+ s）整帧 ([\d.]+) ms，提交\+出图 ([\d.]+) ms，GPU ([\d.]+) ms；新建管线 (\d+) 条 \+ 着色器库 (\d+) 个共 ([\d.]+) ms；上传贴图 首传 (\d+) 张.*?([\d.]+) MB')
    for line in open(out / 'game.log', errors='replace'):
        m = rx.search(line)
        if m:
            g = m.groups()
            long_frames.append(dict(frame=int(g[0]), ms=float(g[1]), submit=float(g[2]), gpu=float(g[3]), pso=int(g[4]),
                                    compile=float(g[6]), new_tex=int(g[7]), mb=float(g[8])))

    def cause(r):
        if r['pso'] and r['submit'] > r['ms'] * 0.5: return 'pipeline'
        if r['mb'] > 8 or r['new_tex'] > 50: return 'streaming'
        if r['gpu'] > r['ms'] * 0.6: return 'gpu'
        return 'cpu/memory'

    rows, flight = [], []
    for seg in segments:
        fr = [f for f in range(seg['begin'] + 1, seg['end'] + 1) if f in times and f - 1 in times]
        dts = sorted((times[f] - times[f - 1]) / 1e6 for f in fr)
        if not dts:
            continue
        def low(p):   # the FPS of the slowest p of the frames
            k = max(1, int(len(dts) * p))
            return 1000 / statistics.mean(dts[-k:])
        spikes = [r for r in long_frames if seg['begin'] < r['frame'] <= seg['end'] and r['ms'] >= 50]
        causes = {}
        for r in spikes:
            causes[cause(r)] = causes.get(cause(r), 0) + 1
        flight.extend((times[f] - times[f - 1]) / 1e6 for f in fr)
        rows.append(dict(segment=seg['name'], frames=len(dts), seconds=round(sum(dts) / 1000, 1),
                         avg_fps=round(1000 / statistics.mean(dts), 1), low_1=round(low(0.01), 1), low_01=round(low(0.001), 1),
                         worst_ms=round(dts[-1], 1), spikes_50ms=len(spikes), causes=causes, dts=dts))
    if rows:   # the whole run, as NX Optimizer's total
        dts = sorted(d for r in rows for d in r['dts'])
        causes = {}
        for r in rows:
            for k, v in r['causes'].items():
                causes[k] = causes.get(k, 0) + v
        def low(p):
            k = max(1, int(len(dts) * p))
            return 1000 / statistics.mean(dts[-k:])
        rows.append(dict(segment='Total', frames=len(dts), seconds=round(sum(dts) / 1000, 1),
                         avg_fps=round(1000 / statistics.mean(dts), 1), low_1=round(low(0.01), 1), low_01=round(low(0.001), 1),
                         worst_ms=round(dts[-1], 1), spikes_50ms=sum(r['spikes_50ms'] for r in rows), causes=causes, dts=[]))
    for r in rows:
        del r['dts']
    (out / 'report.json').write_text(json.dumps(rows, indent=1) + '\n')
    # the same file UltraCam writes (sd:/UltraCam/TOTK/Benchmark/FullBenchmark.json), with its statistics (bench_compare.py)
    from bench_compare import ultracam_stats
    (out / 'FullBenchmark.json').write_text(json.dumps(dict(Name='FullBenchmark', Type='All', **ultracam_stats(flight),
                                                            Frametimes=[round(x, 2) for x in flight]), indent=4) + '\n')
    lines = ['| Scene | Frames | s | Average FPS | 1% low | 0.1% low | Worst ms | Spikes ≥50 ms | Causes |',
             '|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        c = ', '.join(f'{k} {v}' for k, v in sorted(r['causes'].items())) or '-'
        lines.append(f"| {r['segment']} | {r['frames']} | {r['seconds']} | {r['avg_fps']} | {r['low_1']} | {r['low_01']} | {r['worst_ms']} | {r['spikes_50ms']} | {c} |")
    (out / 'report.md').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--app', required=True)
    p.add_argument('--binary', help='game binary to run instead of the app\'s own (a test build)')
    p.add_argument('--out', default='bench-out')
    p.add_argument('--route-file', help='benchmark route (default: ultracam-bench-route.json in the work folder)')
    p.add_argument('--scenes', help='only these scenes, comma-separated parts of their names')
    p.add_argument('--flip', action='store_true', help='look the other way along the keyframes\' view vector')
    p.add_argument('--save', help='save slot folder to benchmark with (progress.sav inside; default: TOTK_BENCH_SAVE; own = the player\'s)')
    p.add_argument('--cmd', action='append', default=[], help='an input-file command sent once the player is in the world')
    p.add_argument('--start-delay', type=float, default=5, help='seconds from the player appearing to the first scene')
    p.add_argument('--cache', help='benchmark-only shader cache folder, used in place and kept (default: a copy of the player\'s)')
    p.add_argument('--poses', help='JSON list of {pose, scene, t}: screenshots of those camera frames (no timing report)')
    p.add_argument('--env', action='append', default=[], help='extra environment for the game, NAME=value')
    a = p.parse_args()
    if subprocess.run(['pgrep', '-q', '-f', 'build/totk-native'], check=False).returncode == 0:
        raise SystemExit('a game is running: close it first (the benchmark needs the game slot)')
    run(a)


if __name__ == '__main__':
    main()

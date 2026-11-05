#!/usr/bin/env python3
"""bench_compare.py: the recomp against citron on NX Optimizer's (UltraCam's) benchmark, frame times and pictures.

  bench_compare.py poses  --citron <citron run> --out poses.json          which camera frame each citron screenshot shows
  bench_compare.py report --citron <citron run> --recomp <bench_run out> --shots <bench_run --poses out> --out <dir>

A citron run is a folder from a benchmark run in citron (the same save installed as slot_00): FullBenchmark.json
(UltraCam's own result), ucon.log (UltraCam console replies, with Link's position every 0.25 s) and shots/<epoch>.png
(window screenshots). UltraCam plays each scene the same way (reversed from subsdk3, see world_survey.c): Link is put
5 m above the scene's start, the camera waits 10 s once he is no more than 2 m above it, then flies the scene's
keyframes in real time for T[n-2] + 1.5 (T[n-1] - T[n-2]) seconds. So each scene's flight start follows from the
console log, and each screenshot's time into its flight; the recomp then holds its camera at exactly those times
(bench_run.py --poses) for the picture comparison. The route is read from ultracam-bench-route.json in the work folder."""
import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

SCENES = 6   # UltraCam's "All" benchmark plays the first six scenes (Zora's Domain never plays)


def ultracam_stats(frametimes_ms):
    """UltraCam's FullBenchmark statistics (subsdk3 0xa1360): each frame time truncated to whole milliseconds; Time is
    their sum; Low and Lowest are 1000 over the mean of the slowest 1% / 0.1%, in integer arithmetic."""
    ms = [int(x) for x in frametimes_ms]
    n = len(ms)
    if not n:
        return dict(Total=0, Average=0, Max=0, Low=0, Lowest=0, Time=0.0)
    total = sum(ms)
    slow = sorted(ms, reverse=True)

    def low(div):
        k = n // div
        return 1000 // max(1, sum(slow[:k]) // k) if k else 0
    return dict(Total=n, Average=int(n / (total / 1000)) if total else 0, Max=1000 // max(1, min(ms)), Low=low(100),
                Lowest=low(1000) if n >= 1000 else 0, Time=round(total / 1000, 2))


def flight_length(scene):
    t = [k['time'] for k in scene['keyframes']]
    return t[-2] + 1.5 * (t[-1] - t[-2])


def split_scenes(frametimes_ms, scenes):
    """Each flight lasts its scene's length in real time, so the run's frame times split at the cumulative lengths."""
    out, i, acc = [], 0, 0.0
    for sc in scenes:
        end, part = acc + flight_length(sc) * 1000, []
        while i < len(frametimes_ms) and acc + frametimes_ms[i] <= end + 1:
            acc += frametimes_ms[i]; part.append(frametimes_ms[i]); i += 1
        acc = end
        out.append(part)
    if out and i < len(frametimes_ms):
        out[-1].extend(frametimes_ms[i:])
    return out


def citron_timeline(run, scenes):
    """Each scene's flight start (epoch seconds) from Link's positions in the console log."""
    cords, done = [], None
    for line in open(run / 'ucon.log', errors='replace'):
        m = re.match(r'([\d.]+) < Cordinates ([-\d.]+), ([-\d.]+), ([-\d.]+)', line)
        if m:
            cords.append((float(m.group(1)), *map(float, m.groups()[1:])))
        elif 'COMPLETED' in line:
            done = float(line.split()[0])
    starts, i = [], 0
    for sc in scenes:
        sx, sy, sz = sc['player_start']
        while i < len(cords) and math.dist(cords[i][1:], (sx, sy + 5, sz)) > 4:   # the teleport to start + 5 m
            i += 1
        while i < len(cords) and not (cords[i][2] <= sy + 2 and math.dist(cords[i][1:], (sx, sy, sz)) < 25):   # landed
            i += 1
        if i >= len(cords):
            raise SystemExit(f"{sc['name']}: no teleport / landing found in the console log")
        starts.append(cords[i][0] + 10)
    return starts, done


def load_route(path):
    return json.loads(Path(path).read_text())[:SCENES]


def cmd_poses(a):
    scenes = load_route(a.route)
    run = Path(a.citron)
    starts, done = citron_timeline(run, scenes)
    end_err = (starts[-1] + flight_length(scenes[-1]) - done) if done else None
    poses = []
    for shot in sorted((run / 'shots').glob('*.png'), key=lambda p: float(p.stem)):
        ts = float(shot.stem)
        for sc, st in zip(scenes, starts):
            t = ts - st
            if 0.5 <= t <= flight_length(sc) - 0.5:
                poses.append(dict(pose=f'{len(poses):03d}', scene=sc['name'], t=round(t, 2), citron=str(shot)))
    Path(a.out).write_text(json.dumps(poses, indent=1) + '\n')
    print(f'{len(poses)} poses from {len(list((run / "shots").glob("*.png")))} screenshots; '
          f'last flight ends {end_err:+.2f} s from UltraCam\'s COMPLETED line' if end_err is not None else '')


def picture(path, crop=None):
    from PIL import Image
    im = Image.open(path).convert('RGB')
    if crop:   # the game area inside citron's window (title, menu and status bars cut)
        w, h = im.size
        im = im.crop((0, int(h * crop[0]), w, int(h * (1 - crop[1]))))
    else:      # the recomp's shot: the picture without the unused part of the canvas (magenta)
        px = im.load(); w, h = im.size
        right = next((x for x in range(w - 1, -1, -1) if px[x, h // 4] != (255, 0, 255)), w - 1) + 1
        bottom = next((y for y in range(h - 1, -1, -1) if px[w // 4, y] != (255, 0, 255)), h - 1) + 1
        im = im.crop((0, 0, right, bottom))
    return im.resize((960, 540))


def difference(a, b):
    """Mean absolute difference of two 960x540 pictures at a coarser scale (small offsets in time matter less), without
    the top-right area where UltraCam draws its FPS overlay in citron's pictures."""
    from PIL import Image, ImageChops, ImageDraw, ImageStat
    sa, sb = a.resize((240, 135)), b.resize((240, 135))
    mask = Image.new('L', sa.size, 255)
    ImageDraw.Draw(mask).rectangle((150, 0, 240, 70), fill=0)
    return sum(ImageStat.Stat(ImageChops.difference(sa, sb), mask).mean) / 3


def cmd_report(a):
    from PIL import Image, ImageDraw
    scenes = load_route(a.route)
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    cit = json.loads((Path(a.citron) / 'FullBenchmark.json').read_text())
    rec = json.loads((Path(a.recomp) / 'FullBenchmark.json').read_text())
    rows = []
    for name, ft in (('citron', cit['Frametimes']), ('recomp', rec['Frametimes'])):
        parts = split_scenes(ft, scenes)
        for sc, part in zip(scenes, parts):
            rows.append(dict(side=name, scene=sc['name'], **ultracam_stats(part),
                             median_ms=round(statistics.median(part), 2) if part else 0,
                             p99_ms=round(sorted(part)[int(len(part) * 0.99)], 1) if part else 0))
        rows.append(dict(side=name, scene='Total', **ultracam_stats(ft), median_ms=round(statistics.median(ft), 2),
                         p99_ms=round(sorted(ft)[int(len(ft) * 0.99)], 1)))
    lines = ['| Scene | citron frames | recomp frames | citron avg | recomp avg | citron 1% | recomp 1% | citron 0.1% | recomp 0.1% '
             '| citron median ms | recomp median ms |', '|' + '---|' * 11]
    by = {(r['side'], r['scene']): r for r in rows}
    for sc in [s['name'] for s in scenes] + ['Total']:
        c, r = by[('citron', sc)], by[('recomp', sc)]
        lines.append(f"| {sc} | {c['Total']} | {r['Total']} | {c['Average']} | {r['Average']} | {c['Low']} | {r['Low']} | "
                     f"{c['Lowest']} | {r['Lowest']} | {c['median_ms']} | {r['median_ms']} |")
    pairs = []
    if a.shots:
        crop = tuple(float(x) for x in a.crop.split(','))
        sheet_dir = out / 'pairs'; sheet_dir.mkdir(exist_ok=True)
        for p in json.loads(Path(a.poses).read_text()):
            rp = Path(a.shots) / f"pose-{p['pose']}.png"
            if not rp.exists():
                continue
            ci, ri = picture(p['citron'], crop), picture(rp)
            d = difference(ci, ri)
            pair = Image.new('RGB', (1920, 570), (20, 20, 20))
            pair.paste(ci, (0, 30)); pair.paste(ri, (960, 30))
            ImageDraw.Draw(pair).text((8, 8), f"{p['pose']}  {p['scene']}  t={p['t']} s   citron (left) | recomp (right)   difference {d:.1f}",
                                      fill=(255, 255, 255))
            f = sheet_dir / f"{p['pose']}.jpg"; pair.save(f, quality=88)
            pairs.append(dict(p, difference=round(d, 2), pair=str(f)))
        pairs.sort(key=lambda p: -p['difference'])
        lines += ['', '| Pose | Scene | t (s) | Difference | Pair |', '|---|---|---|---|---|']
        lines += [f"| {p['pose']} | {p['scene']} | {p['t']} | {p['difference']} | {p['pair']} |" for p in pairs]
    (out / 'compare.json').write_text(json.dumps(dict(stats=rows, pairs=pairs), indent=1) + '\n')
    (out / 'compare.md').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('poses', 'report'):
        p = sub.add_parser(name)
        p.add_argument('--citron', required=True)
        p.add_argument('--route', required=True, help='ultracam-bench-route.json')
        p.add_argument('--out', required=True)
    sub.choices['report'].add_argument('--recomp', required=True)
    sub.choices['report'].add_argument('--shots', help='bench_run.py --poses output folder')
    sub.choices['report'].add_argument('--poses', help='the poses.json the shots were taken for')
    sub.choices['report'].add_argument('--crop', default='0.0735,0.0255', help='citron window: top,bottom fractions to cut')
    a = ap.parse_args()
    {'poses': cmd_poses, 'report': cmd_report}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())

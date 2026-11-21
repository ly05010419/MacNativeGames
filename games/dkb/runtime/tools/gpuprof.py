#!/usr/bin/env python3
"""Summarise the host's GPU profile lines in a log (Mac run log or the iPhone's Documents/log.txt).

DM_NVN_GPUPROF=<frame>[-<frame>] or every:<n> prints one "[gpuprof]" line per encoder of the profiled frames (render pass: size, colour attachments, depth, draws,
attachment load / store KB, Metal pixel formats; compute / copy: copies, uniform updates, dispatches) and a "frame" line per frame. DM_AB=<frames> prints one "[ab]"
line per phase. This tool reads both:

  gpuprof.py log.txt                       per-frame totals, then the mean per frame of each pass shape (size, colours, depth, formats), by GPU time
  gpuprof.py log.txt --by class            the same by size class relative to the frame's largest colour pass (full, half, quarter ... per axis: the same for every game)
  gpuprof.py log.txt --frames 600 1200     only those frames
  gpuprof.py log.txt --timeline 600        every encoder of frame 600 in GPU order, with the idle gaps between them
  gpuprof.py log.txt --ab                  the [ab] phases: medians per configuration and the difference from the base

Times are GPU timestamps taken where each encoder begins and ends (relaxed: encoders may overlap), so the sum over encoders is not the frame's GPU time; the frame line's
span (first start to last end) is closer to it. See docs/agents/05-debugging.md, "GPU time on the device".
"""
import argparse, collections, re, statistics, sys

ENC = re.compile(r'\[gpuprof\] F(\d+)\s+(\d+)\s+([\d.]+) us (render pass|compute / copy)(.*)')
RP = re.compile(r'^ (\d+) (\d+)x(\d+) colors (\d+) depth (\d+) loads (\d+) draws (\d+) start ([+-][\d.]+) seg (\d+)(?: load (\d+) KB store (\d+) KB)?(?: fmt (\d+)/(\d+))?')
CP = re.compile(r'^ after pass (\d+) start ([+-][\d.]+) seg (\d+) copies (\d+) \(uniform updates (\d+)\) dispatches (\d+)')
FRAME = re.compile(r'\[gpuprof\] frame (\d+): render ([\d.]+) ms, compute / copy ([\d.]+) ms over (\d+) encoders.*?first start to last end ([\d.]+) ms')
AB = re.compile(r'\[ab\] phase (\d+) (\S+): (\d+) frames, GPU p50 ([\d.]+) / p90 ([\d.]+) / mean ([\d.]+) ms, frame p50 ([\d.]+) / p90 ([\d.]+) / mean ([\d.]+) ms'
                r'(?:; per frame: attachments load ([\d.]+) MB store ([\d.]+) MB, passes ([\d.]+))?')

# MTLPixelFormat values the profile prints (fmt <colour 0>/<depth or stencil>; 0 = none)
MTL_FORMATS = {
    1: 'A8Unorm', 10: 'R8Unorm', 11: 'R8Unorm_sRGB', 12: 'R8Snorm', 13: 'R8Uint', 14: 'R8Sint', 20: 'R16Unorm', 22: 'R16Snorm', 23: 'R16Uint', 24: 'R16Sint',
    25: 'R16Float', 30: 'RG8Unorm', 31: 'RG8Unorm_sRGB', 32: 'RG8Snorm', 33: 'RG8Uint', 34: 'RG8Sint', 40: 'B5G6R5Unorm', 41: 'A1BGR5Unorm', 42: 'ABGR4Unorm',
    43: 'BGR5A1Unorm', 53: 'R32Uint', 54: 'R32Sint', 55: 'R32Float', 60: 'RG16Unorm', 62: 'RG16Snorm', 63: 'RG16Uint', 64: 'RG16Sint', 65: 'RG16Float',
    70: 'RGBA8Unorm', 71: 'RGBA8Unorm_sRGB', 72: 'RGBA8Snorm', 73: 'RGBA8Uint', 74: 'RGBA8Sint', 80: 'BGRA8Unorm', 81: 'BGRA8Unorm_sRGB', 90: 'RGB10A2Unorm',
    91: 'RGB10A2Uint', 92: 'RG11B10Float', 93: 'RGB9E5Float', 94: 'BGR10A2Unorm', 103: 'RG32Uint', 104: 'RG32Sint', 105: 'RG32Float', 110: 'RGBA16Unorm',
    112: 'RGBA16Snorm', 113: 'RGBA16Uint', 114: 'RGBA16Sint', 115: 'RGBA16Float', 123: 'RGBA32Uint', 124: 'RGBA32Sint', 125: 'RGBA32Float',
    250: 'Depth16Unorm', 252: 'Depth32Float', 253: 'Stencil8', 255: 'Depth24Unorm_Stencil8', 260: 'Depth32Float_Stencil8', 261: 'X32_Stencil8', 262: 'X24_Stencil8',
}


def fmt_name(v):
    return MTL_FORMATS.get(v, str(v)) if v else '-'


Enc = collections.namedtuple('Enc', 'frame idx us kind pass_ w h colors depth loads draws start seg load_kb store_kb cfmt dfmt copies ubd dispatches')


def parse(lines):
    """[gpuprof] encoder lines by frame (in log order) and the frame lines ({frame: dict}); lines that do not match are skipped"""
    frames, flines = collections.OrderedDict(), {}
    for line in lines:
        if '[gpuprof]' not in line: continue
        m = ENC.search(line)
        if m:
            f, idx, us, kind, rest = int(m.group(1)), int(m.group(2)), float(m.group(3)), m.group(4), m.group(5)
            if kind == 'render pass':
                r = RP.match(rest)
                if not r: continue
                g = [int(x) if x is not None else 0 for x in (r.group(1), r.group(2), r.group(3), r.group(4), r.group(5), r.group(6), r.group(7))]
                e = Enc(f, idx, us, 'R', g[0], g[1], g[2], g[3], g[4], g[5], g[6], float(r.group(8)), int(r.group(9)),
                        int(r.group(10) or 0), int(r.group(11) or 0), int(r.group(12) or 0), int(r.group(13) or 0), 0, 0, 0)
            else:
                c = CP.match(rest)
                if not c: continue
                e = Enc(f, idx, us, 'C', int(c.group(1)), 0, 0, 0, 0, 0, 0, float(c.group(2)), int(c.group(3)), 0, 0, 0, 0,
                        int(c.group(4)), int(c.group(5)), int(c.group(6)))
            frames.setdefault(f, []).append(e)
            continue
        m = FRAME.search(line)
        if m:
            flines[int(m.group(1))] = {'render_ms': float(m.group(2)), 'compute_ms': float(m.group(3)), 'encoders': int(m.group(4)), 'span_ms': float(m.group(5))}
    return frames, flines


def frame_stats(es):
    """Totals of one frame's encoders: sum of their times, span from the first start to the last end, idle time between them (GPU order)"""
    es = sorted(es, key=lambda e: e.start)
    tot = sum(e.us for e in es)
    span = max(e.start + e.us for e in es) - es[0].start
    gaps, end = 0.0, es[0].start
    for e in es:
        if e.start > end: gaps += e.start - end
        end = max(end, e.start + e.us)
    rp = [e for e in es if e.kind == 'R']
    return {'encoders': len(es), 'passes': len(rp), 'sum_us': tot, 'span_us': span, 'gap_us': gaps, 'draws': sum(e.draws for e in rp),
            'load_kb': sum(e.load_kb for e in rp), 'store_kb': sum(e.store_kb for e in rp)}


def shape_key(e):
    if e.kind != 'R': return 'compute / copy'
    z = '+d' if e.depth else ''
    f = fmt_name(e.cfmt) if e.colors else ''
    d = fmt_name(e.dfmt) if e.dfmt else ''
    return f'{e.w}x{e.h} {e.colors}c{z} {f}{"/" if f and d else ""}{d}'.rstrip()


def class_key(e, ref_area):
    """A size class relative to the frame's largest colour pass (ref_area pixels), by the scale per axis: the same classes for every game and resolution.
    Passes with four colour attachments or more are their own class (a deferred renderer's G-buffer); <=2 draws marks full-screen work (post-processing, copies)"""
    if e.kind != 'R': return 'compute / copy'
    area = e.w * e.h
    if e.colors == 0: return 'depth only (shadow maps)' if e.depth else 'no attachments'
    if area <= 64 * 64: return 'tiny (<=64x64)'
    s = (area / ref_area) ** 0.5 if ref_area else 1   # the scale per axis: 800x450 is half of 1600x900
    size = 'full' if s >= 0.7 else 'half' if s >= 0.35 else 'quarter' if s >= 0.18 else 'eighth' if s >= 0.09 else 'small'
    if e.colors >= 4: return f'{size}, {e.colors} colours{" + depth" if e.depth else ""}'
    return f'{size}, {"<=2 draws" if e.draws <= 2 else ">2 draws"}{" + depth" if e.depth else ""}'


def aggregate(frames, by='shape', only=None):
    """Mean per frame of each group over the frames: (rows sorted by GPU time, number of frames). A row: group, passes, us, draws, load KB, store KB"""
    agg = collections.defaultdict(lambda: [0, 0.0, 0, 0, 0]); nf = 0
    for f, es in frames.items():
        if only and f not in only: continue
        nf += 1
        ref = max((e.w * e.h for e in es if e.kind == 'R' and e.colors), default=0)
        for e in es:
            k = shape_key(e) if by == 'shape' else class_key(e, ref)
            a = agg[k]; a[0] += 1; a[1] += e.us; a[2] += e.draws; a[3] += e.load_kb; a[4] += e.store_kb
    rows = [(k, v[0] / nf, v[1] / nf, v[2] / nf, v[3] / nf, v[4] / nf) for k, v in agg.items()] if nf else []
    rows.sort(key=lambda r: -r[2])
    return rows, nf


def parse_ab(lines):
    out = []
    for line in lines:
        m = AB.search(line)
        if not m: continue
        g = m.groups()
        out.append({'phase': int(g[0]), 'config': g[1], 'frames': int(g[2]), 'gpu_p50': float(g[3]), 'gpu_p90': float(g[4]), 'gpu_mean': float(g[5]),
                    'frame_p50': float(g[6]), 'frame_p90': float(g[7]), 'frame_mean': float(g[8]),
                    'load_mb': float(g[9]) if g[9] else None, 'store_mb': float(g[10]) if g[10] else None, 'passes': float(g[11]) if g[11] else None})
    return out


def ab_summary(phases, skip=1):
    """Median of each value over the phases of each configuration, leaving out the first <skip> phases (loading, first-use pipeline builds)"""
    by = collections.OrderedDict()
    for p in phases:
        if p['phase'] < skip: continue
        by.setdefault(p['config'], []).append(p)
    res = collections.OrderedDict()
    for cfg, ps in by.items():
        r = {'phases': len(ps)}
        for k in ('gpu_p50', 'gpu_p90', 'gpu_mean', 'frame_p50', 'frame_p90', 'frame_mean', 'load_mb', 'store_mb', 'passes'):
            v = [p[k] for p in ps if p[k] is not None]
            r[k] = statistics.median(v) if v else None
        res[cfg] = r
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('log', help='the run log or Documents/log.txt')
    ap.add_argument('--frames', type=int, nargs='+', help='only these profiled frames')
    ap.add_argument('--by', choices=('shape', 'class'), default='shape', help='group passes by exact shape (default) or by size class relative to the largest colour pass')
    ap.add_argument('--top', type=int, default=25, help='groups listed (default 25); the rest are summed as "other"')
    ap.add_argument('--timeline', type=int, metavar='FRAME', help='every encoder of that frame in GPU order')
    ap.add_argument('--ab', action='store_true', help='summarise the DM_AB [ab] lines instead')
    ap.add_argument('--ab-skip', type=int, default=1, help='with --ab: phases left out at the start (default 1)')
    a = ap.parse_args(argv)
    with open(a.log, errors='replace') as fh: lines = fh.readlines()

    if a.ab:
        phases = parse_ab(lines)
        if not phases: print('no [ab] lines (DM_AB=<frames> with a switch set to ab)', file=sys.stderr); return 1
        res = ab_summary(phases, a.ab_skip)
        print(f'{len(phases)} phases; medians per configuration (first {a.ab_skip} left out):')
        print(f'  {"configuration":32s} {"phases":>6s} {"GPU p50":>8s} {"GPU p90":>8s} {"frame p50":>9s} {"frame p90":>9s} {"load MB":>8s} {"store MB":>8s} {"passes":>7s}')
        f1 = lambda v, w=8: f'{v:{w}.2f}' if v is not None else ' ' * (w - 1) + '-'
        for cfg, r in res.items():
            print(f'  {cfg:32s} {r["phases"]:6d} {f1(r["gpu_p50"])} {f1(r["gpu_p90"])} {f1(r["frame_p50"], 9)} {f1(r["frame_p90"], 9)} {f1(r["load_mb"])} {f1(r["store_mb"])} {f1(r["passes"], 7)}')
        base = res.get('base')
        if base:
            for cfg, r in res.items():
                if cfg == 'base': continue
                print(f'  {cfg} - base: GPU p50 {r["gpu_p50"] - base["gpu_p50"]:+.2f} ms, frame p50 {r["frame_p50"] - base["frame_p50"]:+.2f} ms')
        return 0

    frames, flines = parse(lines)
    if not frames: print('no [gpuprof] encoder lines (DM_NVN_GPUPROF=<frame> or every:<n>)', file=sys.stderr); return 1
    only = set(a.frames) if a.frames else None

    if a.timeline is not None:
        es = frames.get(a.timeline)
        if not es: print(f'frame {a.timeline} was not profiled; profiled: {" ".join(map(str, list(frames)[:20]))}', file=sys.stderr); return 1
        es = sorted(es, key=lambda e: e.start); end = es[0].start
        print(f'frame {a.timeline}: {len(es)} encoders in GPU order (start and duration in us; gap = idle before it)')
        for e in es:
            gap = max(0.0, e.start - end); end = max(end, e.start + e.us)
            if e.kind == 'R':
                what = (f'pass {e.pass_} {e.w}x{e.h} {e.colors}c{"+d" if e.depth else ""} draws {e.draws} loads {e.loads} '
                        f'load {e.load_kb} KB store {e.store_kb} KB {fmt_name(e.cfmt)}/{fmt_name(e.dfmt)}')
            else:
                what = f'compute / copy after pass {e.pass_}: copies {e.copies} (uniform updates {e.ubd}) dispatches {e.dispatches}'
            print(f'  {e.idx:4d} +{e.start:9.1f} {e.us:8.1f}{"  gap %.1f" % gap if gap >= 1 else "":>12s}  {what}')
        return 0

    for f, es in frames.items():
        if only and f not in only: continue
        s = frame_stats(es); fl = flines.get(f)
        print(f'frame {f}: {s["encoders"]} encoders ({s["passes"]} render passes), sum {s["sum_us"] / 1e3:.2f} ms, span {s["span_us"] / 1e3:.2f} ms, idle {s["gap_us"] / 1e3:.2f} ms, '
              f'draws {s["draws"]}, attachments load {s["load_kb"] / 1024:.0f} MB store {s["store_kb"] / 1024:.0f} MB'
              + (f' | host: render {fl["render_ms"]:.2f} ms, compute / copy {fl["compute_ms"]:.2f} ms' if fl else ''))
    rows, nf = aggregate(frames, a.by, only)
    tot = sum(r[2] for r in rows) or 1
    print(f'\nmean per frame over {nf} frames, by {a.by}:')
    print(f'  {"group":52s} {"passes":>7s} {"ms":>7s} {"share":>6s} {"us/pass":>8s} {"draws":>7s} {"load MB":>8s} {"store MB":>8s}')
    shown, rest = rows[:a.top], rows[a.top:]
    if rest: shown = shown + [('other (%d groups)' % len(rest), sum(r[1] for r in rest), sum(r[2] for r in rest), sum(r[3] for r in rest), sum(r[4] for r in rest), sum(r[5] for r in rest))]
    for k, n, us, dr, ld, st in shown:
        print(f'  {k:52s} {n:7.1f} {us / 1e3:7.2f} {100 * us / tot:5.1f}% {us / n if n else 0:8.1f} {dr:7.0f} {ld / 1024:8.1f} {st / 1024:8.1f}')
    print(f'  {"total":52s} {sum(r[1] for r in rows):7.1f} {tot / 1e3:7.2f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

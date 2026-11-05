#!/usr/bin/env python3
"""Summarize recorded Mac soak evidence without controlling the game.

Minute frame counters are coarse game-frame counts, not display-present counts.
Mode labels record observer requests, which must be checked against game logs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def fingerprint(path, contents):
    return dict(path=str(path.resolve()), bytes=len(contents),
                sha256=hashlib.sha256(contents).hexdigest())


def summarize(report, memory=(), log='', instrumentation=()):
    samples = report['samples']
    buckets = {}
    actions = [a['elapsed'] for a in report.get('actions', [])
               if a['command'].startswith(('key 19 ', 'key 20 '))]
    for before, after in zip(samples, samples[1:]):
        state = (before['picture'], before['interpolation'])
        if state != (after['picture'], after['interpolation']):
            continue
        # Exclude an interval containing a mode request, plus its first minute.
        if any(before['elapsed'] - 60 <= t <= after['elapsed'] for t in actions):
            continue
        dt, frames = after['elapsed'] - before['elapsed'], after['frame'] - before['frame']
        if dt <= 0 or frames < 0:
            continue
        b = buckets.setdefault(state, dict(seconds=0, game_frames=0, intervals=0))
        b['seconds'] += dt
        b['game_frames'] += frames
        b['intervals'] += 1
    requested_modes = []
    for (picture, interpolation), b in buckets.items():
        requested_modes.append(dict(picture=picture, interpolation=interpolation, **b,
                                    coarse_game_frames_per_second=b['game_frames']/b['seconds']))
    audios = [s['audio'] for s in samples if s.get('audio')]
    audio = None
    if audios:
        audio = dict(first=audios[0], last=audios[-1],
                     underrun_delta=audios[-1]['underruns']-audios[0]['underruns'],
                     overrun_delta=audios[-1]['overruns']-audios[0]['overruns'],
                     sampled_nonzero_rms=sum(a['rms'] > 0 for a in audios), samples=len(audios))
    memory_summary = None
    if memory:
        memory_summary = dict(samples=len(memory),
                              first_elapsed=memory[0]['elapsed_from_soak_start'],
                              last_elapsed=memory[-1]['elapsed_from_soak_start'],
                              first_footprint=memory[0]['phys_footprint'],
                              last_footprint=memory[-1]['phys_footprint'],
                              min_footprint=min(m['phys_footprint'] for m in memory),
                              max_footprint=max(m['phys_footprint'] for m in memory),
                              lifetime_max_footprint=max(m['lifetime_max_phys_footprint'] for m in memory))
    frame_lo, frame_hi = samples[0]['frame'], samples[-1]['frame']
    diagnostic_ranges = []
    for window in instrumentation:
        lo = window['file_birth_epoch'] - report['started']
        hi = window['file_modified_epoch'] - report['started']
        before = [s for s in samples if s['elapsed'] <= lo]
        after = [s for s in samples if s['elapsed'] >= hi]
        if before and after:
            diagnostic_ranges.append(dict(tool=window['tool'],
                first_frame=max(0, before[-1]['frame']-300), last_frame=after[0]['frame']+300,
                scope='conservative minute-sample bracket; overlap is not proof of causality'))
    spikes = [dict(frame=int(m[1]), game_log_seconds=float(m[2]), host_ms=float(m[3]),
                   gpu_reported_ms=float(m[4]), pipelines=int(m[5]), libraries=int(m[6]),
                   creation_ms=float(m[7])) for m in re.finditer(
                       r'\[长帧\] 帧 (\d+)（([\d.]+) s）整帧 ([\d.]+) ms.*?GPU ([\d.]+) ms；'
                       r'新建管线 (\d+) 条 \+ 着色器库 (\d+) 个共 ([\d.]+) ms', log)]
    spikes = [s for s in spikes if frame_lo <= s['frame'] <= frame_hi]
    for spike in spikes:
        spike['possible_diagnostic_overlap'] = [w['tool'] for w in diagnostic_ranges
            if w['first_frame'] <= spike['frame'] <= w['last_frame']]
    return dict(status=report['status'], elapsed_seconds=report['elapsed_seconds'],
                runtime_sha256=report['runtime_sha256'], frame_counter_granularity=300,
                frame_samples=len(samples), screenshot_requests=len(report.get('screenshots', [])),
                preexisting_diagnostics=len(report.get('preexisting_diagnostic_lines', [])),
                new_diagnostics=report.get('suspicious_log_lines', []), audio=audio,
                requested_mode_buckets=requested_modes, process_memory=memory_summary,
                relative_spike_diagnostics=dict(rows=len(spikes), first_frame=frame_lo,
                    last_frame=frame_hi, diagnostic_frame_brackets=diagnostic_ranges,
                    host_above_33ms=sum(s['host_ms'] > 33 for s in spikes),
                    host_above_100ms=sum(s['host_ms'] > 100 for s in spikes),
                    host_above_500ms=sum(s['host_ms'] > 500 for s in spikes),
                    largest_host=sorted(spikes, key=lambda s:s['host_ms'], reverse=True)[:12]),
                limitations=[
                    'coarse game-frame counters; not generated-frame or visible-display FPS',
                    'mode labels are requested states; confirm actual transitions in the game log',
                    'changing weather, time, camera and cache; buckets are not controlled A/B comparisons',
                    'relative-spike logs omit many ordinary frames; no full-frame p95/p99',
                    'log may include startup before observer and diagnostic-tool pauses; inspect timestamps',
                    'memory collection may start late; footprint growth alone does not identify a leak'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--memory', type=Path)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--instrumentation', type=Path, action='append', default=[],
                        help='diagnostic time-window JSON; may be repeated')
    args = parser.parse_args()
    # Hash the exact bytes parsed, including when observing a growing live log.
    evidence = {p: p.read_bytes() for p in (args.report, args.memory, args.log, *args.instrumentation) if p}
    report = json.loads(evidence[args.report])
    memory = [json.loads(line) for line in evidence[args.memory].splitlines() if line.strip()] if args.memory else []
    log = evidence[args.log].decode('utf-8', errors='replace') if args.log else ''
    result = summarize(report, memory, log, [json.loads(evidence[p]) for p in args.instrumentation])
    result['evidence'] = [fingerprint(p, contents) for p, contents in evidence.items()]
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

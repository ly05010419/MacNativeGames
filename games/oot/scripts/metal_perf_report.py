#!/usr/bin/env python3
"""Report the game's Metal performance from a macOS 27 Metal performance trace, by game state.

    metal_perf_report.py report TRACE.atrc --out DIR [--app APP] [--pid PID] [--note KEY=VALUE ...]
    metal_perf_report.py collect --out DIR [--last 10m | --start DATE [--end DATE]] [--app APP] [--pid PID] [--note ...]

macOS 27 records frame timing and related statistics of every Metal layer all the time (metalperftrace(1)). `collect`
takes a time range of that recording (metalperftrace collect) and reports it; `report` reports a trace collected before.
Both write into DIR:
  trace.atrc        Apple's raw trace (a copy; Instruments opens it). It holds every process that drew with Metal in the
                    time range, not only the game, with process names and IDs: look before you share it.
  performance.json  the game's numbers: the whole run, visible play, every state the game reported, numbers per state
  performance.md    the same, to read
  metadata.json     game, version, build (source commit, runtime UUID and SHA-256), hardware, system, test configuration
Only the game's process (totk-native, or --pid) is read from the trace. Nothing is collected unless this command runs,
and nothing leaves the Mac.

The game reports its states only with TOTK_STATE_REPORTING=1 (docs/metal-performance-diagnostics.md). Percentiles (p50 /
p90 / p95 / p99) need per-frame data, which Metal records only for a process started with
METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1 (or after `metalperftrace setup --enable per-frame-metrics --pid PID`); without it
the trace holds one-second summaries and the report says the percentiles are not available instead of estimating them.
Shader compiler statistics likewise need METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS=1.

A trace holds the states the game entered inside its time range, plus the ones the system repeats about every 90 s (its
heartbeat); collect a range that starts before the game does to have them all. --app: the app that ran (its runtime
SHA-256 is recorded when its UUID matches the trace's). --note: test configuration to record (repeatable).
Exit status: 0 report written; 2 usage; 3 metalperftrace missing or failed; 4 no game process in the trace.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

PROCESS = 'totk-native'   # the game binary of both games (the launcher execs Contents/Resources/totk/build/totk-native)
LIFECYCLE, GRAPHICS = 'org.macnativegames.Lifecycle', 'org.macnativegames.Graphics'
VISIBLE = ('focused', 'unfocused', 'settings')   # Lifecycle states in which the game picture is on screen
SCHEMA = 'macnativegames-metal-perf/1'
EXIT_TOOL, EXIT_NO_GAME = 3, 4
PERCENTILES = (50, 90, 95, 99)
COUNTERS = ('gameFrames', 'generatedFrames', 'mergedFrames', 'upscaledFrames', 'antiAliasedFrames', 'paceTicks1', 'paceTicks2',
            'paceTicks3', 'shaderTranslations', 'librariesCreated', 'pipelinesCreated', 'creationMs')   # totk_state_report.m
SAMPLE_SLACK = 0.25   # s: a state's own counter sample is read this close to its transition (sampledAt)


def scrub(text, *paths):
    """Text without the user's paths: the report folder, the trace and the home folder become placeholders."""
    for p, name in sorted([(str(p), f'<{n}>') for p, n in paths] + [(str(Path.home()), '~')], key=lambda x: -len(x[0])):
        if p:
            text = text.replace(p, name)   # the longest first: the trace inside the report folder inside the home folder
    return text


def tool(args, timeout=900):
    """Runs metalperftrace (args after the program name): (exit status, stdout, stderr). Missing: (None, '', why)."""
    exe = shutil.which('metalperftrace')
    if not exe:
        return None, '', 'metalperftrace not found: it ships with macOS 27 (/usr/bin/metalperftrace)'
    try:
        p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, '', f'metalperftrace {args[0]}: {e}'
    return p.returncode, p.stdout, p.stderr


def date(s):
    """metalperftrace's ISO dates ('2026-10-26T02:53:08.318Z'); None for 'Unknown' or nothing."""
    if not s or s == 'Unknown':
        return None
    return datetime.datetime.fromisoformat(s.replace('Z', '+00:00'))


def iso(d):
    return d.astimezone(datetime.timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z') if d else None


def stats_ms(s):
    """A metalperftrace statistics block ({'Count', 'Min (ms)', 'Average (ms)', ...}) as count/min/avg/max/stddev in ms."""
    if not s or not s.get('Count'):
        return None
    return dict(count=s['Count'], min=round(s['Min (ms)'], 3), avg=round(s['Average (ms)'], 3),
                max=round(s['Max (ms)'], 3), stddev=round(s['StdDev (ms)'], 3))


def pooled(blocks):
    """Several statistics blocks as one: count, min, max and the count-weighted mean, and the pooled standard deviation
    (exact for the union of the groups); None without data."""
    blocks = [b for b in blocks if b and b.get('Count')]
    n = sum(b['Count'] for b in blocks)
    if not n:
        return None
    mean = sum(b['Average (ms)'] * b['Count'] for b in blocks) / n
    ss = sum((b['Count'] - 1) * b['StdDev (ms)'] ** 2 + b['Count'] * (b['Average (ms)'] - mean) ** 2 for b in blocks)
    return dict(count=n, min=round(min(b['Min (ms)'] for b in blocks), 3), avg=round(mean, 3),
                max=round(max(b['Max (ms)'] for b in blocks), 3), stddev=round(math.sqrt(ss / (n - 1)) if n > 1 else 0, 3))


def nearest_rank(values, p):
    """The p-th percentile of the values by nearest rank (a measured value, never an interpolation)."""
    v = sorted(values)
    return v[max(0, math.ceil(p / 100 * len(v)) - 1)] if v else None


def frame_metrics(perf):
    """The frame statistics of one layer over some time (a session, or a state's aggregate)."""
    shown, skipped = perf.get('Presented Frame Stats') or {}, perf.get('Skipped Frame Stats') or {}
    out = dict(output_fps=round(shown['FPS'], 2) if shown.get('Frame Count') else None,
               drawables_shown=shown.get('Frame Count', 0), drawables_skipped=skipped.get('Frame Count', 0),
               frame_time_ms=stats_ms(perf.get('Frame-On-Glass Interval Stats')),
               cpu_to_present_ms=stats_ms(shown.get('End-to-end Walltime Stats (CPU)')),
               gpu_time_ms=stats_ms(shown.get('On-GPU Walltime Stats')),
               drawable_wait_ms=stats_ms(shown.get('Next Drawable Wait Walltime Stats')),
               present_lateness_ms=stats_ms(perf.get('On-Glass Present Lateness Stats')),
               command_buffers=shown.get('Command Buffer Count'), active_s=round(perf.get('Time Active (sec)', 0), 3))
    sc = shown.get('Shader Compiler')
    if sc:
        out['shader_compiler'] = dict(compilations=sc.get('Total Shader Compilations'),
                                      cached_compilations=sc.get('Total Cached Shader Compilations'),
                                      compilation_s=sc.get('Total Shader Compilation Time'),
                                      pipeline_states=sc.get('Total Pipeline States'))
    return out


def bucket_metrics(buckets):
    """Whole one-second buckets of the trace's timeline, summed: the same fields as frame_metrics."""
    if not buckets:
        return None
    shown = [b.get('Presented Frame Stats') or {} for b in buckets]
    frames, active = sum(s.get('Frame Count', 0) for s in shown), sum(b.get('Time Active (sec)', 0) for b in buckets)
    return dict(seconds=round(sum(b.get('Total Duration (sec)', 0) for b in buckets), 3), buckets=len(buckets),
                output_fps=round(frames / active, 2) if active else None, drawables_shown=frames,
                drawables_skipped=sum((b.get('Skipped Frame Stats') or {}).get('Frame Count', 0) for b in buckets),
                frame_time_ms=pooled([b.get('Frame-On-Glass Interval Stats') for b in buckets]),
                cpu_to_present_ms=pooled([s.get('End-to-end Walltime Stats (CPU)') for s in shown]),
                gpu_time_ms=pooled([s.get('On-GPU Walltime Stats') for s in shown]),
                drawable_wait_ms=pooled([s.get('Next Drawable Wait Walltime Stats') for s in shown]))


def main_layer(layers):
    """The game's CAMetalLayer: the layer that showed the most frames (another layer, e.g. a HUD's, shows few)."""
    def shown(layer):
        return ((layer.get('Total Session Stats') or layer.get('Performance Stats') or {})
                .get('Presented Frame Stats') or {}).get('Frame Count', 0)
    return max(layers, key=shown) if layers else None


def drawables(layer):
    """Per-drawable records (only when per-frame metrics were on): start, on-glass ms, GPU ms, CPU ms, drawable wait ms, and
    whether MetalFX frame interpolation was enabled for that drawable."""
    out = []
    for d in layer.get('On-Glass Drawable Timeline') or []:
        f = d.get('In-Flight Drawable Info') or {}
        cpu = (f.get('CPU End-to-End Walltime Interval') or {}).get('Duration MCT Seconds')
        out.append(dict(start=date(d.get('Start Date')), on_glass_ms=d.get('Duration MCT Seconds', 0) * 1e3,
                        gpu_ms=f['On-GPU Time (seconds)'] * 1e3 if 'On-GPU Time (seconds)' in f else None,
                        cpu_ms=cpu * 1e3 if cpu is not None else None,
                        wait_ms=f['Wait for Next Drawable Walltime Interval (seconds)'] * 1e3
                        if 'Wait for Next Drawable Walltime Interval (seconds)' in f else None,
                        interpolator=bool(f.get('MetalFX Frame Interpolator Enabled'))))
    return out


def percentiles(rows):
    """Nearest-rank percentiles of the per-drawable records (ms), or None without records. They describe the window's
    drawables (the present thread), the interpolated ones included, not the game's own frames."""
    if not rows:
        return None
    out = dict(drawables=len(rows), method='nearest rank over every drawable of the per-frame trace data')
    for key, name in (('on_glass_ms', 'frame_time_ms'), ('gpu_ms', 'gpu_time_ms'), ('cpu_ms', 'cpu_to_present_ms'),
                      ('wait_ms', 'drawable_wait_ms')):
        vals = [r[key] for r in rows if r[key] is not None]
        out[name] = {f'p{p}': round(nearest_rank(vals, p), 3) for p in PERCENTILES} if vals else None
    out['interpolator_drawables'] = sum(r['interpolator'] for r in rows)
    return out


def states_of(session, end):
    """The game's state transitions in time order. A state still open at the end of the trace ends with the session
    (end_inferred); one the system repeated with its heartbeat began before the trace (began_before_trace)."""
    out = []
    for domain, d in (session.get('States') or {}).items():
        for s in d.get('States') or []:
            start, stop = date(s.get('Start Date')), date(s.get('End Date'))
            inferred = stop is None or (end is not None and stop > end)
            vol = s.get('Volatile States') or {}
            sampled = date(vol.get('sampledAt')) if isinstance(vol.get('sampledAt'), str) else None
            stop = end if inferred else stop
            if start and stop and stop < start:   # a state begun after the layer's last frame (closing): no length
                stop = start
            out.append(dict(domain=domain, label=s.get('State Label'), start=start, end=stop,
                            end_inferred=inferred, stable=s.get('Stable States') or {}, volatile=vol, sampled=sampled,
                            began_before_trace=bool(sampled and start and (start - sampled).total_seconds() > SAMPLE_SLACK)))
    out.sort(key=lambda s: (s['start'], s['domain']))
    return out


def samples(states):
    """The game's counter readings (time, values), once each: every transition carries them, and a heartbeat repeats
    them unchanged with the original reading time (sampledAt)."""
    seen = {}
    for s in states:
        v = s['volatile']
        if 'gameFrames' in v:
            t = s['sampled'] or s['start']
            seen.setdefault(iso(t), (t, v))
    return sorted(seen.values(), key=lambda m: m[0])


def counter_change(marks, a, b):
    """How the counters changed from the reading at a (within SAMPLE_SLACK) to the last one up to b: (seconds, deltas)."""
    if a is None or b is None:
        return None
    inside = [m for m in marks if (a - m[0]).total_seconds() <= SAMPLE_SLACK and (m[0] - b).total_seconds() <= SAMPLE_SLACK]
    first = next((m for m in inside if abs((m[0] - a).total_seconds()) <= SAMPLE_SLACK), None)
    if not first or inside[-1] is first:
        return None
    t, v = inside[-1]
    return (t - first[0]).total_seconds(), {k: v[k] - first[1][k] for k in COUNTERS
                                           if isinstance(v.get(k), (int, float)) and isinstance(first[1].get(k), (int, float))}


def game_rates(seconds, c):
    """The three frame rates, never added or subtracted: the game's own presents, interpolated frames handed to the screen,
    game frames the display merged away; and how often the stages ran."""
    if not seconds or 'gameFrames' not in c:
        return None
    g = c['gameFrames']
    out = dict(seconds=round(seconds, 3), game_frames=g, game_fps=round(g / seconds, 2),
               generated_fps=round(c.get('generatedFrames', 0) / seconds, 2), merged_fps=round(c.get('mergedFrames', 0) / seconds, 2),
               source='game counters at state changes: presents (as the FPS box counts them), interpolated frames handed '
                      'to the screen, game frames the display link merged away')
    if g:
        out['metalfx_upscaled_pct'] = round(100 * c.get('upscaledFrames', 0) / g, 1)
        out['taa_pct'] = round(100 * c.get('antiAliasedFrames', 0) / g, 1)
        ticks = [c.get(f'paceTicks{i}', 0) for i in (1, 2, 3)]
        if sum(ticks):
            out['pace_ticks_pct'] = {f'{i}{"+" if i == 3 else ""}': round(100 * t / sum(ticks), 1) for i, t in zip((1, 2, 3), ticks)}
    out['shader_work'] = {k: c[k] for k in ('shaderTranslations', 'librariesCreated', 'pipelinesCreated', 'creationMs') if k in c}
    return out


def intervals(states, domain, labels=None):
    """The [start, end) ranges of a domain's states (only those labels)."""
    return [(s['start'], s['end']) for s in states if s['domain'] == domain and s['start'] and s['end']
            and (labels is None or s['label'] in labels)]


def intersect(xs, ys):
    out = []
    for a, b in xs:
        for c, d in ys:
            lo, hi = max(a, c), min(b, d)
            if lo < hi:
                out.append((lo, hi))
    return out


def inside(ranges, buckets):
    """The whole one-second buckets inside the ranges (a bucket that straddles a state change is left out)."""
    out = []
    for b in buckets:
        s, e = date(b.get('Start Date')), date(b.get('End Date'))
        if s and e and b.get('Total Duration (sec)', 0) >= 0.5 and any(lo <= s and e <= hi for lo, hi in ranges):
            out.append(b)
    return out


def overlapping(ranges, buckets):
    """The one-second buckets that overlap the ranges at all."""
    out = []
    for b in buckets:
        s, e = date(b.get('Start Date')), date(b.get('End Date'))
        if s and e and any(s < hi and lo < e for lo, hi in ranges):
            out.append(b)
    return out


def by_state(states, groups, skipped, rows, buckets):
    """One row per (domain, label): metalperftrace's aggregate (every second of the state, whatever the other domain
    said), the game's counters over it, and for Graphics also the visible part only (whole seconds while the picture
    showed: not starting, warming up, covered or closing), with per-drawable percentiles."""
    marks, visible = samples(states), intervals(states, LIFECYCLE, VISIBLE)
    keyed = {}
    for s in states:
        k = keyed.setdefault((s['domain'], s['label']), dict(stable=s['stable'], segments=0, seconds=0.0, ranges=[],
                                                             counted=0.0, counters={}, before=False))
        k['segments'] += 1
        k['before'] |= s['began_before_trace']
        if s['start'] and s['end']:
            k['seconds'] += (s['end'] - s['start']).total_seconds()
            k['ranges'].append((s['start'], s['end']))
            change = counter_change(marks, s['sampled'] or s['start'], s['end']) if not s['began_before_trace'] else \
                counter_change(marks, next((t for t, _ in marks if s['start'] <= t <= s['end']), None), s['end'])
            if change:
                k['counted'] += change[0]
                for c, d in change[1].items():
                    k['counters'][c] = k['counters'].get(c, 0) + d
    out = []
    for (domain, label), k in keyed.items():
        row = dict(domain=domain, label=label, stable=k['stable'], segments=k['segments'], seconds=round(k['seconds'], 3))
        if k['before']:
            row['began_before_trace'] = True
        g = groups.get((domain, label))
        if g:
            layer = main_layer(g.get('Layers') or [])
            row['apple_aggregate'] = dict(scope='metalperftrace --aggregate: every second of the state, in any state of '
                                                'the other domain', residency_pct=round(g.get('Residency (%)', 0), 2),
                                          **frame_metrics((layer or {}).get('Performance Stats') or {}))
        else:
            # metalperftrace lists the states it skipped without saying why: only what the trace shows is given
            row['apple_aggregate'] = None
            if k['seconds'] < 1:
                why = 'metalperftrace aggregates whole seconds; this state is shorter than one second'
            elif not any((b.get('Presented Frame Stats') or {}).get('Frame Count') for b in overlapping(k['ranges'], buckets)):
                why = 'the game window showed no drawable in it (the start-up screen, a covered window)'
            elif any(s['domain'] == domain and s['label'] == label for s in skipped):
                why = 'metalperftrace skipped it and gives no reason'
            else:
                why = 'metalperftrace has no aggregate for it'
            row['not_aggregated_because'] = why
        rates = game_rates(k['counted'], k['counters'])
        if rates:
            rates['covers_s'] = rates.pop('seconds')
            row['game'] = rates
        ranges = intersect(k['ranges'], visible) if domain == GRAPHICS else \
            (k['ranges'] if label in VISIBLE else [])
        if ranges:
            vis = bucket_metrics(inside(ranges, buckets))
            if vis:
                vis['scope'] = 'whole one-second buckets while the game picture showed (focused, unfocused, settings)'
            row['visible'] = vis
            row['percentiles'] = percentiles([r for r in rows if r['start'] and any(a <= r['start'] < b for a, b in ranges)])
        interp = k['stable'].get('interpolation')
        if domain == GRAPHICS and interp and interp != 'off':
            row['note'] = ('frame interpolation on: output FPS counts every drawable shown, the generated ones included; '
                           'game.game_fps is the game\'s own frame rate')
        out.append(row)
    out.sort(key=lambda r: (r['domain'], -r['seconds']))
    return out


def session_report(session, aggregate):
    start, end = date(session.get('Start Date')), date(session.get('End Date'))
    layer = main_layer(session.get('Layers') or [])
    total = (layer or {}).get('Total Session Stats') or {}
    if layer and total.get('End Date'):
        end = max(end, date(total['End Date'])) if end else date(total['End Date'])
    states = states_of(session, end)
    rows, buckets = drawables(layer or {}), (layer or {}).get('Stats Timeline') or []
    groups, skipped = {}, []
    if aggregate:
        for g in aggregate.get('Groups') or []:
            groups[(g.get('Domain'), g.get('State Label'))] = g
        skipped = [dict(domain=s.get('Domain'), label=s.get('State Label')) for s in aggregate.get('Skipped States') or []]
    life = next((s['stable'] for s in states if s['domain'] == LIFECYCLE and s['stable'].get('game')), {})
    conf = ((layer or {}).get('Configuration Timeline') or [(layer or {}).get('Configuration') or {}])[-1]
    usage = session.get('Resource Usage') or {}
    out = dict(pid=session.get('PID'), start=iso(start), end=iso(end),
               seconds=round((end - start).total_seconds(), 3) if start and end else None,
               game=dict(id=life.get('game'), version=life.get('gameVersion'), source_commit=life.get('sourceCommit'),
                         runtime_uuid=life.get('runtimeUUID'), shader_chain=life.get('shaderChain'),
                         settings=life.get('settings'), host=dict(model=life.get('hostModel'), chip=life.get('hostChip'),
                                                                 os=life.get('hostOS'))) if life else None,
               state_reporting=bool(states),
               layer=dict(width=conf.get('Width (pixels)'), height=conf.get('Height (pixels)'),
                          pixel_format=conf.get('MTL Pixel Format Name'), layers_in_process=len(session.get('Layers') or [])),
               overall=frame_metrics(total) if total else None,
               memory=dict(footprint_mib=usage.get('Physical Footprint (MiB)'), peak_footprint_mib=usage.get('Peak Physical Footprint (MiB)'),
                           metal_allocated_mib=usage.get('Metal Device Allocated Size (MiB)'),
                           sampled_at=usage.get('Date')) if usage else None,
               cpu=dict(user_s=usage.get('CPU User Time (Seconds)'), system_s=usage.get('CPU Sys Time (Seconds)'),
                        disk_read_mib=usage.get('Disk Reads (MiB)'), disk_write_mib=usage.get('Disk Writes (MiB)')) if usage else None,
               per_frame_data=bool(rows))
    visible = intervals(states, LIFECYCLE, VISIBLE)
    out['visible_play'] = dict(bucket_metrics(inside(visible, buckets)) or {}, percentiles=percentiles(
        [r for r in rows if r['start'] and any(a <= r['start'] < b for a, b in visible)])) if visible else None
    out['percentiles'] = percentiles(rows) or dict(available=False, reason=(
        'the trace holds one-second summaries only (count, min, average, max, standard deviation): start the game with '
        'METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1 or run `metalperftrace setup --enable per-frame-metrics --pid PID` '
        'for per-frame data'))
    if not (out['overall'] or {}).get('shader_compiler'):
        out['shader_compiler'] = dict(available=False, reason='no shader compiler statistics in the trace: start the game '
                                      'with METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS=1')
    out['states'] = [dict(domain=s['domain'], label=s['label'], start=iso(s['start']), end=iso(s['end']),
                          end_inferred=s['end_inferred'], began_before_trace=s['began_before_trace'],
                          stable=s['stable'], volatile=s['volatile']) for s in states]
    out['by_state'] = by_state(states, groups, skipped, rows, buckets)
    out['timeline'] = [dict(t=round((date(b['Start Date']) - start).total_seconds(), 3) if start else None,
                            seconds=round(b.get('Total Duration (sec)', 0), 3),
                            output_fps=round(b['Presented Frame Stats']['FPS'], 2) if (b.get('Presented Frame Stats') or {}).get('Frame Count') else 0,
                            frame_time_max_ms=(stats_ms(b.get('Frame-On-Glass Interval Stats')) or {}).get('max'),
                            gpu_avg_ms=(stats_ms((b.get('Presented Frame Stats') or {}).get('On-GPU Walltime Stats')) or {}).get('avg'))
                       for b in buckets if b.get('Total Duration (sec)', 0) >= 0.5]
    notes = []
    if not states:
        notes.append('the game reported no states: it reports them only with TOTK_STATE_REPORTING=1 on macOS 27')
    first = min((s['start'] for s in states if not s['began_before_trace'] and s['start']), default=None)
    if states and start and first and (first - start).total_seconds() > 2 and not any(s['began_before_trace'] for s in states):
        notes.append(f'the first state starts {round((first - start).total_seconds(), 1)} s into the run: what came before '
                     'is not attributed to any state')
    if any(s['began_before_trace'] for s in states):
        notes.append('some states began before the trace: the system repeated them (heartbeat); their start is not known')
    if any(s['domain'] == GRAPHICS and s['stable'].get('interpolation', 'off') != 'off' for s in states):
        notes.append('frame interpolation was on for part of the run: output FPS counts the generated drawables too; '
                     'game.game_fps per state is the game\'s own frame rate')
    if any(s['domain'] == LIFECYCLE and s['label'] == 'occluded' for s in states):
        notes.append('the window was covered (occluded): macOS stops presenting it, so those seconds are not game performance')
    if any(s['domain'] == LIFECYCLE and s['label'] == 'reportingOff' for s in states):
        notes.append('the reports were switched off for a while (reportingOff): those stretches belong to no state')
    if 'TOTK_ASSUME_FOCUS=1' in ((out['game'] or {}).get('settings') or ''):
        notes.append('TOTK_ASSUME_FOCUS=1 (a test run): the window never takes the keyboard, so it shows as unfocused throughout')
    out['notes'] = notes
    return out


def build_report(overview, aggregate, pid=None, process=PROCESS):
    """performance.json's content from metalperftrace's overview (--json-include-timeline --include-state-transitions)
    and --aggregate output (both already limited to the game's process)."""
    sessions = [s for s in overview if (pid is None and s.get('Process') == process) or (pid is not None and s.get('PID') == pid)]
    agg = {a.get('PID'): a for a in aggregate or [] if 'Groups' in a}
    return dict(schema=SCHEMA, process=process if pid is None else f'pid {pid}',
                sessions=[session_report(s, agg.get(s.get('PID'))) for s in sessions])


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def macho_uuid(path):
    """LC_UUID of a thin 64-bit Mach-O (the runtime), as the runtime reports it; None otherwise."""
    try:
        with open(path, 'rb') as f:
            magic, _, _, _, ncmds, size = struct.unpack('<IiiIII', f.read(32)[:24])
            if magic != 0xfeedfacf:
                return None
            cmds = f.read(size)
        off = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', cmds, off)
            if cmd == 0x1b:
                b = cmds[off + 8:off + 24].hex().upper()
                return f'{b[:8]}-{b[8:12]}-{b[12:16]}-{b[16:20]}-{b[20:]}'
            off += cmdsize
    except (OSError, struct.error):
        return None
    return None


def redact(value):
    """A setting's value without paths (a path names the user's folders)."""
    return '<path>' if isinstance(value, str) and ('/' in value or value.startswith('~')) else value


def app_identity(app):
    """The app's identity from its bundle; only file names, never where it is."""
    if not app:
        return None, []
    app = Path(app)
    try:
        info = plistlib.loads((app/'Contents/Info.plist').read_bytes())
    except (OSError, ValueError) as e:
        return None, [f'--app {app.name}: no readable Contents/Info.plist ({e.__class__.__name__})']
    runtime = app/'Contents/Resources/totk/build/totk-native'
    launcher = app/'Contents/MacOS'/str(info.get('CFBundleExecutable', ''))
    env = info.get('LSEnvironment') or {}
    out = dict(app=app.name, bundle_id=info.get('CFBundleIdentifier'), version=info.get('CFBundleShortVersionString'),
               source_commit=info.get('MNGSourceCommit'),
               app_defaults={k: redact(v) for k, v in sorted(env.items()) if re.match(r'(TOTK|DM|MTL|METAL)_', k)})
    problems = []
    if runtime.is_file():
        out.update(runtime_sha256=sha256(runtime), runtime_uuid=macho_uuid(runtime), runtime_bytes=runtime.stat().st_size)
    else:
        problems.append(f'{app.name}: no Contents/Resources/totk/build/totk-native')
    if launcher.is_file():
        out['launcher_sha256'] = sha256(launcher)
    try:
        out['shader_seed_chain'] = json.loads((app/'Contents/Resources/totk/build/shader-seed/seed-manifest.json').read_text()).get('chain')
    except (OSError, ValueError, AttributeError):
        pass
    return out, problems


def sysctl(name):
    try:
        return subprocess.run(['sysctl', '-n', name], capture_output=True, text=True, timeout=10).stdout.strip() or None
    except OSError:
        return None


def machine():
    """The Mac this command runs on, without serial numbers or other identifiers."""
    mem, p, e = sysctl('hw.memsize'), sysctl('hw.perflevel0.logicalcpu'), sysctl('hw.perflevel1.logicalcpu')
    sw = {}
    for key, flag in (('product_version', '-productVersion'), ('build_version', '-buildVersion')):
        try:
            sw[key] = subprocess.run(['sw_vers', flag], capture_output=True, text=True, timeout=10).stdout.strip() or None
        except OSError:
            sw[key] = None
    return dict(model=sysctl('hw.model'), chip=sysctl('machdep.cpu.brand_string'), arch=platform.machine(),
                memory_gb=round(int(mem) / 2**30) if mem and mem.isdigit() else None,
                performance_cores=int(p) if p and p.isdigit() else None,
                efficiency_cores=int(e) if e and e.isdigit() else None, os='macOS', **sw)


def tool_source():
    """This script's own source: the repository commit it runs from, and whether that checkout has changes."""
    here = Path(__file__).resolve().parent
    try:
        rev = subprocess.run(['git', '-C', str(here), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=10)
        dirty = subprocess.run(['git', '-C', str(here), 'status', '--porcelain'], capture_output=True, text=True, timeout=30)
    except OSError:
        return dict(script='games/totk/scripts/metal_perf_report.py', commit=None)
    return dict(script='games/totk/scripts/metal_perf_report.py', commit=rev.stdout.strip() or None,
                uncommitted_changes=bool(dirty.stdout.strip()) if rev.returncode == 0 else None)


def cell(text):
    return str(text).replace('|', '\\|')


def ms(block, key='avg'):
    return f"{block[key]:.2f}" if block else 'n/a'


PRIVACY = ('trace.atrc is Apple\'s raw trace: it holds the Metal statistics, names and process IDs of every process that '
           'drew in its time range, not only the game. Look before you share it; the other three files hold the game only.')


def markdown(perf, meta):
    """performance.md: the summary a person reads first."""
    out = ['# Metal performance report', '', f'> {PRIVACY}', '']
    for n, m in enumerate(meta['runs'], 1):
        out.append(f"- Run {n}: {m['game'].get('title') or m['game'].get('id') or 'game unknown'} {m['game'].get('version') or ''}; "
                   f"commit {m['build'].get('source_commit') or 'not recorded'}; runtime {m['build'].get('runtime_check')}; "
                   f"{(m.get('host') or {}).get('model') or '?'} {(m.get('host') or {}).get('os') or ''}")
    out += [f"- Trace: {', '.join(f['name'] for f in meta['trace']['files']) or 'none'}; status: {perf['status']}", '']
    for e in perf.get('errors', []):
        out.append(f"> **{e['step']}**: {e['message']}")
    if perf.get('errors'):
        out.append('')
    if not perf['sessions'] and perf['status'] != 'failed':
        out.append(f"No `{PROCESS}` process is in the trace.")
    for n, s in enumerate(perf['sessions'], 1):
        o, v = s['overall'] or {}, s['visible_play'] or {}
        out += [f"## Run {n}: pid {s['pid']}, {s['start']} → {s['end']} ({s['seconds']} s)", '']
        if not s['state_reporting']:
            out.append('The game reported no states (TOTK_STATE_REPORTING=1 needs macOS 27); the numbers cover the whole run.')
        out += ['', '| | Whole run | Visible play |', '|---|---|---|',
                f"| Output FPS (drawables shown per second) | {o.get('output_fps')} | {v.get('output_fps', 'n/a')} |",
                f"| Drawables shown / skipped | {o.get('drawables_shown')} / {o.get('drawables_skipped')} | {v.get('drawables_shown', 'n/a')} / {v.get('drawables_skipped', 'n/a')} |"]
        for key, name in (('frame_time_ms', 'Frame time on glass'), ('gpu_time_ms', 'GPU time per drawable'),
                          ('cpu_to_present_ms', 'CPU begin to present'), ('drawable_wait_ms', 'Next drawable wait')):
            out.append(f"| {name}, avg / max (ms) | {ms(o.get(key))} / {ms(o.get(key), 'max')} | {ms(v.get(key))} / {ms(v.get(key), 'max')} |")
        p = s['percentiles']
        if p.get('available') is False:
            out.append(f"| Frame time p50 / p95 / p99 | not available: {p['reason']} | |")
        else:
            ft, vp = p.get('frame_time_ms') or {}, ((v.get('percentiles') or {}).get('frame_time_ms') or {})
            out.append(f"| Frame time p50 / p95 / p99 (ms, per drawable) | {ft.get('p50')} / {ft.get('p95')} / {ft.get('p99')} | "
                       f"{vp.get('p50', 'n/a')} / {vp.get('p95', 'n/a')} / {vp.get('p99', 'n/a')} |")
        m = s['memory'] or {}
        out.append(f"| Memory footprint / peak / Metal (MiB) | {m.get('footprint_mib') and round(m['footprint_mib'])} / "
                   f"{m.get('peak_footprint_mib') and round(m['peak_footprint_mib'])} / {m.get('metal_allocated_mib') and round(m['metal_allocated_mib'])} | |")
        sc = o.get('shader_compiler')
        out.append(f"| Metal shader compilations (cached) / time (s) | {sc['compilations']} ({sc['cached_compilations']}) / {sc['compilation_s']} | |" if sc
                   else f"| Metal shader compiler | not available: {s['shader_compiler']['reason']} | |")
        rows = s['by_state']
        if rows:
            out += ['', '| State | Seconds | Output FPS (all / visible) | Game FPS | Generated / merged per s | Frame time avg / max ms (visible) | GPU avg ms | CPU→present avg ms | p95 frame ms |',
                    '|---|---|---|---|---|---|---|---|---|']
            short = [r for r in rows if not r['apple_aggregate']]
            for r in rows:
                if not r['apple_aggregate'] and len(short) > 3:
                    continue
                a, vis, g = r['apple_aggregate'] or {}, r.get('visible') or {}, r.get('game')
                name = f"{r['domain'].rsplit('.', 1)[-1]}: {cell(r['label'])}" + (' (began before the trace)' if r.get('began_before_trace') else '')
                game = f"{g['game_fps']}" + (f" ({g['covers_s']} s)" if g['covers_s'] < r['seconds'] - 1 else '') if g else 'n/a'
                gen = f"{g['generated_fps']} / {g['merged_fps']}" if g else 'n/a'
                ft = vis.get('frame_time_ms') or a.get('frame_time_ms')
                p95 = ((r.get('percentiles') or {}).get('frame_time_ms') or {}).get('p95', 'n/a')
                fps = [x if x is not None else 'n/a' for x in (a.get('output_fps'), vis.get('output_fps'))]
                out.append(f"| {name} | {r['seconds']} | {fps[0]} / {fps[1]} | {game} | {gen} | "
                           f"{ms(ft)} / {ms(ft, 'max')} | {ms(vis.get('gpu_time_ms') or a.get('gpu_time_ms'))} | "
                           f"{ms(vis.get('cpu_to_present_ms') or a.get('cpu_to_present_ms'))} | {p95} |")
            if len(short) > 3:
                out.append(f"| {len(short)} states not aggregated | {round(sum(r['seconds'] for r in short), 3)} | "
                           "no metalperftrace aggregate (why: performance.json by_state, Not available) | | | | | | |")
            out += ['', 'Output FPS: every drawable the window showed (metalperftrace; "all" is its aggregate over every second '
                    'of the state, "visible" only whole seconds while the picture showed). Game FPS: the game\'s own presents, '
                    'from its counters at state changes (the seconds they cover in brackets when less than the state). '
                    'Generated: MetalFX interpolated frames handed to the screen; merged: game frames the display link '
                    'merged away. The three are never added or subtracted.']
        if s['states']:
            out += ['', '<details><summary>State transitions</summary>', '', '| Start | Domain | State |', '|---|---|---|']
            out += [f"| {x['start']} | {x['domain'].rsplit('.', 1)[-1]} | {cell(x['label'])} |" for x in s['states']]
            out += ['', '</details>']
        for note in s['notes']:
            out.append(f"\n- {note}")
        out.append('')
    if perf.get('unavailable'):
        out += ['## Not available', ''] + [f"- {u['metric']}: {u['reason']}" for u in perf['unavailable']]
    return '\n'.join(out) + '\n'


def unavailable(perf):
    """Every metric the trace could not give, with why: the report never estimates them."""
    out = []
    for n, s in enumerate(perf['sessions'], 1):
        if s['percentiles'].get('available') is False:
            out.append(dict(metric=f'run {n}: p50 / p90 / p95 / p99 of frame time, GPU time, CPU to present and drawable wait',
                            reason=s['percentiles']['reason']))
        if 'shader_compiler' in s:
            out.append(dict(metric=f'run {n}: Metal shader compiler statistics', reason=s['shader_compiler']['reason']))
        if not s['state_reporting']:
            out.append(dict(metric=f'run {n}: numbers per game state, game frame rate', reason='the game reported no states'))
        short = [r for r in s['by_state'] if not r['apple_aggregate']]
        for why in dict.fromkeys(r['not_aggregated_because'] for r in short):
            these = [r for r in short if r['not_aggregated_because'] == why]
            out.append(dict(metric=f"run {n}: metalperftrace aggregate of {len(these)} state(s): " + ', '.join(
                f"{r['domain'].rsplit('.', 1)[-1]} {r['label']}" for r in these[:8]) + (' ...' if len(these) > 8 else ''),
                reason=why))
        nogame = [r for r in s['by_state'] if not r.get('game') and r['label'] != 'reportingOff']
        if s['state_reporting'] and nogame:
            out.append(dict(metric=f"run {n}: game frame rate of {len(nogame)} state(s): " + ', '.join(
                f"{r['domain'].rsplit('.', 1)[-1]} {r['label']}" for r in nogame[:8]) + (' ...' if len(nogame) > 8 else ''),
                reason='no counter reading after the state began inside it (a state open at the end of the trace, or '
                       'repeated by the heartbeat): close the game before collecting to have the last one'))
    return out


def run_meta(s, apps):
    """One run's build, host and test configuration: from the trace (the game's own report) first."""
    g = s['game'] or {}
    match = next((a for a in apps if a.get('runtime_uuid') and a['runtime_uuid'] == g.get('runtime_uuid')), None)
    build = dict(source_commit=g.get('source_commit') or (match or {}).get('source_commit'), runtime_uuid=g.get('runtime_uuid'),
                 shader_chain=g.get('shader_chain'),
                 note='source_commit is what the app recorded at build time; uncommitted changes do not show in it: '
                      'runtime_uuid and runtime_sha256 identify the build')
    if match:
        build.update(runtime_sha256=match['runtime_sha256'], launcher_sha256=match.get('launcher_sha256'),
                     runtime_check=f"UUID matches --app {match['app']}", app={k: match[k] for k in ('app', 'bundle_id', 'version', 'runtime_bytes')})
    else:
        build['runtime_check'] = 'unverified: no --app with this runtime UUID' if g.get('runtime_uuid') else 'unverified: the game did not report its UUID'
    titles = {'totk': 'Tears of the Kingdom', 'botw': 'Breath of the Wild'}
    return dict(pid=s['pid'], game=dict(id=g.get('id'), title=titles.get(g.get('id')), version=g.get('version')),
                build=build, host=g.get('host'), settings_in_force=g.get('settings'),
                app_defaults=(match or {}).get('app_defaults'),
                first_graphics_state=next((x['label'] for x in s['states'] if x['domain'] == GRAPHICS), None),
                per_frame_metrics=s['per_frame_data'], shader_compiler_metrics=bool((s['overall'] or {}).get('shader_compiler')))


def write(out, perf, meta):
    (out/'performance.json').write_text(json.dumps(perf, ensure_ascii=False, indent=1) + '\n')
    (out/'metadata.json').write_text(json.dumps(meta, ensure_ascii=False, indent=1) + '\n')
    (out/'performance.md').write_text(markdown(perf, meta))


def metadata(args, traces, collected, runs, problems):
    sw = machine()
    return dict(schema='macnativegames-metal-perf-metadata/1', generated_utc=iso(datetime.datetime.now(datetime.timezone.utc)),
                tool=tool_source(), runs=runs, report_host=sw,
                test_config=dict(notes=dict(args.note or [])),
                trace=dict(files=[dict(name=t.name, bytes=t.stat().st_size, sha256=sha256(t)) for t in traces],
                           collected_by_this_command=collected),
                metalperftrace=dict(tool='metalperftrace' if shutil.which('metalperftrace') else None,
                                    macos=f"{sw.get('product_version')} ({sw.get('build_version')})"),
                app_problems=problems, privacy=PRIVACY)


def report(traces, out, args, collected):
    """Writes performance.json, performance.md and metadata.json for the traces already in out; returns the exit status."""
    errors, overview, aggregate = [], [], []
    pred = f'pid == {args.pid}' if args.pid is not None else f"processName == '{PROCESS}'"
    for t in traces:
        for name, extra in (('overview', ['--json-include-timeline', '--include-state-transitions']), ('aggregate', ['--aggregate', '--json'])):
            rc, stdout, stderr = tool(['overview', *extra, '--predicate', pred, str(t)])
            if rc != 0:
                errors.append(dict(step=f'metalperftrace overview ({name}) {t.name}',
                                   message=scrub((stderr.strip() or f'exit status {rc}')[:2000], (out, 'out'), (t, 'trace'))))
                continue
            try:
                data = json.loads(stdout)
            except ValueError as e:
                errors.append(dict(step=f'metalperftrace overview ({name}) {t.name}', message=f'not JSON: {e}'))
                continue
            (overview if name == 'overview' else aggregate).extend(data if isinstance(data, list) else [data])
    perf = build_report(overview, aggregate, args.pid)
    perf['errors'] = errors
    perf['status'] = 'failed' if errors and not overview else 'partial' if errors else 'ok' if perf['sessions'] else 'no game process'
    perf['unavailable'] = unavailable(perf) if perf['sessions'] else []
    apps, problems = [], []
    for a in args.app or []:
        ident, why = app_identity(a)
        problems += why
        if ident:
            apps.append(ident)
    runs = [run_meta(s, apps) for s in perf['sessions']]
    reported = {r['build'].get('runtime_uuid') for r in runs} - {None}
    for a in apps:
        if a.get('runtime_uuid') and not reported:
            problems.append(f"--app {a['app']}: no run in the trace reported its runtime UUID (state reports off): "
                            "whether it is the build that ran is not known")
        elif a.get('runtime_uuid') and a['runtime_uuid'] not in reported:
            problems.append(f"--app {a['app']}: its runtime UUID {a['runtime_uuid']} is not one the trace's runs report: not the build that ran")
    write(out, perf, metadata(args, traces, collected, runs, problems))
    print(f"{out.name}: {perf['status']}, {len(perf['sessions'])} game run(s)\n  {PRIVACY}" +
          ''.join(f"\n  {e['step']}: {e['message']}" for e in perf['errors']) + ''.join(f'\n  {p}' for p in problems))
    return EXIT_TOOL if errors else 0 if perf['sessions'] else EXIT_NO_GAME


def place(src, dst):
    """Copies a trace into the report folder: an APFS clone where possible (no extra space), a byte copy otherwise."""
    if src.resolve() == dst.resolve():
        return
    if dst.exists():
        dst.unlink()
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode:
        shutil.copy2(src, dst)


def failure(out, args, step, message):
    """A report that only says what failed (the tool is missing or collected nothing): the metadata is still written."""
    perf = dict(schema=SCHEMA, status='failed', errors=[dict(step=step, message=scrub(message, (out, 'out')))], sessions=[], unavailable=[])
    write(out, perf, metadata(args, [], True, [], []))
    print(f'{out.name}: failed: {step}: {perf["errors"][0]["message"]}', file=sys.stderr)
    return EXIT_TOOL


def note(text):
    key, sep, value = text.partition('=')
    if not sep or not key:
        raise argparse.ArgumentTypeError(f'--note wants KEY=VALUE, got {text!r}')
    return key, redact(value)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('report', 'collect'):
        p = sub.add_parser(name)
        if name == 'report':
            p.add_argument('trace', type=Path, help='a .atrc file from metalperftrace collect')
        else:
            when = p.add_mutually_exclusive_group()
            when.add_argument('--last', default=None, help='the time before now to collect, e.g. 90s, 10m, 1h (default 10m)')
            when.add_argument('--start', help='start of the time range (metalperftrace collect --start format)')
            p.add_argument('--end', help='end of the time range (default now)')
        p.add_argument('--out', type=Path, required=True, help='the report folder (created)')
        p.add_argument('--app', type=Path, action='append', help='an app that ran (its build identity goes into metadata.json); repeatable')
        p.add_argument('--pid', type=int, help='report this process instead of every totk-native process')
        p.add_argument('--note', type=note, action='append', help='test configuration KEY=VALUE to record (repeatable)')
    args = parser.parse_args(argv)
    if args.command == 'collect' and args.end and not args.start:
        parser.error('--end needs --start')
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    if args.command == 'report':
        if not args.trace.is_file():
            parser.error(f'{args.trace}: no such trace file')
        dst = out/'trace.atrc'
        place(args.trace, dst)
        return report([dst], out, args, collected=False)
    when = ['--start', args.start] + (['--end', args.end] if args.end else []) if args.start else ['--last', args.last or '10m']
    with tempfile.TemporaryDirectory(dir=out, prefix='.collect-') as tmp:
        rc, stdout, stderr = tool(['collect', *when, '--prefix', 'trace', '--json', tmp])
        if rc != 0:
            return failure(out, args, 'metalperftrace collect', (stderr.strip() or f'exit status {rc}')[:2000])
        try:
            found = [Path(p) for p in json.loads(stdout).get('traces', [])]
        except (ValueError, AttributeError) as e:
            return failure(out, args, 'metalperftrace collect', f'unexpected output (not JSON with "traces"): {e}')
        found = [p for p in (p if p.is_absolute() else Path(tmp)/p for p in found) if p.is_file()]
        if not found:
            return failure(out, args, 'metalperftrace collect', 'no trace was written for that time range')
        names = ['trace.atrc'] if len(found) == 1 else [f'trace-{i}.atrc' for i in range(1, len(found) + 1)]
        traces = []
        for src, name in zip(sorted(found), names):
            os.replace(src, out/name)
            traces.append(out/name)
    return report(traces, out, args, collected=True)


if __name__ == '__main__':
    sys.exit(main())

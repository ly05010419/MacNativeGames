#!/usr/bin/env python3
"""What the Smash host does not do yet, from the host's own reports, so a gap is fixed before the player finds it (Q60).

The host is loud by design: an unimplemented service, IPC command or GPU function stops the game with its name, and a
GPU operation it can only half do is drawn without the missing part and reported once per run as a 【红】 line. Those
lines reach the log long before anyone sees the result (2026-10-24: the missing scaled texture copy behind the magenta
World of Light save picture was in four logs before the player's screenshot). This report gathers them:

  1. every distinct 【红】 warning in the logs (addresses, keys and timings folded, sizes and formats kept: they are what
     tells two copy shapes apart), with how many logs had it, the first and the last, and the quest that tracks it;
  2. the stops (FAIL lines) with the missing name;
  3. the GPU placeholders: NVN functions the game can fetch (names.txt) that no host function implements, computed from
     the sources and checked against the count the runtime prints at start; those called in a session are listed.

    gap_report.py                         all logs in SSBU_build/logs
    gap_report.py --session <log>         what this one log shows that no earlier log did (ssbu-play.sh runs this)
"""
import argparse
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
RED = '【红】'
# Warnings and stops a quest tracks (pattern on the folded text -> quest, "✅" once fixed); the rest is open.
TRACKED = [
    (r'贴图→贴图拷贝未实现的形态：1920x1080x1→384x216x1', 'Q58 ✅'),
    (r'贴图→贴图拷贝未实现的形态：1920x1080x1→960x540x1 标志 0 格式 260→260', 'Q59'),
    (r'VP9 decode error', 'Q48'),
    (r'service "(caps:su|fsp-srv)"', 'Q54 ✅'),
    (r'service "hwopus"', 'Q42 ✅'),
]
FOLD = [
    (re.compile(r'\b[0-9a-f]{64}(-[0-9a-f]{12})?(_b)?\b'), '<shader>'),
    (re.compile(r'0x[0-9a-f]{6,}'), '<addr>'),
    (re.compile(r'\b\d+(\.\d+)? (us|ms|s)\b'), '<time>'),
    (re.compile(r'\b\d+-byte\b'), '<n>-byte'),
    (re.compile(r'x0=\S+ x1=\S+ x2=\S+'), 'x0..x2'),
    (re.compile(r'\bser=\d+'), 'ser=<n>'),
    (re.compile(r'\(\d+ times\)'), '(<n> times)'),
    (re.compile(r'贴图号 \d+'), '贴图号 <n>'),
]
# A warning line can run into the next message written by another thread without a newline in between.
NEXT_MESSAGE = re.compile(r'\[(totk|dm|ssbu|game-slot|smo)[^\]]{0,20}\]')


def fold(text):
    cut = NEXT_MESSAGE.search(text)
    if cut and cut.start() > 0:
        text = text[:cut.start()]
    for pattern, by in FOLD:
        text = pattern.sub(by, text)
    return text.strip()


def warnings(text):
    """The folded 【红】 warnings of one log, in order of first appearance."""
    seen = {}
    for line in text.splitlines():
        at = line.find(RED)
        if at >= 0:
            seen.setdefault(fold(line[at + len(RED):]), None)
    return list(seen)


def stops(text):
    """The FAIL lines with the name the host printed just before (a service, a command, a GPU function)."""
    lines, found = text.splitlines(), []
    for i, line in enumerate(lines):
        if '[totk] FAIL:' in line:
            why = next((l.strip() for l in reversed(lines[max(0, i - 40):i]) if 'not implemented' in l or '未实现被调用' in l), '')
            found.append(fold(line.split('FAIL:', 1)[1]) + (f' — {fold(why)}' if why else ''))
    return found


def placeholders(gfx):
    """NVN functions in names.txt without a host function (NVN(...) definitions and g_nops in hle_nvn.c)."""
    names = [n.split()[0] for n in (gfx / 'names.txt').read_text().splitlines() if n.strip()]
    implemented = set()
    for src in sorted(gfx.glob('*.c')) + sorted(gfx.glob('*.m')):
        text = src.read_text(errors='replace')
        implemented |= set(re.findall(r'\bNVN[A-Z_]*\((nvn[A-Za-z0-9]+)', text))
        nops = re.search(r'g_nops\[\]\s*=\s*\{(.*?)\};', text, re.S)
        if nops:
            implemented |= set(re.findall(r'"(nvn[A-Za-z0-9]+)"', nops.group(1)))
    return [n for n in names if n not in implemented], len(names)


def runtime_count(text):
    """The placeholder count the runtime printed at start (hle_nvn.c: 其余 N 个接报红桩), or None."""
    m = re.search(r'其余 (\d+) 个接报红桩', text)
    return int(m.group(1)) if m else None


def tracked(warning):
    return next((quest for pattern, quest in TRACKED if re.search(pattern, warning)), '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--logs', type=Path, default=HERE / 'SSBU_build/logs')
    ap.add_argument('--gfx', type=Path, default=HERE / 'third_party/d2r_gfx/gfx/nvn')
    ap.add_argument('--session', type=Path, help='report only what this log shows that no earlier log did')
    a = ap.parse_args()
    logs = sorted(a.logs.glob('*.log'), key=lambda p: (p.stat().st_mtime, p.name))
    if a.session:
        logs = [p for p in logs if p.resolve() != a.session.resolve() and p.stat().st_mtime <= a.session.stat().st_mtime] + [a.session]
    if not logs:
        sys.exit(f'gap_report: no logs in {a.logs}')
    texts = {p: p.read_text(errors='replace') for p in logs}
    table, stop_table = {}, {}
    for p in logs:
        for w in warnings(texts[p]):
            table.setdefault(w, []).append(p.name)
        for s in stops(texts[p]):
            stop_table.setdefault(s, []).append(p.name)
    missing, total = placeholders(a.gfx)
    counted = next((c for c in (runtime_count(texts[p]) for p in reversed(logs)) if c is not None), None)
    called = sorted({m for p in logs for m in re.findall(r'NVN 未实现被调用：(nvn[A-Za-z0-9]+)', texts[p])})
    if a.session:
        last = a.session.name
        new = [w for w, seen in table.items() if seen == [last]]
        new_stops = [s for s, seen in stop_table.items() if seen == [last]]
        print(f'Lücken: {len(new)} neue Warnungen, {len(new_stops)} neue Abbrüche in {last} (gegen {len(logs) - 1} frühere Logs)')
        for w in new:
            print(f'  NEU {w}' + (f'  [{tracked(w)}]' if tracked(w) else ''))
        for s in new_stops:
            print(f'  NEU Abbruch: {s}')
        return
    print(f'{len(logs)} Logs, {len(table)} verschiedene Warnungen, {len(stop_table)} verschiedene Abbrüche\n')
    order = {p.name: i for i, p in enumerate(logs)}   # oldest first: the most recently seen go to the top
    when = {p.name: time.strftime('%d.%m. %H:%M', time.localtime(p.stat().st_mtime)) for p in logs}
    print('| Warnung | Logs | zuletzt gesehen | Quest |')
    print('|---|---|---|---|')
    for w, seen in sorted(table.items(), key=lambda kv: (-order[kv[1][-1]], -len(kv[1]), kv[0])):
        print(f'| {w[:140]} | {len(seen)} | {when[seen[-1]]} ({seen[-1]}) | {tracked(w) or "offen"} |')
    if stop_table:
        print('\n| Abbruch | Logs | zuletzt gesehen | Quest |\n|---|---|---|---|')
        for s, seen in sorted(stop_table.items(), key=lambda kv: -order[kv[1][-1]]):
            print(f'| {s[:160]} | {len(seen)} | {when[seen[-1]]} ({seen[-1]}) | {tracked(s) or "offen"} |')
    check = 'gleich' if counted == len(missing) else f'ABWEICHUNG: Laufzeit meldet {counted}'
    print(f'\nGPU-Platzhalter: {len(missing)} von {total} NVN-Funktionen ohne Host-Funktion (Laufzeit-Zählung: {check}); '
          f'in den Logs aufgerufen: {", ".join(called) if called else "keiner"}')
    if counted is not None and counted != len(missing):
        sys.exit(1)


if __name__ == '__main__':
    main()

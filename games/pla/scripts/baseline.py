#!/usr/bin/env python3
"""baseline.py — one-command PLA regression baseline (next-steps A6).

Runs the default build for N seconds through run_scenario.sh, parses the stderr
log, applies the A6 gates, and writes both a readable summary and a JSON record.
The JSON is meant to be kept next to the commit it was measured on, so two runs
can be compared with plain `jq`/python.

  python3 scripts/baseline.py                          # 60 s cold baseline (PASS/FAIL)
  python3 scripts/baseline.py --seconds 120 --tag base-120
  python3 scripts/baseline.py --hot                    # hot start: first frame < 6 s
  python3 scripts/baseline.py --reuse-log /tmp/pla_re/lead1.log   # parse only
  python3 scripts/baseline.py --mfx --tag b1-interp30 --seconds 60  # B1: TOTK_MFX=interp30
  python3 scripts/baseline.py --dry-run                # print the plan, launch nothing

Metrics (all from stderr, no probes required except the audio counter):
  first frame   [totk-boot] <s> first presented frame           (cold <12 s, hot <6 s, A5)
  frames        max "至帧 N", plus strictly-increasing windows  (A6 "帧递增")
  frame time    "计时 至帧 N: 帧时间 p50/p95/最大" per 300 frames (A2; gate last p95 ≤ 35 ms)
  p99           上屏 p99 (reported, not gated)
  memory peak   max(常驻 R MB, 上屏 footprint F MiB, 显卡 G MB)   (A1; gate ≤ 4096 MB)
  crashes       host abort / [nvn-mtl] FATAL / 内存看门狗 / SEGV   (A6; gate 0)
  audio         [pcmfed] last nonzero A/N, AudioOut open count   (A4/A6; gate nonzero>0)

B1 scene (--mfx): injects TOTK_GPU_ALLOW_SUBMIT=1 TOTK_MFX=interp30 and gates the
  [mfx] canvas / scene / frame-interpolator lines, 插值帧 > 0 and MetalFX GPU
  p95 < 16.7 ms; there is deliberately no frame-rate gate (a busy machine moves
  wall-clock frame time without saying anything about MetalFX).

The audio nonzero counter needs PLA_AUDIO_PCM_LOG=1 (runtime/audio_out.c:346).
The default run injects it (observation only: it counts samples as they are
appended; it does not change the DSP path). Use --no-audio-probe for a strict
"no PLA_*/TOTK_* env at all" run; then the nonzero gate is SKIPped and only the
AudioOut-open gate applies. The exact injected env is recorded in the JSON.

Exit codes: 0 = PASS, 1 = FAIL, 2 = usage/environment error, 3 = run aborted
(run_scenario died early; the log is still parsed and reported as FAIL).
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RUNNER = os.path.join(HERE, "run_scenario.sh")
DEFAULT_BUILD_DIR = os.environ.get("PLA_BUILD_DIR") or os.path.join(REPO, "PLA_build")
DEFAULT_LOG_DIR = os.environ.get("PLA_LOG_DIR") or "/tmp/pla_re"
SCHEMA = "pla-baseline-v1"

TARGET_FPS = 30.0          # PLA's normal frame rate on this port
FRAME_BUDGET_MS = 1000.0 / TARGET_FPS   # 33.333 ms
TAG_RE = re.compile(r"^[A-Za-z0-9._-]+$")

RE_FIRST = re.compile(r"\[totk-boot\]\s+([0-9]+(?:\.[0-9]+)?)\s+first presented frame")
RE_TIMING = re.compile(
    r"计时 至帧\s+(\d+)：\s*帧时间 p50\s+([0-9.]+)\s*/\s*p95\s+([0-9.]+)\s*/\s*最大\s+([0-9.]+)\s*ms")
RE_PRESENT = re.compile(
    r"上屏 至帧\s+(\d+)：\s*(\d+) 帧，掉帧\s+(\d+)(?:（丢弃\s*(\d+)）)?，"
    r"p99\s+([0-9.]+)\s*ms；footprint\s+(\d+)\s*MiB")
RE_INTERVAL = re.compile(r"帧间隔 至帧\s+(\d+)：.*?p99\s+([0-9.]+)\s*ms")
RE_MEM = re.compile(r"内存 至帧\s+(\d+)：常驻\s+(\d+)\s*MB，显卡已分配\s+(\d+)\s*MB")
RE_PCM = re.compile(r"\[pcmfed\]\s+append #(\d+)\s+bytes=(\d+)\s+nonzero=(\d+)/(\d+)")
RE_AUDIO_OPEN = re.compile(r"\[totk-audio\]\s+AudioOut open\s+(\d+)\s*Hz")

# B1 (MetalFX frame interpolation, TOTK_MFX=interp30). The [mfx] lines carry the
# shared "[dm] [nvn-mtl] " prefix; matching starts at the literal "[mfx] " tag.
RE_MFX_CANVAS = re.compile(r"\[mfx\] canvas: texture (\d+) \(PLA rule: the composite's target\)")
RE_MFX_CANVAS_OTHER = re.compile(r"\[mfx\] canvas: texture (\d+) \(\d+x(\d+)\)")
RE_MFX_SCENE = re.compile(
    r"\[mfx\] scene \(format (\d+)\) / UI / output textures (\d+)x(\d+)")
RE_MFX_INTERP = re.compile(
    r"\[mfx\] frame interpolator: depth / motion (\d+)x(\d+) -> (\d+)x(\d+)")
RE_MFX_STATS = re.compile(
    r"\[mfx\] 至帧 (\d+)：插值帧 (\d+) / 游戏帧 (\d+)（MetalFX GPU p50 ([0-9.]+) / p95 ([0-9.]+) ms")

# (name, regex) — any hit means the session is not crash-free.  【红】 lines alone
# are not failures: most are recurring benign warnings (scissor mismatch, CPU
# fallback for pre-laid-out textures).
CRASH_MARKERS = [
    ("host_abort", re.compile(r"^\[totk\] host abort", re.M)),
    ("nvn_fatal", re.compile(r"\[nvn-mtl\] FATAL", re.M)),
    ("mem_watchdog", re.compile(r"内存看门狗", re.M)),
    ("segv", re.compile(r"Segmentation fault|SIGSEGV|EXC_BAD_ACCESS", re.M)),
]


def out(msg=""):
    print(msg)
    sys.stdout.flush()


def err(msg):
    print(msg, file=sys.stderr)
    sys.stderr.flush()


def iso_now():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


# ---------------------------------------------------------------------------
# parsing
# ---------------------------------------------------------------------------
def parse_log(text):
    """Turn a stderr log into the A6 metrics. Never raises on missing sections."""
    m = {}

    firsts = [float(v) for v in RE_FIRST.findall(text)]
    m["first_frame_s"] = min(firsts) if firsts else None

    timing = [(int(f), float(p50), float(p95), float(mx))
              for f, p50, p95, mx in RE_TIMING.findall(text)]
    present = [(int(f), int(n), int(dropped), int(discard or 0), float(p99), int(fp))
               for f, n, dropped, discard, p99, fp in RE_PRESENT.findall(text)]
    mem = [(int(f), int(rss), int(gpu)) for f, rss, gpu in RE_MEM.findall(text)]
    intervals = [(int(f), float(p99)) for f, p99 in RE_INTERVAL.findall(text)]

    frame_numbers = [w[0] for w in present] + [w[0] for w in timing] + \
                    [w[0] for w in mem] + [w[0] for w in intervals]
    m["frames"] = max(frame_numbers) if frame_numbers else 0
    m["window_count"] = len(timing)
    m["present_window_count"] = len(present)

    # A6 "帧递增": the present windows must advance strictly.
    present_frames = [w[0] for w in present]
    m["frames_increasing"] = (all(b > a for a, b in zip(present_frames, present_frames[1:]))
                              if len(present_frames) >= 2 else None)
    m["frames_presented_total"] = sum(w[1] for w in present) if present else 0

    if timing:
        f, p50, p95, mx = timing[-1]
        m["p50_last_ms"] = p50
        m["p95_last_ms"] = p95
        m["max_frame_last_ms"] = mx
        m["p95_worst_ms"] = max(w[2] for w in timing)
        m["p95_worst_frame"] = max(timing, key=lambda w: w[2])[0]
        m["p95_median_ms"] = sorted(w[2] for w in timing)[len(timing) // 2]
    else:
        for k in ("p50_last_ms", "p95_last_ms", "max_frame_last_ms",
                  "p95_worst_ms", "p95_worst_frame", "p95_median_ms"):
            m[k] = None

    if present:
        m["p99_last_ms"] = present[-1][4]
        m["p99_worst_ms"] = max(w[4] for w in present)
        m["dropped_last"] = present[-1][2]
        m["dropped_total"] = sum(w[2] for w in present)
        m["discarded_total"] = sum(w[3] for w in present)
        m["footprint_max_mib"] = max(w[5] for w in present)
    else:
        m["p99_last_ms"] = None
        m["p99_worst_ms"] = None
        m["dropped_last"] = None
        m["dropped_total"] = None
        m["discarded_total"] = None
        m["footprint_max_mib"] = None

    if mem:
        m["resident_max_mb"] = max(w[1] for w in mem)
        m["gpu_max_mb"] = max(w[2] for w in mem)
        m["mem_readout_count"] = len(mem)
    else:
        m["resident_max_mb"] = None
        m["gpu_max_mb"] = None
        m["mem_readout_count"] = 0

    mem_candidates = [v for v in (m["footprint_max_mib"], m["resident_max_mb"],
                                  m["gpu_max_mb"]) if v is not None]
    m["memory_peak_mb"] = max(mem_candidates) if mem_candidates else None

    pcm = [(int(i), int(nbytes), int(nz), int(n)) for i, nbytes, nz, n in RE_PCM.findall(text)]
    m["audio_appends"] = len(pcm)
    if pcm:
        m["audio_nonzero"] = pcm[-1][2]
        m["audio_appended"] = pcm[-1][3]
        m["audio_nonzero_ratio"] = (pcm[-1][2] / pcm[-1][3]) if pcm[-1][3] else None
    else:
        m["audio_nonzero"] = None
        m["audio_appended"] = None
        m["audio_nonzero_ratio"] = None
    m["audio_out_open"] = len(RE_AUDIO_OPEN.findall(text))

    # B1 interpolation lines (absent unless TOTK_MFX is on).
    canvas = RE_MFX_CANVAS.findall(text)
    canvas_other = RE_MFX_CANVAS_OTHER.findall(text)
    scene = RE_MFX_SCENE.findall(text)
    interp = RE_MFX_INTERP.findall(text)
    mfx_stats = [(int(f), int(made), int(gf), float(p50), float(p95))
                 for f, made, gf, p50, p95 in RE_MFX_STATS.findall(text)]
    m["mfx_canvas_texture"] = int(canvas[-1]) if canvas else None
    m["mfx_canvas_pla_rule"] = bool(canvas)
    m["mfx_canvas_other"] = ("%sx%s" % (canvas_other[-1][1], canvas_other[-1][2])) if canvas_other else None
    m["mfx_scene"] = bool(scene)
    m["mfx_scene_format"] = int(scene[-1][0]) if scene else None
    m["mfx_output"] = ("%sx%s" % (scene[-1][1], scene[-1][2])) if scene else None
    m["mfx_interpolator"] = bool(interp)
    m["mfx_interp_input"] = ("%sx%s" % (interp[-1][0], interp[-1][1])) if interp else None
    m["mfx_interp_output"] = ("%sx%s" % (interp[-1][2], interp[-1][3])) if interp else None
    m["mfx_window_count"] = len(mfx_stats)
    if mfx_stats:
        m["mfx_frames_made"] = mfx_stats[-1][1]
        m["mfx_game_frames"] = mfx_stats[-1][2]
        m["mfx_ratio"] = (mfx_stats[-1][1] / float(mfx_stats[-1][2])) if mfx_stats[-1][2] else None
        m["mfx_gpu_p50_last_ms"] = mfx_stats[-1][3]
        m["mfx_gpu_p95_last_ms"] = mfx_stats[-1][4]
        worst = max(mfx_stats, key=lambda w: w[4])
        m["mfx_gpu_p95_worst_ms"] = worst[4]
        m["mfx_gpu_p95_worst_frame"] = worst[0]
        m["mfx_frames_made_total"] = sum(w[1] for w in mfx_stats)
    else:
        m["mfx_frames_made"] = None
        m["mfx_game_frames"] = None
        m["mfx_ratio"] = None
        m["mfx_gpu_p50_last_ms"] = None
        m["mfx_gpu_p95_last_ms"] = None
        m["mfx_gpu_p95_worst_ms"] = None
        m["mfx_gpu_p95_worst_frame"] = None
        m["mfx_frames_made_total"] = None

    m["crash_markers"] = {}
    for name, rx in CRASH_MARKERS:
        hits = rx.findall(text)
        if hits:
            m["crash_markers"][name] = len(hits)
    m["crash_count"] = sum(m["crash_markers"].values())
    return m


def read_log(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def read_rc(path):
    """Raw wait status written by run_scenario.sh (<tag>.rc), or None."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            value = f.read().strip()
        return int(value)
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------
# gates
# ---------------------------------------------------------------------------
def add_check(checks, name, ok, detail, required=True, value=None, limit=None):
    checks.append({"name": name, "ok": ok, "required": required,
                   "detail": detail, "value": value, "limit": limit})


def build_checks(metrics, args, run_info):
    checks = []
    m = metrics

    if run_info["mode"] == "run":
        add_check(checks, "ran_to_completion", run_info["runner_exit"] == 0,
                  "run_scenario exit %s" % run_info["runner_exit"],
                  value=run_info["runner_exit"], limit=0)
    else:
        add_check(checks, "ran_to_completion", None, "log-only parse (--reuse-log)",
                  required=False)

    ff = m["first_frame_s"]
    if ff is None:
        add_check(checks, "first_frame", False, "no 'first presented frame' line (crash before first frame?)")
    else:
        add_check(checks, "first_frame", ff <= args.first_frame_max,
                  "%.3f s (%s)" % (ff, "hot" if args.hot else "cold"),
                  value=ff, limit=args.first_frame_max)

    if m["frames_increasing"] is None:
        add_check(checks, "frames_increasing", None, "fewer than two present windows",
                  required=False)
    else:
        add_check(checks, "frames_increasing", m["frames_increasing"],
                  "present windows advance" if m["frames_increasing"] else "frame counter went backwards")

    add_check(checks, "frames_min", m["frames"] >= args.min_frames,
              "%d frames (至帧), min %d" % (m["frames"], args.min_frames),
              value=m["frames"], limit=args.min_frames)

    if m["p95_last_ms"] is None:
        add_check(checks, "p95_frame_time", False,
                  "no 计时 windows (run shorter than %d frames, or log truncated)" % (300,))
    else:
        add_check(checks, "p95_frame_time", m["p95_last_ms"] <= args.p95_max,
                  "last %.2f ms (worst window %.2f ms @ frame %s)"
                  % (m["p95_last_ms"], m["p95_worst_ms"], m["p95_worst_frame"]),
                  value=m["p95_last_ms"], limit=args.p95_max)

    if args.p95_worst_max > 0:
        if m["p95_worst_ms"] is None:
            add_check(checks, "p95_worst", False, "no 计时 windows")
        else:
            add_check(checks, "p95_worst", m["p95_worst_ms"] <= args.p95_worst_max,
                      "worst window %.2f ms @ frame %s"
                      % (m["p95_worst_ms"], m["p95_worst_frame"]),
                      value=m["p95_worst_ms"], limit=args.p95_worst_max)

    peak = m["memory_peak_mb"]
    if peak is None:
        add_check(checks, "memory_peak", False, "no memory readout (no 上屏/内存 line)")
    else:
        add_check(checks, "memory_peak", peak <= args.mem_max_mb,
                  "%d MB (footprint %s / resident %s / gpu %s)"
                  % (peak, m["footprint_max_mib"], m["resident_max_mb"], m["gpu_max_mb"]),
                  value=peak, limit=args.mem_max_mb)

    add_check(checks, "crash_free", m["crash_count"] == 0,
              "none" if not m["crash_markers"]
              else ", ".join("%s×%d" % (k, v) for k, v in sorted(m["crash_markers"].items())),
              value=m["crash_count"], limit=0)

    add_check(checks, "audio_out_open", m["audio_out_open"] > 0,
              "%d AudioOut open%s" % (m["audio_out_open"],
                                      "" if m["audio_out_open"] else " (audio never started)"),
              value=m["audio_out_open"], limit=1)

    if m["audio_nonzero"] is not None:
        add_check(checks, "audio_nonzero", m["audio_nonzero"] > 0,
                  "nonzero %d/%d (%.0f%% of appends)"
                  % (m["audio_nonzero"], m["audio_appended"],
                     100.0 * (m["audio_nonzero_ratio"] or 0.0)),
                  value=m["audio_nonzero"], limit=1)
    elif args.audio_probe and run_info["mode"] == "run":
        add_check(checks, "audio_nonzero", False,
                  "PLA_AUDIO_PCM_LOG=1 was injected but no [pcmfed] line appeared")
    else:
        add_check(checks, "audio_nonzero", None,
                  "skipped (no PLA_AUDIO_PCM_LOG=1 in this log; "
                  "use --audio-probe on a real run to gate it)",
                  required=False)

    # B1 scene gates (only with --mfx; no frame-rate gate: a busy machine moves
    # wall-clock frame time without saying anything about MetalFX).
    if args.mfx:
        add_check(checks, "mfx_canvas", m["mfx_canvas_pla_rule"],
                  "canvas texture %s (PLA rule)" % m["mfx_canvas_texture"]
                  if m["mfx_canvas_pla_rule"]
                  else "no '[mfx] canvas: texture N (PLA rule: the composite's target)' line")
        add_check(checks, "mfx_scene", m["mfx_scene"],
                  "scene format %s, output %s" % (m["mfx_scene_format"], m["mfx_output"])
                  if m["mfx_scene"]
                  else "no '[mfx] scene (format N) / UI / output textures WxH' line")
        add_check(checks, "mfx_interpolator", m["mfx_interpolator"],
                  "depth / motion %s -> %s" % (m["mfx_interp_input"], m["mfx_interp_output"])
                  if m["mfx_interpolator"]
                  else "no '[mfx] frame interpolator: depth / motion WxH -> WxH' line")
        made = m["mfx_frames_made"]
        add_check(checks, "mfx_interp_frames", (made or 0) > 0,
                  "插值帧 %s / 游戏帧 %s in the last window (%d window%s)"
                  % (fmt(made), fmt(m["mfx_game_frames"]), m["mfx_window_count"],
                     "" if m["mfx_window_count"] == 1 else "s")
                  if made is not None
                  else "no '[mfx] 至帧 N：插值帧 …' window (run < 300 game frames or interpolation off)",
                  value=made, limit=1)
        p95 = m["mfx_gpu_p95_last_ms"]
        add_check(checks, "mfx_gpu_p95", p95 is not None and p95 < args.mfx_gpu_p95_max,
                  "last window %.2f ms (worst %.2f ms @ 至帧 %s)"
                  % (p95, m["mfx_gpu_p95_worst_ms"], m["mfx_gpu_p95_worst_frame"])
                  if p95 is not None
                  else "no MetalFX GPU timing in the log",
                  value=p95, limit=args.mfx_gpu_p95_max)
    return checks


def verdict_of(checks):
    return "PASS" if all(c["ok"] is not False for c in checks) else "FAIL"


# ---------------------------------------------------------------------------
# summary / json
# ---------------------------------------------------------------------------
def fmt(v, spec="%s"):
    return "?" if v is None else spec % v


def status_label(c):
    if c["ok"] is None:
        return "skip"
    return "ok" if c["ok"] else "FAIL"


def print_summary(metrics, checks, run_info, out_path, verdict):
    m = metrics
    out("PLA baseline — %s%s" % (verdict, "" if verdict == "PASS" else " (%d failed)"
                                 % sum(1 for c in checks if c["ok"] is False)))
    out("  log        %s (%d lines)" % (run_info["log"], run_info["log_lines"]))
    if run_info["git"]:
        g = run_info["git"]
        out("  git        %s (%s%s)" % (g["head"], g["branch"], ", dirty" if g["dirty"] else ""))
    if run_info.get("wall_seconds") is not None:
        out("  run        %ss requested, wall %.1fs (%s -> %s), runner exit %s, process status %s"
            % (run_info.get("seconds"), run_info["wall_seconds"],
               run_info["started_at"], run_info["finished_at"], run_info["runner_exit"],
               "n/a" if run_info.get("process_rc") is None else run_info["process_rc"]))
    if m["first_frame_s"] is not None:
        out("  first frame %.3f s%s" % (m["first_frame_s"], " (hot)" if run_info["hot"] else " (cold)"))
    out("  frames     %d (至帧; min %d)" % (m["frames"], run_info["min_frames"]))
    if m["p50_last_ms"] is not None:
        out("  p50/p95    %.2f / %.2f ms (last %d-frame window; median p95 %.2f; worst p95 %.2f @ %s)"
            % (m["p50_last_ms"], m["p95_last_ms"], 300, m["p95_median_ms"],
               m["p95_worst_ms"], m["p95_worst_frame"]))
        out("  p99        %s ms (worst %s)" % (fmt(m["p99_last_ms"], "%.2f"),
                                               fmt(m["p99_worst_ms"], "%.2f")))
    out("  memory     %s MB (footprint %s / resident %s / gpu %s)"
        % (fmt(m["memory_peak_mb"]), fmt(m["footprint_max_mib"]),
           fmt(m["resident_max_mb"]), fmt(m["gpu_max_mb"])))
    if m["audio_nonzero"] is not None:
        out("  audio      AudioOut open %d, nonzero %d/%d"
            % (m["audio_out_open"], m["audio_nonzero"], m["audio_appended"]))
    else:
        out("  audio      AudioOut open %d, nonzero n/a" % m["audio_out_open"])
    if m["dropped_total"] is not None:
        out("  drops      %d dropped / %d discarded (last window %s)"
            % (m["dropped_total"], m["discarded_total"], m["dropped_last"]))
    if run_info.get("mfx") or m["mfx_window_count"]:
        out("  mfx        canvas %s / scene format %s (output %s) / interp %s -> %s"
            % (fmt(m["mfx_canvas_texture"]), fmt(m["mfx_scene_format"]), fmt(m["mfx_output"]),
               fmt(m["mfx_interp_input"]), fmt(m["mfx_interp_output"])))
        if m["mfx_window_count"]:
            out("  mfx frames 插值帧 %d / 游戏帧 %d (%.0f%%), MetalFX GPU p50/p95 %.2f / %.2f ms"
                " (worst p95 %.2f @ 至帧 %s)"
                % (m["mfx_frames_made"], m["mfx_game_frames"],
                   100.0 * (m["mfx_ratio"] or 0.0), m["mfx_gpu_p50_last_ms"],
                   m["mfx_gpu_p95_last_ms"], m["mfx_gpu_p95_worst_ms"],
                   m["mfx_gpu_p95_worst_frame"]))
    if run_info["fps_est"] is not None:
        out("  fps (est)  %.2f  (presented frames / wall seconds; includes startup)"
            % run_info["fps_est"])

    out("")
    for c in checks:
        label = "  %-18s %-4s" % (c["name"], status_label(c))
        lim = ""
        if c["limit"] is not None:
            lim = " (limit %s)" % c["limit"]
        out("%s %s%s" % (label, c["detail"], lim))
    failed = [c["name"] for c in checks if c["ok"] is False]
    if failed:
        out("")
        out("  failures: %s" % ", ".join(failed))
    if out_path:
        out("")
        out("  json       %s" % out_path)


def git_info():
    try:
        head = subprocess.check_output(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
        branch = subprocess.check_output(["git", "-C", REPO, "rev-parse", "--abbrev-ref", "HEAD"],
                                         text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(["git", "-C", REPO, "status", "--porcelain"],
                                             text=True, stderr=subprocess.DEVNULL).strip())
        return {"head": head, "branch": branch, "dirty": dirty}
    except Exception:
        return None


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def build_parser():
    p = argparse.ArgumentParser(
        description="PLA regression baseline (A6): run, parse, gate, report.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit: 0 PASS, 1 FAIL, 2 usage/environment, 3 run aborted early (log is parsed anyway).")
    p.add_argument("--seconds", type=int, default=60, help="run duration, default 60")
    p.add_argument("--tag", default=None, help="log tag, default baseline-<timestamp>")
    p.add_argument("--build-dir", default=DEFAULT_BUILD_DIR)
    p.add_argument("--log-dir", default=DEFAULT_LOG_DIR)
    p.add_argument("--reuse-log", metavar="PATH", default=None,
                   help="parse an existing log instead of running")
    p.add_argument("--out", metavar="PATH", default=None,
                   help="JSON path (default <log-dir>/<tag>.baseline.json); '-' = stdout")
    p.add_argument("--no-json", action="store_true", help="do not write a JSON file")
    p.add_argument("--dry-run", action="store_true", help="print the plan and exit")
    p.add_argument("--hot", action="store_true", help="hot-start thresholds (first frame < 6 s)")
    p.add_argument("--first-frame-max", type=float, default=None,
                   help="first-frame limit in seconds (default 12 cold / 6 hot)")
    p.add_argument("--p95-max", type=float, default=round(FRAME_BUDGET_MS * 1.05, 2),
                   help="last-window p95 limit; default 33.33*1.05 = 35.0 ms (A2)")
    p.add_argument("--p95-worst-max", type=float, default=0.0,
                   help="optional limit for the worst 300-frame window p95 (0 = off)")
    p.add_argument("--mem-max-mb", type=int, default=4096,
                   help="memory peak limit in MB, default 4096")
    p.add_argument("--min-frames", type=int, default=None,
                   help="minimum 至帧 number (default seconds*30*0.5)")
    p.add_argument("--audio-probe", dest="audio_probe", action="store_true", default=True,
                   help="inject PLA_AUDIO_PCM_LOG=1 so the audio-nonzero gate can run (default)")
    p.add_argument("--no-audio-probe", dest="audio_probe", action="store_false",
                   help="inject no probe; the audio-nonzero gate becomes skip")
    p.add_argument("--mfx", action="store_true",
                   help="B1 scene: inject TOTK_GPU_ALLOW_SUBMIT=1 TOTK_MFX=interp30 and gate the [mfx] lines")
    p.add_argument("--mfx-gpu-p95-max", type=float, default=16.7,
                   help="B1: MetalFX GPU p95 limit in ms, default 16.7")
    p.add_argument("--no-auto-input", action="store_true", help="do not inject the scripted input")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                   help="extra environment passed to run_scenario.sh (repeatable)")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.seconds < 1:
        err("baseline: --seconds must be >= 1")
        return 2
    if args.first_frame_max is None:
        args.first_frame_max = 6.0 if args.hot else 12.0
    if args.min_frames is None:
        args.min_frames = max(1, int(args.seconds * TARGET_FPS * 0.5))
    tag = args.tag or "baseline-%s" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    if not TAG_RE.match(tag):
        err("baseline: --tag must match [A-Za-z0-9._-]+: %r" % tag)
        return 2
    for e in args.env:
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", e):
            err("baseline: bad --env %r (want NAME=VALUE)" % e)
            return 2

    # Environment injected by the baseline itself (recorded in the JSON).
    injected_env = {}
    if args.audio_probe:
        injected_env["PLA_AUDIO_PCM_LOG"] = "1"
    if args.mfx:
        injected_env["TOTK_GPU_ALLOW_SUBMIT"] = "1"
        injected_env["TOTK_MFX"] = "interp30"
    user_env = dict((e.split("=", 1) for e in args.env))
    injected_env.update(user_env)

    # ---- log-only mode -----------------------------------------------------
    if args.reuse_log:
        log_path = os.path.abspath(args.reuse_log)
        if not os.path.isfile(log_path):
            err("baseline: log not found: %s" % log_path)
            return 2
        text = read_log(log_path)
        metrics = parse_log(text)
        rc_path = os.path.splitext(log_path)[0] + ".rc"
        process_rc = read_rc(rc_path)
        run_info = {
            "mode": "reuse", "tag": tag, "log": log_path,
            "log_lines": text.count("\n") + (0 if text.endswith("\n") or not text else 1),
            "log_bytes": len(text.encode("utf-8", "replace")),
            "runner_exit": None, "hot": args.hot, "min_frames": args.min_frames,
            "fps_est": None, "git": git_info(), "injected_env": injected_env,
            "mfx": bool(args.mfx),
            "started_at": None, "finished_at": None, "wall_seconds": None,
            "process_rc": process_rc,
            "process_rc_file": rc_path if process_rc is not None else None,
        }
        checks = build_checks(metrics, args, run_info)
        verdict = verdict_of(checks)
        out_path = None
        emit_json = False
        if not args.no_json and not args.dry_run:
            out_path = args.out or (os.path.splitext(log_path)[0] + ".baseline.json")
            if out_path == "-":
                out_path = None
                emit_json = True
        print_summary(metrics, checks, run_info, out_path, verdict)
        record = make_record(metrics, checks, run_info, args, out_path, verdict)
        if args.dry_run:
            out("")
            out("  dry run    log-only parse; JSON not written")
        elif emit_json:
            out("")
            out(json.dumps(record, ensure_ascii=False, indent=2))
        elif out_path:
            write_json(record, out_path)
        return 0 if verdict == "PASS" else 1

    # ---- run mode ---------------------------------------------------------
    log_path = os.path.join(args.log_dir, tag + ".log")
    runner_cmd = [RUNNER, "--tag", tag, "--seconds", str(args.seconds),
                  "--build-dir", args.build_dir, "--log-dir", args.log_dir]
    if args.no_auto_input:
        runner_cmd.append("--no-auto-input")
    for name, value in injected_env.items():
        runner_cmd += ["--env", "%s=%s" % (name, value)]

    if args.dry_run:
        out("baseline: dry run — invoking run_scenario.sh --dry-run")
        proc = subprocess.run(runner_cmd + ["--dry-run"])
        out("")
        out("baseline: would then parse %s and gate:" % log_path)
        out("  first_frame <= %.1f s, frames >= %d (至帧, increasing), p95 <= %.2f ms,"
            % (args.first_frame_max, args.min_frames, args.p95_max))
        out("  memory peak <= %d MB, crash markers == 0, audio nonzero > 0%s"
            % (args.mem_max_mb, "" if args.audio_probe else " (probe off: skipped)"))
        if args.mfx:
            out("  B1 mfx: canvas + scene + interpolator lines present, 插值帧 > 0,"
                " MetalFX GPU p95 < %.1f ms" % args.mfx_gpu_p95_max)
        return 0 if proc.returncode == 0 else 2

    if not os.access(RUNNER, os.X_OK):
        err("baseline: runner not executable: %s" % RUNNER)
        return 2

    out("baseline: running %ss (%s, %s)" % (args.seconds, "hot" if args.hot else "cold",
                                            "probe on" if injected_env.get("PLA_AUDIO_PCM_LOG") else "no probe"))
    started_at = iso_now()
    t0 = time.time()
    proc = subprocess.run(runner_cmd)
    wall_seconds = round(time.time() - t0, 3)
    finished_at = iso_now()
    runner_exit = proc.returncode
    if not os.path.isfile(log_path):
        err("baseline: run produced no log at %s (runner exit %s)" % (log_path, runner_exit))
        return 2

    text = read_log(log_path)
    metrics = parse_log(text)
    rc_path = os.path.join(args.log_dir, tag + ".rc")
    process_rc = read_rc(rc_path)
    run_info = {
        "mode": "run", "tag": tag, "log": log_path,
        "log_lines": text.count("\n") + (0 if text.endswith("\n") or not text else 1),
        "log_bytes": len(text.encode("utf-8", "replace")),
        "runner_exit": runner_exit, "hot": args.hot, "min_frames": args.min_frames,
        "fps_est": None, "git": git_info(), "injected_env": injected_env,
        "mfx": bool(args.mfx),
        "seconds": args.seconds,
        "started_at": started_at, "finished_at": finished_at,
        "wall_seconds": wall_seconds, "process_rc": process_rc,
        "process_rc_file": rc_path if process_rc is not None else None,
    }
    if metrics["frames_presented_total"]:
        run_info["fps_est"] = metrics["frames_presented_total"] / float(args.seconds)

    checks = build_checks(metrics, args, run_info)
    verdict = verdict_of(checks)
    out_path = None
    if not args.no_json:
        out_path = args.out or os.path.join(args.log_dir, tag + ".baseline.json")
    if out_path == "-":
        out_path = None
        emit_stdout_json = True
    else:
        emit_stdout_json = False

    print_summary(metrics, checks, run_info, out_path, verdict)
    record = make_record(metrics, checks, run_info, args, out_path, verdict)
    if emit_stdout_json:
        out("")
        out(json.dumps(record, ensure_ascii=False, indent=2))
    elif out_path:
        write_json(record, out_path)
    if runner_exit == 3:
        return 3
    return 0 if verdict == "PASS" else 1


def make_record(metrics, checks, run_info, args, out_path, verdict):
    return {
        "schema": SCHEMA,
        "created_at": iso_now(),
        "verdict": verdict,
        "git": run_info["git"],
        "run": {
            "mode": run_info["mode"],
            "tag": run_info["tag"],
            "seconds": run_info.get("seconds"),
            "log": run_info["log"],
            "log_lines": run_info["log_lines"],
            "log_bytes": run_info["log_bytes"],
            "runner_exit": run_info["runner_exit"],
            "started_at": run_info.get("started_at"),
            "finished_at": run_info.get("finished_at"),
            "wall_seconds": run_info.get("wall_seconds"),
            "process_rc": run_info.get("process_rc"),
            "process_rc_file": run_info.get("process_rc_file"),
            "hot": run_info["hot"],
            "injected_env": run_info["injected_env"],
            "json": out_path,
        },
        "gates": {
            "first_frame_max_s": args.first_frame_max,
            "min_frames": args.min_frames,
            "p95_max_ms": args.p95_max,
            "p95_worst_max_ms": args.p95_worst_max or None,
            "mem_max_mb": args.mem_max_mb,
            "audio_probe": bool(args.audio_probe),
            "mfx": bool(args.mfx),
            "mfx_gpu_p95_max_ms": args.mfx_gpu_p95_max if args.mfx else None,
        },
        "metrics": metrics,
        "checks": checks,
        "failures": [c["name"] for c in checks if c["ok"] is False],
    }


def write_json(record, path):
    directory = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(directory, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(record, f, ensure_ascii=False, indent=2)
            f.write("\n")
    except OSError as e:
        err("baseline: cannot write %s: %s" % (path, e))


if __name__ == "__main__":
    sys.exit(main())

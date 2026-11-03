#!/usr/bin/env python3
"""Cuts one long recording made on another machine (the card's S/PDIF or
line output, recorded while eaxcap.exe ran) into the per-case files of an
eaxcap.exe output directory, so analyze.py can use it in place of the
"What U Hear" recordings.

    python3 -I split_capture.py --long spdif.wav --capture capture --out capture_spdif

--capture is eaxcap.exe's output directory (all its provider directories,
recorded in one long file) or one provider directory. The manifests give
the order of the recordings (start_tick_ms), their lengths and where the
sync click sat in each; every recording starts with at least half a
second of silence and then the click, so the long file is cut at each
click found after a silence.
"""

import argparse
import json
import os
import shutil
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import load_manifest, read_wav  # noqa: E402


def pcm16_map(path):
    """(rate, frames x channels int16 memmap) of a 16-bit PCM WAV, else None."""
    with open(path, "rb") as f:
        head = f.read(1 << 16)
    pos = 12
    fmt = None
    while pos + 8 <= len(head):
        cid, size = head[pos:pos + 4], struct.unpack_from("<I", head, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", head, pos + 8)
        elif cid == b"data":
            if not fmt or fmt[0] != 1 or fmt[5] != 16:
                return None
            ch = fmt[1]
            n = size // (2 * ch)
            return fmt[2], np.memmap(path, "<i2", "r", pos + 8, (n, ch))
        pos += 8 + size + (size & 1)
    return None


def write_wav16(path, x, rate):
    data = np.clip(np.round(x * 32768), -32768, 32767).astype("<i2").tobytes()
    ch = x.shape[1]
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, ch, rate, rate * 2 * ch, 2 * ch, 16) +
                b"data" + struct.pack("<I", len(data)) + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--long", required=True, help="the long recording (WAV)")
    ap.add_argument("--capture", required=True,
                    help="eaxcap.exe's output directory, or one provider directory in it")
    ap.add_argument("--out", required=True)
    ap.add_argument("--silence", type=float, default=0.3,
                    help="seconds of silence required before a click (default 0.3)")
    a = ap.parse_args()

    m = pcm16_map(a.long)
    if m:
        rate, raw = m
        scale = 1 / 32768.0
    else:
        rate, raw = read_wav(a.long)
        scale = 1.0
    if raw.shape[1] < 2:
        raw = np.repeat(raw[:, :1], 2, axis=1)
    raw = raw[:, :2]

    # Every provider directory's manifests, in recording order.
    if any(n.endswith(".json") and not n.startswith("run_") for n in os.listdir(a.capture)):
        dirs = [("", a.capture)]
    else:
        dirs = [(d, os.path.join(a.capture, d)) for d in sorted(os.listdir(a.capture))
                if os.path.isdir(os.path.join(a.capture, d))]
    manifests = []
    for sub, d in dirs:
        for name in os.listdir(d):
            if name.endswith(".json"):
                meta = load_manifest(os.path.join(d, name))
                if "start_tick_ms" in meta:
                    manifests.append((meta["start_tick_ms"], sub, name[:-5], meta))
    manifests.sort(key=lambda m: m[0])
    if not manifests:
        print("no eaxcap.exe manifests in %s" % a.capture, file=sys.stderr)
        return 1
    if manifests[0][3]["rate"] != rate:
        print("the long recording is %d Hz, the capture %d Hz" % (rate, manifests[0][3]["rate"]),
              file=sys.stderr)
        return 1

    for sub, d in dirs:
        os.makedirs(os.path.join(a.out, sub), exist_ok=True)
        for name in os.listdir(d):
            if name.startswith("stim_"):
                shutil.copy(os.path.join(d, name), os.path.join(a.out, sub, name))

    # Threshold: well above the noise at the start of the long file.
    block = 4096
    noise = float(np.abs(raw[:rate // 5]).max()) * scale
    thr = max(noise * 8, 10 ** (-60 / 20))
    need = int(a.silence * rate)
    pos = 0
    for _, sub, base, meta in manifests:
        click = None
        quiet = 0
        while pos < len(raw) and click is None:
            chunk = np.abs(raw[pos:pos + block]).max(axis=1) * scale
            loud = np.nonzero(chunk > thr)[0]
            if not len(loud):
                quiet += len(chunk)
            elif quiet + int(loud[0]) >= need:
                click = pos + int(loud[0])
            else:
                quiet = len(chunk) - 1 - int(loud[-1])
            pos += len(chunk) if click is None else 0
        if click is None:
            print("ran out of recording at %s" % base, file=sys.stderr)
            return 1
        at = meta.get("sync_frame_found") or int((meta["preroll_s"] + 0.1) * rate)
        start = click - at
        n = int(meta["record_frames"])
        seg = np.zeros((n, 2))
        lo = max(start, 0)
        seg[lo - start:lo - start + min(n, len(raw) - lo)] = raw[lo:lo + n] * scale
        write_wav16(os.path.join(a.out, sub, base + ".wav"), seg, rate)
        meta = dict(meta, wavein="split from " + os.path.basename(a.long),
                    split_click_frame=click)
        with open(os.path.join(a.out, sub, base + ".json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
        print("%-50s click at %8.3f s" % (os.path.join(sub, base), click / rate))
        pos = start + n
    return 0


if __name__ == "__main__":
    sys.exit(main())

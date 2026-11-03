"""Convert LM3_PASS_DUMP raw render targets (<dir>/*.bin) into PNGs next to them (HDR formats clamped to 0..1).
usage: pass_png.py <dir>"""
import re, sys
from pathlib import Path
import numpy as np
from PIL import Image
def f11(v, bits):   # unsigned float with 5-bit exponent (R11G11B10)
    m, e = v & ((1 << (bits - 5)) - 1), (v >> (bits - 5)) & 31
    return np.where(e == 0, m / (1 << (bits - 5)) * 2.0 ** -14, (1 + m / (1 << (bits - 5))) * 2.0 ** (e.astype(np.float64) - 15))
for path in sorted(Path(sys.argv[1]).glob('*.bin')):
    m = re.search(r'_(\d+)x(\d+)_pf(\d+)_', path.name); w, h, pf = map(int, m.groups()); raw = path.read_bytes()
    if pf in (70, 71, 80, 81): a = np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., :3]; a = a[..., ::-1] if pf in (80, 81) else a; img = a
    elif pf == 92: v = np.frombuffer(raw, np.uint32).reshape(h, w); img = np.stack([f11(v & 2047, 11), f11((v >> 11) & 2047, 11), f11(v >> 22, 10)], -1)
    elif pf == 90: v = np.frombuffer(raw, np.uint32).reshape(h, w); img = np.stack([(v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023], -1)
    elif pf == 115: img = np.frombuffer(raw, np.float16).reshape(h, w, 4)[..., :3].astype(np.float64)
    elif pf == 65: v = np.frombuffer(raw, np.float16).reshape(h, w, 2).astype(np.float64); img = np.stack([v[..., 0], v[..., 1], np.zeros((h, w))], -1)
    elif pf in (25, 55): img = np.frombuffer(raw, np.float16 if pf == 25 else np.float32).reshape(h, w).astype(np.float64)
    elif pf == 10: img = np.frombuffer(raw, np.uint8).reshape(h, w)
    elif pf == 20: img = np.frombuffer(raw, np.uint16).reshape(h, w) / 65535
    elif pf == 30: v = np.frombuffer(raw, np.uint8).reshape(h, w, 2); img = np.stack([v[..., 0], v[..., 1], np.zeros((h, w), np.uint8)], -1)
    else: continue
    if img.dtype != np.uint8: img = (np.clip(np.nan_to_num(img), 0, 1) * 255).astype(np.uint8)
    Image.fromarray(img).save(path.with_suffix('.png'))

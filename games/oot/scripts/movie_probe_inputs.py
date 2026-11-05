#!/usr/bin/env python3
"""Inputs for probe-movie: the blood-moon cutscene from the verified RomFS (extracted/v0/romfs), its VP9 frames as IVF
(ffmpeg -c copy: the samples the game's extractor hands the decoder) and ffmpeg's own decode as packed NV12 (ffmpeg's
VP9 decoder, independent of libvpx). Writes <out>/frames.ivf, <out>/reference.nv12, <out>/meta.txt (width height frames).
usage: movie_probe_inputs.py <out dir> [RomFS path inside, default Event/Movie/DmT_SY_BloodyMoon_PreRender.webm] [first N frames]
With N, both the IVF and the reference stop after the first N frames (scripts/diag/movie_probe_all.sh: every movie, raw NV12
of a whole 1080p movie runs to gigabytes)."""
import json, os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]); name = sys.argv[2] if len(sys.argv) > 2 else 'Event/Movie/DmT_SY_BloodyMoon_PreRender.webm'
first = ['-frames:v', sys.argv[3]] if len(sys.argv) > 3 else []
FFMPEG, FFPROBE = '/opt/homebrew/bin/ffmpeg', '/opt/homebrew/bin/ffprobe'
for tool in (FFMPEG, FFPROBE):
    if not os.access(tool, os.X_OK): raise SystemExit(f'{tool} missing (brew install ffmpeg): probe-movie compares against its VP9 decoder')
out.mkdir(parents=True, exist_ok=True)
webm = out / 'movie.webm'
if not (out / 'meta.txt').exists():
    index = json.load(open(ROOT / 'extracted/v0/romfs/index.json'))
    entry = next((f for f in index['files'] if f['path'] == name), None)
    if not entry: raise SystemExit(f'{name} not in the RomFS index')
    with open(ROOT / 'extracted/v0/romfs/romfs.bin', 'rb') as f:
        f.seek(entry['offset']); webm.write_bytes(f.read(entry['size']))
    subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', str(webm), '-c:v', 'copy', *first, '-f', 'ivf', str(out / 'frames.ivf')], check=True)
    subprocess.run([FFMPEG, '-v', 'error', '-y', '-c:v', 'vp9', '-i', str(webm), *first, '-f', 'rawvideo', '-pix_fmt', 'nv12', str(out / 'reference.nv12')], check=True)
    w, h = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', str(webm)],
                          check=True, capture_output=True, text=True).stdout.strip().split(',')
    frames = (out / 'reference.nv12').stat().st_size // (int(w) * int(h) * 3 // 2)
    (out / 'meta.txt').write_text(f'{w} {h} {frames}\n')
print((out / 'meta.txt').read_text().strip())

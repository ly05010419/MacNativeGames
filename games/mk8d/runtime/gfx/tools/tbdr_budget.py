#!/usr/bin/env python3
# Per-frame attachment bandwidth budget table (target 120 fps, ≤ 20 GB/s → ≤ 170 MB per frame): from one frame of the DM_NVN_PASSDUMP data stream, list load / store MB per pass,
# sorted by their sum in descending order. Load is counted from P rows with load (clear / dontcare not counted); store is counted from the P rows' attachments, minus those the Z rows judge dontcare (store lookahead).
# Combined depth-stencil formats (fmt 260 = Depth32Float_Stencil8) are split as depth 4 + stencil 1 bytes: d rows record the depth plane, s rows (stencil-only attachment) record the stencil plane.
# Texture sampling reads (supplementary column, rough estimate):
#   fullscreen-class passes (draw vertex count ≤ 6 and ≤ 3 draws; draw count from the d rows of PASSDUMP_ALL, or else the number of fragment program binding rows): for each sampled bound view, min(whole bound view bytes, target pixel count × bytes per texel);
#   geometry-class passes: target pixel count × number of texture slots declared by the fragment shaders in the pass (take the max) × average bytes per texel of the sampled textures (about one sample per pixel per slot; material textures have mips).
#   Texture format and size come from the B row of each descriptor in the same frame and pass (the same serial can bind views of different levels / layers); 4 bytes if not found; slot counts come from the B library reflection json.
# Usage: tbdr_budget.py <log containing PASSDUMP> [frame number (default: first frame)] [budget MB=170] [--tsv=path of the full table]
# The TSV is not truncated to the top 25 / shader keys; it carries per-draw sources, the unknown-texture denominator and merged-pass notes. The texture column is always a rough binding-model estimate, not an external-memory measurement.
import re, sys, collections
BPP = {70: 4, 71: 4, 80: 4, 81: 4, 90: 4, 92: 4, 93: 4, 115: 8, 125: 16, 55: 4, 25: 2, 65: 4, 105: 4, 103: 4, 252: 4, 250: 2, 260: 5, 255: 4, 10: 1, 20: 2, 30: 2, 60: 4, 53: 4}
TSV = next((a[6:] for a in sys.argv[1:] if a.startswith('--tsv=')), None)
args = [a for a in sys.argv[1:] if not a.startswith('--tsv=')]
log = args[0]; FR = args[1] if len(args) > 1 else None; BUDGET = float(args[2]) if len(args) > 2 else 170
P = collections.OrderedDict(); dead = set(); shader = collections.defaultdict(collections.Counter)
TBPP = {10: 1, 20: 2, 25: 2, 30: 2, 53: 4, 55: 4, 60: 4, 65: 8, 70: 4, 71: 4, 80: 4, 81: 4, 90: 4, 92: 4, 93: 4, 103: 8, 105: 8, 115: 8, 125: 16,
        130: .5, 131: .5, 132: 1, 133: 1, 134: 1, 135: 1, 140: .5, 141: .5, 142: 1, 143: 1, 150: 1, 151: 1, 152: 1, 153: 1, 250: 2, 252: 4, 255: 4, 260: 4}
texinfo = collections.defaultdict(dict); sampled = collections.defaultdict(set); tgt = {}; maxvc = collections.defaultdict(int); ndraw = collections.defaultdict(int); fskeys = collections.defaultdict(set); fused = set(); att_desc = collections.defaultdict(list)
import os, json; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)
LIB = os.path.join(DM_ROOT, f'build/cache/metal_shader_cache/b-{dmsh_chain.version()}')
def tex_slots(key):
    try: return sum(1 for b in json.load(open(f'{LIB}/{key}_b.json'))['bindings'] if b.get('kind') in ('tex', 'samp_tex'))
    except Exception: return 4
nm = {}
for ln in open(dmsh_chain.load_game().table('shader_map.tsv')):   # the current game profile's map (DM_GAME)
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): nm.setdefault(c[3], c[0][:-4])
for l in open(log, errors='replace'):
    m = re.search(r'\[pass\] F(\d+) (\w) (\d+) (.*)', l)
    if not m: continue
    f, k, p, r = m.groups()
    if FR is None: FR = f
    if f != FR: continue
    p = int(p); e = P.setdefault(p, {'att': [], 'load': 0, 'store': 0})
    if k == 'P':
        a = re.match(r'(c\d+|d|s) ser=(\d+) fmt=(\d+) (\d+)x(\d+)(?:.*? lv(\d+) sl(\d+))?.*?\b(clear|load|dontcare)', r)
        if not a: continue
        slot, ser, fmt, w, h, lv, sl, act = a.groups(); fmt = int(fmt); lv = int(lv or 0)
        b = max(int(w) >> lv, 1) * max(int(h) >> lv, 1) * BPP.get(fmt, 4)
        if fmt == 260: b = b // 5 * (4 if slot == 'd' else 1)
        e['att'].append((slot, int(ser), lv, int(sl or 0), b, act))
        att_desc[p].append(r)
        tgt.setdefault(p, max(int(w) >> lv, 1) * max(int(h) >> lv, 1))
    elif k == 'B':
        b = re.match(r'st\d+ (?:tex|tbuf) reg\d+ tid=(\d+) real ser=(\d+) fmt=(\d+) (\d+)x(\d+)x(\d+)', r)
        if b:
            tid, ser, fmt, w, h, dd = map(int, b.groups())
            texinfo[p].setdefault(ser, set()).add((tid, fmt, w, h, dd))
    elif k == 'R':
        rr = re.match(r'st\d+ ser=(\d+)', r)
        if rr: sampled[p].add(int(rr.group(1)))
    elif k == 'Z':
        z = re.match(r'(c|d|s) ser=(\d+) lv(\d+) sl(\d+) (dontcare|store)', r)
        if z and z.group(5) == 'dontcare': dead.add((p, z.group(1), int(z.group(2)), int(z.group(3)), int(z.group(4))))
    elif k == 'F' and 'merge with the next pass' in r: fused.add(p)
    elif k == 'K' and ' st1 ' in f' {r} ':
        key = r.split('key=')[1].split()[0].replace('_b', ''); shader[p][nm.get(key, key[:8])] += 1; fskeys[p].add(key)
    elif k in ('D', 'd'):   # d rows (PASSDUMP_ALL) are one per draw, so draws can be counted; D rows only have the first of each pass
        v = re.search(r'vc=(\d+)', r)
        if v: maxvc[p] = max(maxvc[p], int(v.group(1)))
        if k == 'd': ndraw[p] += 1
ALLD = any(ndraw.values())   # classify by draw count when per-draw rows exist, otherwise by the number of fragment program binding rows (when only the first is recorded, many small quads are misclassified as fullscreen class)
rows = []
for p, e in P.items():
    ld = sum(b for *_, b, act in e['att'] if act == 'load')
    st = sum(b for slot, ser, lv, sl, b, act in e['att'] if (p, slot[0], ser, lv, sl) not in dead)
    px = tgt.get(p, 0); tx = 0
    nd = ndraw[p] if ALLD else sum(shader[p].values())
    if maxvc[p] <= 6 and nd <= 3:   # fullscreen class (draw vertices ≤ 6 and ≤ 3 draws; many small quads such as UI count as geometry class)
        for ser in sampled[p]:
            for tid, f, w, h, dd in texinfo[p].get(ser, {(-1, None, 0, 0, 1)}):
                tx += min(w * h * dd * TBPP.get(f, 4), px * TBPP.get(f, 4)) if f is not None else px * 4
    elif sampled[p]:    # geometry class
        bp = [TBPP.get(v[1], 4) for s_ in sampled[p] for v in texinfo[p].get(s_, ())] or [4]
        tx = px * max([tex_slots(k_) for k_ in fskeys[p]] or [4]) * (sum(bp) / len(bp))
    rows.append((ld + st + tx, ld, st, tx, p, ' '.join(f'{slot}:{ser}' + (f'/lv{lv}' if lv else '') for slot, ser, lv, sl, b, act in e['att']), ', '.join(shader[p])[:40]))
tl = sum(r[1] for r in rows) / 1e6; ts = sum(r[2] for r in rows) / 1e6; tt = sum(r[3] for r in rows) / 1e6
print(f'frame {FR}: {len(rows)} passes; attachments load {tl:.1f} + store {ts:.1f} = {tl + ts:.1f} MB (budget {BUDGET:.0f}, over by {tl + ts - BUDGET:+.1f}); texture sampling (rough estimate{"" if ALLD else ", no per-draw rows, coarse classification"}) {tt:.1f} MB; total bandwidth {tl + ts + tt:.1f} MB')
print('total MB | att. load | att. store | tex. sampling | pass | attachments | main fragment shaders')
acc = 0
for t, ld, st, tx, p, att, sh in sorted(rows, reverse=True)[:25]:
    acc += t; print(f'{t / 1e6:6.1f} | {ld / 1e6:5.1f} | {st / 1e6:5.1f} | {tx / 1e6:5.1f} | {p} | {att[:52]} | {sh}   (cumulative {acc / 1e6:.0f})')
if TSV:
    import csv
    with open(TSV, 'w', encoding='utf-8', newline='') as out:
        wr = csv.writer(out, delimiter='\t')
        wr.writerow(['frame', 'render_order', 'pass', 'draws', 'draw_source', 'sampling_model', 'target_pixels', 'load_bytes', 'store_bytes', 'texture_est_bytes', 'total_est_bytes', 'unknown_textures', 'unknown_formats', 'texture_views', 'sampled_serials', 'fragment_keys', 'fragment_names', 'attachments', 'attachment_details', 'note'])
        for order, (total, ld, st, tx, p, att, _) in enumerate(rows):
            nd = ndraw[p] if ALLD else sum(shader[p].values())
            source = 'PASSDUMP_ALL' if ndraw[p] else 'clear_only' if not maxvc[p] else 'shader_bindings'
            model = 'small_draw' if maxvc[p] <= 6 and nd <= 3 else 'geometry'
            views = [v for s_ in sampled[p] for v in texinfo[p].get(s_, ())]
            wr.writerow([FR, order, p, nd, source, model, tgt.get(p, 0), int(ld), int(st), int(tx), int(total),
                         len(sampled[p] - texinfo[p].keys()), sum(v[1] not in TBPP for v in views), len(views), ','.join(map(str, sorted(sampled[p]))), ','.join(sorted(fskeys[p])), ','.join(shader[p]), att, '; '.join(att_desc[p]),
                         'binding-model rough estimate; includes tile binding; not a memory-traffic measurement' if p in fused else 'binding-model rough estimate; not a memory-traffic measurement'])

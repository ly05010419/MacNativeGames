#!/usr/bin/env python3
"""Build the RomFS overlay a bigger game output needs ("true 2K", TOTK_OUTPUT_RES=<w>x<h>, runtime/game_patches.c).

The game composes its frame in a canvas of the output size; the canvas-sized render targets come from
agl::utl::DynamicTextureAllocator, a single fixed heap that cannot grow (main+0x8e1a98). Its size and the size of the
heap it is carved from are data: System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs, "Heaps" -> name -> "Size" in KiB,
looked up by name (main+0x1123b70 -> main+0x8119b0):
  DynamicTextureAllocatorMem  113664 KiB (111 MiB), parent Graphics
  Graphics                    201728 KiB (197 MiB), the heap of gsys::SystemTask (its getHeapName, main+0x2a7cb80)
At 2560x1440 the title already runs out of the texture heap (crash at main+0x799390). This overlay scales
DynamicTextureAllocatorMem with the output pixels (rounded up to whole MiB) and grows Graphics by the same amount; the
runtime adds that amount to the application memory pool it reports (runtime/services.c), so no other heap shrinks. The
values are the same formula as runtime_output_res_extra_bytes() (runtime/game_patches.c): keep the two in step.

--scene <w>x<h> (1.4.3, game_patches_v143.c): the same allocator also holds the 3D scene's render targets (1600x900 docked
in the shipped heap). A scene rendered at the output size ("native 2K" renders the scene at 2560x1440) needs the heap
scaled with the scene pixels instead: 1.35 x (the scene / 1600x900 pixel ratio + the canvas pixels beyond 2560x1440 /
1920x1080), measured 2026-10-19: 384 MiB for a 2560x1440 scene and canvas loaded and played; a 3840x2160 scene and
canvas ran out of 640 MiB at once and of 1 GiB after turning the camera (this gives it 1.2 GiB; the 3200x1800 and
3840x2160 sizes are for Macs with 24 / 32 GB and were not played on the 16 GB test Mac). The manifest's header records
the scene size and the added KiB; the 1.4.3 runtime takes both from there instead of repeating the formula.

Input: the IVFC-verified local RomFS image (extracted/v0/romfs). Output: extracted/v0/romfs-patches-out/<w>x<h>/<path>
and manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original); game content stays in the ignored folder.
usage: make_output_res_overlay.py 2560x1440 [--scene 2560x1440]
"""
import argparse, hashlib, json, math, struct
from pathlib import Path
import compression.zstd as zstd

BASE_W, BASE_H = 1920, 1080
HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
DTA_KIB, GRAPHICS_KIB = 113664, 201728   # the shipped values, checked below

SCENE_W, SCENE_H, SCENE_MARGIN = 1600, 900, 1.35   # the shipped docked scene size; see --scene above

def extra_kib(w, h, scene=None):
    """KiB added to DynamicTextureAllocatorMem (and to Graphics, and to the application pool) for a w x h output."""
    ratio = w * h / (BASE_W * BASE_H)
    if scene: ratio = max(ratio, (scene[0] * scene[1] / (SCENE_W * SCENE_H) + max(0, w * h - 2560 * 1440) / (BASE_W * BASE_H)) * SCENE_MARGIN)
    if ratio <= 1: return 0
    return math.ceil(DTA_KIB * ratio / 1024) * 1024 - DTA_KIB

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('size', help='<width>x<height>, e.g. 2560x1440')
parser.add_argument('--output-root', type=Path, default=root / 'extracted/v0/romfs-patches-out')
parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
parser.add_argument('--scene', help='<width>x<height>: also room for a 3D scene of this size (1.4.3)')
args = parser.parse_args()
version = args.version
if args.output_root == root / 'extracted/v0/romfs-patches-out': args.output_root = root / f'extracted/{version}/romfs-patches-out'
w, h = (int(v) for v in args.size.lower().split('x'))
assert 640 <= w <= 7680 and 360 <= h <= 4320, 'size out of range'
scene = tuple(int(v) for v in args.scene.lower().split('x')) if args.scene else None
assert not scene or (64 <= scene[0] <= 4096 and 64 <= scene[1] <= 4096), 'scene size out of range'
manifest = json.loads((root / f'extracted/{version}/romfs/index.json').read_text())
assert manifest['status'] == 'pass'
files = {f['path']: f for f in manifest['files']}

def sarc_members(data):
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out

with (root / f'extracted/{version}/romfs/romfs.bin').open('rb') as image:
    def read(path): f = files[path]; image.seek(f['offset']); b = image.read(f['size']); assert len(b) == f['size']; return b
    dictionaries = {}
    pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
    for name, (lo, hi) in sarc_members(pack).items():
        d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
    raw = read(HEAPDEF)
info = zstd.get_frame_info(raw)
dictionary = dictionaries[info.dictionary_id] if info.dictionary_id else None   # HeapDef is compressed without a dictionary
byml = bytearray(zstd.decompress(raw, zstd_dict=dictionary)); assert len(byml) == info.decompressed_size

# BYML v7, little endian ("YB"): hash nodes 0xC1 {u24 key index, u8 type, u32 value}; ints (0xD1) are stored inline.
assert byml[:2] == b'YB' and struct.unpack_from('<H', byml, 2)[0] == 7
key_table, _, root_node = struct.unpack_from('<III', byml, 4)
def string_table(off):
    n = struct.unpack_from('<I', byml, off)[0] >> 8
    offs = struct.unpack_from(f'<{n + 1}I', byml, off + 4)
    return [byml[off + o: byml.index(0, off + o)].decode() for o in offs[:n]]
keys = string_table(key_table)
def field(node, name, want_type):
    """Byte offset of the value field of hash node `node`'s entry `name` (its type must be want_type)."""
    assert byml[node] == 0xc1
    for i in range(struct.unpack_from('<I', byml, node)[0] >> 8):
        e = node + 4 + 8 * i
        if keys[struct.unpack_from('<I', byml, e)[0] & 0xffffff] == name:
            assert byml[e + 3] == want_type, (name, byml[e + 3])
            return e + 4
    raise KeyError(name)
def child(node, name): return struct.unpack_from('<I', byml, field(node, name, 0xc1))[0]
heaps = child(root_node, 'Heaps')
def size_field(heap): return field(child(heaps, heap), 'Size', 0xd1)
dta_at, graphics_at = size_field('DynamicTextureAllocatorMem'), size_field('Graphics')
dta, graphics = (struct.unpack_from('<i', byml, a)[0] for a in (dta_at, graphics_at))
assert (dta, graphics) == (DTA_KIB, GRAPHICS_KIB), (dta, graphics)
extra = extra_kib(w, h, scene)
assert extra > 0, 'the output is not bigger than 1920x1080: no overlay needed'
struct.pack_into('<i', byml, dta_at, dta + extra)
struct.pack_into('<i', byml, graphics_at, graphics + extra)
original = zstd.decompress(raw, zstd_dict=dictionary)
diff = [i for i in range(len(original)) if original[i] != byml[i]]
assert diff and all(dta_at <= i < dta_at + 4 or graphics_at <= i < graphics_at + 4 for i in diff), diff
data = zstd.compress(bytes(byml), level=19, zstd_dict=dictionary)
assert zstd.decompress(data, zstd_dict=dictionary) == bytes(byml)
assert zstd.get_frame_info(data).dictionary_id == info.dictionary_id and zstd.get_frame_info(data).decompressed_size == len(byml)
out_dir = args.output_root / f'{w}x{h}'
target = out_dir / HEAPDEF; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
line = '\t'.join([HEAPDEF, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()])
header = f'# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_output_res_overlay.py {w}x{h})'
if scene: header += f' scene={scene[0]}x{scene[1]} extra_kib={extra}'
(out_dir / 'manifest.tsv').write_text(header + '\n' + line + '\n')
print(json.dumps(dict(status='pass', output=f'{w}x{h}', scene=f'{scene[0]}x{scene[1]}' if scene else None, path=HEAPDEF, extra_kib=extra,
                      DynamicTextureAllocatorMem_kib=[dta, dta + extra], Graphics_kib=[graphics, graphics + extra],
                      size=len(data), changed_bytes=len(diff), dir=str(out_dir))))

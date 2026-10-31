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
values are the same formula as runtime_output_res_extra_bytes() (runtime/game_patches.c; with --scene, 1.4.3 only:
runtime/game_patches_v143.c): keep the two in step.

Input: the IVFC-verified local RomFS image (extracted/v0/romfs). Output: extracted/v0/romfs-patches-out/<w>x<h>/<path>
and manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original); game content stays in the ignored folder.

--heapdef <file.zs> uses a player mod's replacement of the same file as the base, so the overlay carries the mod's own
heap sizes with the 2K sizes added on top (build_app.py does this with --mod). The manifest still names the RomFS
original, and the player's mod manifest must leave this file to the app, or the game stops with "two overlays replace
the same file".

overlay.json next to the manifest records what the overlay was made from (the script's own SHA-256, the output size,
the scene size, and the SHA-256 of the heap table it was built on, or null for the RomFS original), so build_app.py can
tell a current overlay from one a --mod build left behind - the mod's heap sizes must not stay in a later build that
passes no --mod.
usage: make_output_res_overlay.py 2560x1440 [--heapdef /path/to/mod/System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs]
"""
import argparse, hashlib, json, math, struct
from pathlib import Path
import compression.zstd as zstd

BASE_W, BASE_H = 1920, 1080
HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
DTA_KIB, GRAPHICS_KIB = 113664, 201728   # the shipped values, checked below

SCENE_W, SCENE_H = 1600, 900   # the game's docked scene render size (TOTK_RENDER_RES, 1.4.3)

def extra_kib(w, h, sw=0, sh=0):
    """KiB added to DynamicTextureAllocatorMem (and to Graphics, and to the application pool) for a w x h output and an
    optional sw x sh scene: each grows the heap by its pixel ratio above the shipped size, a bigger scene with a 1.6
    margin (runtime_output_res_extra_bytes, runtime/game_patches_v143.c)."""
    scene = sw * sh > SCENE_W * SCENE_H
    grow = max(0, w * h / (BASE_W * BASE_H) - 1) + (sw * sh / (SCENE_W * SCENE_H) - 1 if scene else 0)
    if grow <= 0: return 0
    if scene: grow *= 1.6
    return math.ceil(DTA_KIB * (1 + grow) / 1024) * 1024 - DTA_KIB

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('size', help='<width>x<height>, e.g. 2560x1440')
parser.add_argument('--output-root', type=Path, default=root / 'extracted/v0/romfs-patches-out')
parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
parser.add_argument('--scene', help='<width>x<height> scene render size (TOTK_RENDER_RES, 1.4.3 / 1.2.1) the heaps must also hold')
parser.add_argument('--heapdef', type=Path,
                    help='a mod\'s replacement HeapDef(.zs) to build on: its heap sizes are kept, the 2K sizes added')
args = parser.parse_args()
version = args.version
if args.output_root == root / 'extracted/v0/romfs-patches-out': args.output_root = root / f'extracted/{version}/romfs-patches-out'
w, h = (int(v) for v in args.size.lower().split('x'))
sw, sh = (int(v) for v in args.scene.lower().split('x')) if args.scene else (0, 0)
scene_dir = f'-scene-{sw}x{sh}' if sw * sh > SCENE_W * SCENE_H else ''
assert 640 <= w <= 7680 and 360 <= h <= 4320, 'size out of range'
assert not args.scene or version in ('v143', 'v121'), '--scene: 1.4.3 and 1.2.1 render their scene at TOTK_RENDER_RES'
assert not args.scene or (64 <= sw <= 4096 and 64 <= sh <= 4096), 'scene size out of range (TOTK_RENDER_RES: 64..4096)'
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
    romfs_raw = read(HEAPDEF)       # the manifest's original_sha256 column always names the RomFS file
mod_raw = args.heapdef.read_bytes() if args.heapdef else romfs_raw
try:
    info = zstd.get_frame_info(mod_raw)
    dictionary = dictionaries[info.dictionary_id] if info.dictionary_id else None   # HeapDef is compressed without one
    byml = bytearray(zstd.decompress(mod_raw, zstd_dict=dictionary))
except Exception as error:          # a mod file that is not a HeapDef at all
    raise SystemExit(f'{args.heapdef}: not a zstd-compressed HeapDef ({error})')
assert len(byml) == info.decompressed_size, args.heapdef or 'the RomFS HeapDef'
base = bytes(byml)

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
if not args.heapdef:   # the shipped table is the reference; a mod's table may already have changed other heaps
    assert (dta, graphics) == (DTA_KIB, GRAPHICS_KIB), (dta, graphics)
extra = extra_kib(w, h, sw, sh)
assert extra > 0, 'neither the output nor the scene is bigger than the shipped size: no overlay needed'
struct.pack_into('<i', byml, dta_at, dta + extra)
struct.pack_into('<i', byml, graphics_at, graphics + extra)
diff = [i for i in range(len(base)) if base[i] != byml[i]]
assert diff and all(dta_at <= i < dta_at + 4 or graphics_at <= i < graphics_at + 4 for i in diff), diff
data = zstd.compress(bytes(byml), level=19, zstd_dict=dictionary)
assert zstd.decompress(data, zstd_dict=dictionary) == bytes(byml)
written = zstd.get_frame_info(data)
assert written.dictionary_id == info.dictionary_id and written.decompressed_size == len(byml)
out_dir = args.output_root / f'{w}x{h}{scene_dir}'
target = out_dir / HEAPDEF; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
line = '\t'.join([HEAPDEF, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(romfs_raw).hexdigest()])
(out_dir / 'manifest.tsv').write_text(f'# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_output_res_overlay.py {w}x{h}{" --scene " + args.scene if args.scene else ""}{" --heapdef " + str(args.heapdef) if args.heapdef else ""})\n' + line + '\n')
(out_dir / 'overlay.json').write_text(json.dumps(dict(
    script='make_output_res_overlay.py', script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    version=version, size=f'{w}x{h}', scene=args.scene or None, extra_kib=extra,
    heapdef=str(args.heapdef) if args.heapdef else None,
    heapdef_sha256=hashlib.sha256(mod_raw).hexdigest() if args.heapdef else None), indent=2) + '\n')
print(json.dumps(dict(status='pass', output=f'{w}x{h}', path=HEAPDEF, extra_kib=extra,
                      base=str(args.heapdef) if args.heapdef else 'the RomFS original',
                      DynamicTextureAllocatorMem_kib=[dta, dta + extra], Graphics_kib=[graphics, graphics + extra],
                      size=len(data), changed_bytes=len(diff), dir=str(out_dir))))

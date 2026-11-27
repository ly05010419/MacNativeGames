#!/usr/bin/env python3
"""The game's own item icons for the VR item wheel (zeldaVR/gfx/oot_vr_wheel_draw.h), made from the player's own RomFS.

The pause menu's icons are UI/Tex/Icon/<name>.bntx.zs: one BNTX texture each, zstd without a dictionary, ASTC 4x4 sRGB
(format 0x2d06), block-linear, one mip, 341x341 most (683x683 or 1440x1440 some). This script decodes them with our own
ASTC decoder (astc_ldr.py; the packaging Python has no image library), shrinks each to SIZE x SIZE and writes it as a PNG
(straight alpha) into the App's extracted/oot/vr-icons/<name>.png. Each only when the player's game has it: a missing or
unreadable one is named and left out, and the wheel draws that slot's name instead. Generated game content stays
outside the checkout.

Which icon stands for which wheel slot is the runtime's (zeldaVR/runtime/oot_vr_wheel_icons.h); the names follow
HookshotVR's notes (its radial item table and the bottle-content icons, facts only), each checked present in the RomFS.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import os
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import astc_ldr  # noqa: E402
import make_vr_rig  # noqa: E402

SIZE = 128
ASTC_4X4 = {0x2d01: False, 0x2d06: True}   # format -> sRGB
ICONS = ('Weapon_Range_Bow', 'Pouch_Range_BowAndArrow_Fire', 'Pouch_Range_BowAndArrow_Ice', 'Pouch_Range_BowAndArrow_Light',
         'Item_Longshot', 'Item_Hookshot', 'Item_Bomb', 'Item_Bombchu', 'Item_Hammer', 'Item_DekuNut', 'Item_DekuStick',
         'Weapon_Range_SlingShot', 'Item_Boomerang', 'Armor_GlassTruth', 'Item_DinFire', 'Item_FaroreWind', 'Item_NayruLove',
         'Item_MagicBean', 'Item_Bottle', 'Item_Bottle_RedPotion', 'Item_Bottle_GreenPotion', 'Item_Bottle_BluePotion',
         'Item_Bottle_BlueFire', 'Item_Bottle_Fish', 'Item_Bottle_Letter', 'Item_Bottle_BigPoe', 'Item_Bottle_Poe',
         'Item_Bottle_Bug', 'Item_Bottle_Fairy', 'Item_Bottle_Milk', 'Item_Bottle_HalfMilk', 'Weapon_Sword_Kokiri',
         'Weapon_Sword_Master', 'Weapon_Sword_Biggoron', 'Weapon_Sword_Giant', 'Weapon_Sword_BrokenGiant', 'Weapon_Shield_Deku',
         'Weapon_Shield_Hylian', 'Weapon_Shield_Mirror', 'Armor_Adult_Normal', 'Armor_Adult_Goron', 'Armor_Adult_Zora')


def icon_path(name):
    return f'UI/Tex/Icon/{name}.bntx.zs'


def texture(bntx):
    """The first texture of a BNTX: (width, height, sRGB?, block height in GOBs, offset of its pixels, their size)."""
    if bntx[:4] != b'BNTX':
        raise ValueError('not a BNTX')
    at = bntx.find(b'BRTI')
    if at < 0 or at + 0x78 > len(bntx):
        raise ValueError('no texture in the BNTX')
    dim, tile, _, mips, _, fmt, _, width, height, depth, layers, layout = struct.unpack_from('<BHHHIIIIIIII', bntx, at + 0x11)
    size = struct.unpack_from('<I', bntx, at + 0x50)[0]
    if fmt not in ASTC_4X4 or tile != 0 or dim != 2 or mips < 1 or depth != 1 or layers != 1 or not 0 < width <= 4096 or not 0 < height <= 4096:
        raise ValueError(f'format 0x{fmt:x}, tile mode {tile}, {width}x{height}x{depth}, {layers} layers: not a 2D ASTC 4x4 texture')
    table = struct.unpack_from('<Q', bntx, at + 0x70)[0]
    if table + 8 > len(bntx):
        raise ValueError('the texture has no pixel pointer')
    offset = struct.unpack_from('<Q', bntx, table)[0]
    block_height = 1 << (layout & 7)
    if padded_size((width + 3) // 4, (height + 3) // 4, block_height) > size or offset + size > len(bntx):
        raise ValueError('the texture is smaller than its layout says')
    return width, height, ASTC_4X4[fmt], block_height, offset, size


def padded_size(blocks_x, blocks_y, block_height):
    rows = 8 * block_height
    return (blocks_x * 16 + 63) // 64 * 64 * ((blocks_y + rows - 1) // rows * rows)


def gob_offset(x, y, gobs_wide, block_height):
    """Byte x, row y of a block-linear surface whose rows are gobs_wide GOBs (64 bytes x 8 rows) wide, in the Switch 2's
    order of a GOB's 16-byte pieces (x5 y2 x4 y1 y0; the Tegra X1's is x5 y2 y1 x4 y0): the order our runtime's own
    deswizzle uses for this game (third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c, GAME_NS2). Checked on the icons: decoded
    in the Tegra X1 order every 4x4 block steps against its neighbours; in this order the block edges are as smooth as
    the inside of the blocks (the only one of the 120 orders of these five bits that is)."""
    return ((y // (8 * block_height)) * 512 * block_height * gobs_wide + (x // 64) * 512 * block_height
            + ((y % (8 * block_height)) // 8) * 512 + ((x % 64) // 32) * 256 + ((y % 8) // 4) * 128
            + ((x % 32) // 16) * 64 + ((y % 4) // 2) * 32 + (y % 2) * 16 + x % 16)


def linear_blocks(data, offset, blocks_x, blocks_y, block_height):
    """The 16-byte blocks of a block-linear surface, row by row."""
    gobs_wide = (blocks_x * 16 + 63) // 64
    return [bytes(data[offset + (o := gob_offset(bx * 16, by, gobs_wide, block_height)):offset + o + 16])
            for by in range(blocks_y) for bx in range(blocks_x)]


def decode(width, height, blocks, srgb):
    """RGBA8 (straight alpha, as stored) of an ASTC 4x4 image from its blocks in row order."""
    blocks_x = (width + 3) // 4
    out = bytearray(width * height * 4)
    seen = {}
    for k, block in enumerate(blocks):
        texels = seen.get(block)
        if texels is None:
            texels = seen[block] = bytes(c >> 8 for texel in astc_ldr.decode_block(block, srgb) for c in texel)
        bx, by = k % blocks_x * 4, k // blocks_x * 4
        for row in range(min(4, height - by)):
            n = min(4, width - bx)
            at = ((by + row) * width + bx) * 4
            out[at:at + 4 * n] = texels[16 * row:16 * row + 4 * n]
    return out


def taps(source, size):
    """For each of size outputs: (source index, share) of the source samples it covers."""
    scale, out = source / size, []
    for j in range(size):
        lo, hi = j * scale, (j + 1) * scale
        out.append([(i, min(hi, i + 1) - max(lo, i)) for i in range(int(lo), min(source, int(hi) + 1)) if min(hi, i + 1) > max(lo, i)])
    return out


def shrink(rgba, width, height, size):
    """The image averaged over areas into size x size, the colours weighted by their alpha (no dark fringes)."""
    across, down = taps(width, size), taps(height, size)
    rows = []
    for y in range(height):
        line, row = rgba[y * width * 4:(y + 1) * width * 4], []
        for t in across:
            a = r = g = b = w = 0.0
            for i, share in t:
                al = line[4 * i + 3] * share
                r += line[4 * i] * al; g += line[4 * i + 1] * al; b += line[4 * i + 2] * al; a += al; w += share
            row.append((r, g, b, a, w))
        rows.append(row)
    out = bytearray(size * size * 4)
    for x in range(size):
        for j, t in enumerate(down):
            r = g = b = a = w = 0.0
            for i, share in t:
                pr, pg, pb, pa, pw = rows[i][x]
                r += pr * share; g += pg * share; b += pb * share; a += pa * share; w += pw * share
            at = (j * size + x) * 4
            if a > 0:
                out[at:at + 4] = bytes((min(255, round(r / a)), min(255, round(g / a)), min(255, round(b / a)), min(255, round(a / w))))
    return out


def png(width, height, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def icon_png(bntx, size=SIZE):
    width, height, srgb, block_height, offset, _ = texture(bntx)
    blocks = linear_blocks(bntx, offset, (width + 3) // 4, (height + 3) // 4, block_height)
    return png(size, size, shrink(decode(width, height, blocks, srgb), width, height, size))


def unpack(game, path):
    """A RomFS file, with the game's dictionary or (the UI textures) none."""
    raw = game.read(path)
    if make_vr_rig.zstd.get_frame_info(raw).dictionary_id == 0:
        return make_vr_rig.zstd.decompress(raw)
    return game.unpack(path)


def _made(job):
    name, bntx = job
    try:
        return name, icon_png(bntx), None
    except (ValueError, IndexError, struct.error) as e:
        return name, None, str(e)


def generate(romfs, output, names=ICONS, workers=None):
    """(written, [what the player's game does not have or could not be made])"""
    output = Path(output)
    make_vr_rig.check_output(output)
    output.mkdir(parents=True, exist_ok=True)
    jobs, missing = [], []
    with make_vr_rig.RomFS(romfs) as game:
        for name in names:
            if icon_path(name) not in game.files:
                missing.append(name)
                continue
            try:
                jobs.append((name, unpack(game, icon_path(name))))
            except (ValueError, OSError, make_vr_rig.zstd.ZstdError) as e:
                missing.append(f'{name} ({e})')
    workers = workers or min(8, os.cpu_count() or 1)
    if workers > 1 and len(jobs) > 1:
        with ProcessPoolExecutor(max_workers=workers) as pool:   # about 10 s of decoding on one core, 2 s on eight
            made = list(pool.map(_made, jobs))
    else:
        made = [_made(job) for job in jobs]
    written = 0
    for name, data, error in made:
        if data is None:
            missing.append(f'{name} ({error})')
        else:
            written += make_vr_rig.write_if_changed(output / f'{name}.png', data)
    return written, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    written, missing = generate(args.romfs, args.output)
    print(f'VR item icons: {written} written, {args.output}' + (', without ' + ', '.join(missing) if missing else ''))


if __name__ == '__main__':
    main()

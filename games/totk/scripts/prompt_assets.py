"""Button-guide texture metadata shared by static mods and live prompt preparation."""
import hashlib
import struct

HUD_TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041,
                'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
                'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046,
                'Nt_KeyTexLSt_00': 0xE052, 'Nt_KeyTexLSt_01': 0xE05E}


def gob_offset(x, y, width_gobs, block_height):
    return ((y // (8 * block_height)) * 512 * block_height * width_gobs
            + (x // 64) * 512 * block_height + ((y % (8 * block_height)) // 8) * 512
            + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((x % 32) // 16) * 32 + (y % 2) * 16 + x % 16)


def hud_fingerprints(bntx):
    """(code point, width, height, SHA-256 of linear R8 pixels), never game pixels."""
    found, pos = {}, 0
    while (at := bntx.find(b'BRTI', pos)) >= 0:
        pos = at + 4
        name_at = struct.unpack_from('<Q', bntx, at + 0x60)[0]
        length = struct.unpack_from('<H', bntx, name_at)[0]
        name = bntx[name_at + 2:name_at + 2 + length].decode().split('^')[0]
        if name not in HUD_TEXTURES:
            continue
        mips = struct.unpack_from('<H', bntx, at + 0x16)[0]
        fmt = struct.unpack_from('<I', bntx, at + 0x1C)[0]
        width, height = struct.unpack_from('<II', bntx, at + 0x24)
        layout = struct.unpack_from('<I', bntx, at + 0x34)[0]
        pointer = struct.unpack_from('<Q', bntx, at + 0x70)[0]
        data = struct.unpack_from('<Q', bntx, pointer)[0]
        if fmt != 0x201 or width != height or width not in (72, 108) or mips != 1:
            raise ValueError(f'{name}: unsupported HUD texture layout')
        pixels = bytes(bntx[data + gob_offset(x, y, (width + 63) // 64, 1 << (layout & 7))]
                       for y in range(height) for x in range(width))
        found[name] = (HUD_TEXTURES[name], width, height, hashlib.sha256(pixels).hexdigest())
    if set(found) != set(HUD_TEXTURES):
        raise ValueError(f'missing button-guide textures: {sorted(set(HUD_TEXTURES) - set(found))}')
    return [found[name] for name in HUD_TEXTURES]

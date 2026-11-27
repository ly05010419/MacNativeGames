#!/usr/bin/env python3
"""Link's two models, the slingshot's, the bow's and the arrows', decompressed from the player's own RomFS, for the VR first-person body (zeldaVR/runtime/oot_bfres.h).

The runtime has no zstd, the packaging Python has it in its standard library, so the App carries the two models as plain
files next to its other game data. Generated game content stays outside the checkout.

Also, each only when the player's game has it (a missing one is named and left out, and the VR body does without it): what
Link wears (OPTIONAL: the tunics and gauntlets the first-person arms carry, the bracelet, the things on his belt, back, feet
and head that first person leaves out, and his swords and shields, so a worn thing of the same size is never taken for them;
the list of names follows HookshotVR's notes, facts only), and his stepping cycles for the legs (make_vr_gait.py).
"""
import argparse
import compression.zstd as zstd
import json
import os
from pathlib import Path
import struct
import sys

MODELS = {'Player_Child': 'Model/Player_Child.Player_Child.bfres.zs', 'Player_Adult': 'Model/Player_Adult.Player_Adult.bfres.zs',
          'Weapon_Range_SlingShot': 'Model/Weapon_Range_SlingShot.Weapon_Range_SlingShot.bfres.zs',
          'Weapon_Range_Bow': 'Model/Weapon_Range_Bow.Weapon_Range_Bow.bfres.zs',
          'Weapon_Range_BowAndArrow_Normal': 'Model/Weapon_Range_Bow.Weapon_Range_BowAndArrow_Normal.bfres.zs',
          'Weapon_Range_BowAndArrow_Fire': 'Model/Weapon_Range_Bow.Weapon_Range_BowAndArrow_Fire.bfres.zs',
          'Weapon_Range_BowAndArrow_Ice': 'Model/Weapon_Range_Bow.Weapon_Range_BowAndArrow_Ice.bfres.zs',
          'Weapon_Range_BowAndArrow_Light': 'Model/Weapon_Range_Bow.Weapon_Range_BowAndArrow_Light.bfres.zs',
          # the arrows: the one nocked on the VR bow is the game's own (zeldaVR/gfx oot_vr_arrow.inc), known by its vertex counts
          'Item_Arrow_Normal': 'Model/Item_Arrow_Normal.Item_Arrow_Normal.bfres.zs',
          'Item_Arrow_Fire': 'Model/Item_Arrow_Fire.Item_Arrow_Fire.bfres.zs',
          'Item_Arrow_Ice': 'Model/Item_Arrow_Ice.Item_Arrow_Ice.bfres.zs',
          'Item_Arrow_Light': 'Model/Item_Arrow_Light.Item_Arrow_Light.bfres.zs'}
DICTIONARIES = 'Pack/ZsDic.pack.zs'
WORN = ('Item_BombBag_Small', 'Item_BombBag_Medium', 'Item_BombBag_Large', 'Item_BulletBag_Small', 'Item_BulletBag_Medium',
        'Item_BulletBag_Large', 'Item_NutBag_Small', 'Item_NutBag_Medium', 'Item_NutBag_Large', 'Item_StickBag_Small',
        'Item_StickBag_Medium', 'Item_StickBag_Large', 'Item_Quiver_Small', 'Item_Quiver_Medium', 'Item_Quiver_Large',
        'Item_Wallet_Small', 'Item_Wallet_Medium', 'Item_Wallet_Large', 'Item_Scale_Silver', 'Item_Scale_Gold',
        'Item_Bottle.Item_Bottle_Pouch', 'Item_Bottle_Fairy.Item_Bottle_Fairy_Pouch', 'Item_Bottle_Poe.Item_Bottle_Poe_Pouch',
        'Item_AgonyStone', 'Armor_Adult_HeavyBoot', 'Armor_Adult_HoverBoot', 'Armor_Adult_GlassTruth', 'Armor_Child_GlassTruth',
        'Armor_Adult_Default_Head', 'Armor_Child_Default_Head', 'Armor_Child_Mask_Bunny', 'Armor_Child_Mask_Gerudo',
        'Armor_Child_Mask_Goron', 'Armor_Child_Mask_Keaton', 'Armor_Child_Mask_Skull', 'Armor_Child_Mask_Spooky',
        'Armor_Child_Mask_Truth', 'Armor_Child_Mask_Zora', 'Armor_Child_Mask_Amiibo0', 'Armor_Child_Mask_Amiibo1')
CARRIED = ('Armor_Adult_Goron', 'Armor_Adult_Zora', 'Armor_Adult_Gauntlet_Silver', 'Armor_Adult_Gauntlet_Gold', 'Item_Gauntlet_Goron')
OWN = ('Weapon_Sword_Kokiri', 'Weapon_Sword_Master', 'Weapon_Sword_Biggoron', 'Weapon_Shield_Deku', 'Weapon_Shield_Hylian',
       'Weapon_Shield_Mirror', 'Weapon_Sheath_Kokiri', 'Weapon_Sheath_Master', 'Weapon_Sheath_Biggoron', 'Armor_Adult_Normal',
       'Item_OcarinaHolder_Fairy', 'Item_OcarinaHolder_Time', 'Item_Ocarina_Fairy', 'Item_Ocarina_Time')


def model_path(name):
    """'A' -> Model/A.A.bfres.zs, 'A.B' -> Model/A.B.bfres.zs; written as B.bfres"""
    file, model = name.split('.') if '.' in name else (name, name)
    return f'Model/{file}.{model}.bfres.zs', model


OPTIONAL = dict(model_path(n)[::-1] for n in WORN + CARRIED + OWN)


def sarc_members(data):
    """name -> (start, end) of every member of a SARC archive."""
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise ValueError('the dictionary pack is not a little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    if size != len(data) or data[header:header + 4] != b'SFAT':
        raise ValueError('the dictionary pack is damaged')
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    if data[names:names + 4] != b'SFNT':
        raise ValueError('the dictionary pack has no name table')
    names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at, start)].decode()] = (start + lo, start + hi)
    return out


def write_if_changed(path, data):
    if path.is_file() and path.stat().st_size == len(data) and path.read_bytes() == data:
        return False
    temporary = path.with_name('.' + path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


class RomFS:
    """The player's RomFS image (index.json + romfs.bin): its files, decompressed with the game's own dictionaries"""

    def __init__(self, romfs):
        romfs = Path(romfs)
        self.files = {f['path']: f for f in json.loads((romfs / 'index.json').read_text())['files']}
        self.image = (romfs / 'romfs.bin').open('rb')
        try:
            pack = zstd.decompress(self.read(DICTIONARIES))
        except BaseException:
            self.image.close()
            raise
        self.dictionaries = {}
        for name, (lo, hi) in sarc_members(pack).items():
            if name.endswith('.zsdic'):
                dictionary = zstd.ZstdDict(pack[lo:hi])
                self.dictionaries[dictionary.dict_id] = dictionary

    def __enter__(self):
        return self

    def __exit__(self, *error):
        self.image.close()

    def read(self, name):
        entry = self.files[name]
        self.image.seek(entry['offset'])
        raw = self.image.read(entry['size'])
        if len(raw) != entry['size']:
            raise ValueError('truncated RomFS entry: ' + name)
        return raw

    def unpack(self, path):
        raw = self.read(path)
        wanted = zstd.get_frame_info(raw).dictionary_id
        if wanted not in self.dictionaries:
            raise ValueError(f'{path}: its compression dictionary {wanted} is not in the pack')
        return zstd.decompress(raw, zstd_dict=self.dictionaries[wanted])

    def model(self, path):
        model = self.unpack(path)
        if model[:4] != b'FRES':
            raise ValueError(path + ' is not a FRES model')
        return model


def check_output(output):
    repo = Path(__file__).resolve().parents[3]
    if (repo / '.git').exists() and (output.resolve() == repo or repo in output.resolve().parents):
        raise ValueError('generated VR models must remain outside the source checkout')


def generate(romfs, output):
    romfs, output = Path(romfs), Path(output)
    check_output(output)
    output.mkdir(parents=True, exist_ok=True)
    written = 0
    with RomFS(romfs) as game:
        for name, path in MODELS.items():
            written += write_if_changed(output / f'{name}.bfres', game.model(path))
    return written


def generate_optional(romfs, output):
    """What Link wears and his stepping cycles: (written, [what the player's game does not have or could not be made])"""
    output = Path(output)
    check_output(output)
    output.mkdir(parents=True, exist_ok=True)
    with RomFS(romfs) as game:
        return optional_from(game, output)


def optional_from(game, output):
    written, missing = 0, []
    for name, path in OPTIONAL.items():
        if path not in game.files:
            missing.append(name)
            continue
        written += write_if_changed(output / f'{name}.bfres', game.model(path))
    try:
        import make_vr_gait as gait
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import make_vr_gait as gait
    for age, (model, moving, standing) in gait.FILES.items():
        try:
            data = gait.make((output / f'{model}.bfres').read_bytes(), game.unpack(moving), game.unpack(standing))
            written += write_if_changed(output / f'gait-{age}.bin', data)
        except (KeyError, OSError, ValueError, struct.error) as e:
            missing.append(f'gait-{age} ({e})')
    return written, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    print(f'VR models: {generate(args.romfs, args.output)} written, {args.output}')
    try:   # (the body above works without these: a failure here never takes it away)
        written, missing = generate_optional(args.romfs, args.output)
        print(f'VR worn things and steps: {written} written' + (', without ' + ', '.join(missing) if missing else ''))
    except Exception as e:   # noqa: BLE001 (any reason: logged, and the App goes on without them)
        print(f'VR worn things and steps could not be made: {e}')


if __name__ == '__main__':
    main()

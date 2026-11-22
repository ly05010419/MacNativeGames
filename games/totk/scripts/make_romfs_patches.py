#!/usr/bin/env python3
"""Build the RomFS overlay files the native runtime serves in place of the originals (runtime/romfs.c).

UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs, member anim/ShortCutBG_00_In.bflan: the weapon / shield /
material quick menus capture the game picture for their background when the capture panes C_CaptureBG_00,
C_CaptureClearBG_00 and P_CaptureClearBG_00 become visible at animation frame 1.0. UI animations advance 0.5 frames
per game frame at 60 fps (present interval 1), so the capture lands after the world has stopped drawing and the
background can be black. Moving those three visibility keys to frame 0.0 captures the live world immediately;
weapon, shield, material and ability-wheel backgrounds were checked at 60 fps, with a 30 fps regression capture.
The same archive's anim/AutoBuilder_00_In.bflan exposes C_CaptureBG_00 at frame 1.0; move that single
visibility key to frame 0.0 as well. The available test save does not yet unlock AutoBuilder, so that screen
still needs an in-game screenshot check.

On 1.4.3, the same archive's blyt/PaSystemController_00.bflyt captures the faint controller diagram behind an
interaction prompt before the live button-prompt atlases can be reflected in it. Remove the guarded
DynamicCaptureOn = "RGBA8" entry from N_CapGuide_00's active usd1 table without resizing the layout. Its child glyph
panes then keep drawing from the live atlases and follow prompt style and controller remaps. Earlier versions do not
use the macOS live-prompt path and keep their original layout.

Event/EventFlow/DmF_SY_GameOver.bfevfl.zs: after 重试 / 结束游戏 the game over flow requests GameOverPause (Event14)
and then waits one frame (Event25, EventWait Frames 1) before the event ends and the game reloads or goes to the
title. Shipping frame steps are one 30 fps frame or more, so that wait ends in the frame it starts; with the native
real-time step (about 0.35 of a frame at 85 fps, 0.5 with the 60 FPS patch alone) it needs several frames, the pause
freezes it after the first, and the menu never goes away (2026-10-17: 100 % of deaths at the default settings, never
with TOTK_FPS60=0 TOTK_REALTIME=0). Event14 now ends the branch itself: the 2-byte "next event" index of Event14 goes
from Event25 to none. Checked with fall deaths at the default settings: the retry reloads the save.
Only 1.0.0 needs it: 1.2.1 and 1.4.3 run DmF_SY_GameOver.110.bfevfl.zs instead (their EventFlowFileEntry names
version 110), whose wait (EventWaitUpdate) the pause does not freeze. Their RomFS still holds the 1.0.0 file, so its overlay
is made and verified there too; 1.2.1 even opens that file when Link dies (the overlay line shows in the log), but runs the
110 flow: with overlays off (TOTK_ROMFS_PATCHES=0) its retry after a fall death reloads normally (2026-10-19).

Input: the IVFC-verified local RomFS image (extracted/v0/romfs). Output: extracted/v0/romfs-patches/<path> and
manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original). Game content stays in the ignored folder.
"""
import argparse, hashlib, json, struct, sys
from pathlib import Path
import compression.zstd as zstd

root = Path(__file__).resolve().parents[1]

PANE_MAGICS = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
PROMPT_DIAGRAM = 'blyt/PaSystemController_00.bflyt'

def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'; names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out

def flvi_keys(data, base):
    """(pane name, absolute key offset, keys) of every FLVI (visibility) step track in the BFLAN at data[base:]."""
    assert data[base:base + 4] == b'FLAN' and data[base + 4:base + 6] == b'\xff\xfe'
    pos, out = base + 0x14, []
    for _ in range(struct.unpack_from('<H', data, base + 0x10)[0]):
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'pai1':
            count, table = struct.unpack_from('<H', data, pos + 14)[0], pos + struct.unpack_from('<I', data, pos + 16)[0]
            for e in range(count):
                eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
                name = data[eo:eo + 28].split(b'\0')[0].decode()
                for t in range(data[eo + 28]):
                    to = eo + struct.unpack_from('<I', data, eo + 32 + 4 * t)[0]
                    if data[to:to + 4] != b'FLVI': continue
                    for g in range(data[to + 4]):
                        go = to + struct.unpack_from('<I', data, to + 8 + 4 * g)[0]
                        assert data[go + 2] == 1, 'visibility tracks are step curves'
                        ko = go + struct.unpack_from('<I', data, go + 8)[0]
                        keys = [struct.unpack_from('<fH', data, ko + 8 * k) for k in range(struct.unpack_from('<H', data, go + 4)[0])]
                        out.append((name, ko, keys))
        pos += size
    return out

COMMON = {'v0': '100', 'v121': '110', 'v143': '140'}   # the version in each game's Common archive name


def common_archive(version):
    return 'UI/LayoutArchive/Common.Product.' + COMMON[version] + '.Nin_NX_NVN.blarc.zs'


MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}


def common_patch_members(version):
    """Common archive members changed by the build for a game version."""
    if version not in COMMON:
        raise ValueError(f'unknown game version {version}')
    return [*MEMBERS, *([PROMPT_DIAGRAM] if version == 'v143' else [])]


def patch_capture(blarc, members, frame):
    """Move the capture panes' visibility keys in MEMBERS of the decompressed Common archive `blarc` (a bytearray, with
    `members` from sarc_members) from frame 1.0 to `frame`. Each pane must have the shipping keys (0.0, 0) (1.0, 1); a
    player's mod archive goes through the same checks (scripts/make_mod_manifest.py). Returns member -> panes moved."""
    patched = {}
    for member, panes in MEMBERS.items():
        lo, hi = members[member]
        changed = []
        for name, ko, keys in flvi_keys(blarc, lo):
            if name not in panes: continue
            if [(f, v) for f, v in keys] != [(0.0, 0), (1.0, 1)]:
                raise ValueError(f'{member}: pane {name} has the keys {keys}, not the shipping (0.0, 0) (1.0, 1)')
            struct.pack_into('<f', blarc, ko + 8, frame)
            changed.append(name)
        if sorted(changed) != sorted(panes):
            raise ValueError(f'{member}: found the panes {changed}, expected {list(panes)}')
        patched[member] = changed
    return patched


def patch_prompt_diagram(blarc, members):
    """Disable the stale one-time capture behind interaction prompts.

    PaSystemController_00's N_CapGuide_00 pane has an adjacent usd1 user-data
    section with a string entry ``DynamicCaptureOn = RGBA8``. Remove that entry
    from the active table without resizing the layout, so its child button
    glyphs keep using the live prompt atlases. All pointers in a usd1 entry are
    relative to that entry's own start.
    """
    if PROMPT_DIAGRAM not in members:
        raise ValueError(f'{PROMPT_DIAGRAM}: member not found')
    lo, hi = members[PROMPT_DIAGRAM]
    if not (0 <= lo <= hi <= len(blarc)):
        raise ValueError(f'{PROMPT_DIAGRAM}: member bounds {lo}:{hi} are outside the archive')
    if hi - lo < 0x14:
        raise ValueError(f'{PROMPT_DIAGRAM}: truncated BFLYT header')
    if bytes(blarc[lo:lo + 6]) != b'FLYT\xff\xfe':
        raise ValueError(f'{PROMPT_DIAGRAM}: not a little-endian BFLYT')
    header, declared = struct.unpack_from('<HxxxxI', blarc, lo + 6)
    if declared != hi - lo:
        raise ValueError(f'{PROMPT_DIAGRAM}: declared size {declared} differs from member size {hi - lo}')
    if not 0x14 <= header <= declared:
        raise ValueError(f'{PROMPT_DIAGRAM}: header size {header} is outside the declared file')
    end = lo + declared
    section_count = struct.unpack_from('<H', blarc, lo + 0x10)[0]
    pos, targets = lo + header, []
    for _ in range(section_count):
        if pos + 8 > end:
            raise ValueError(f'{PROMPT_DIAGRAM}: truncated section table')
        magic = bytes(blarc[pos:pos + 4])
        size = struct.unpack_from('<I', blarc, pos + 4)[0]
        if size < 8 or pos + size > end:
            raise ValueError(f'{PROMPT_DIAGRAM}: invalid {magic!r} section size {size}')
        if magic in PANE_MAGICS:
            if size < 36:
                raise ValueError(f'{PROMPT_DIAGRAM}: pane section {magic!r} is too small for its name')
            try:
                name = bytes(blarc[pos + 12:pos + 36]).split(b'\0')[0].decode()
            except UnicodeDecodeError as error:
                raise ValueError(f'{PROMPT_DIAGRAM}: invalid pane name encoding') from error
            if name == 'N_CapGuide_00':
                targets.append(pos + size)
        pos += size
    if len(targets) != 1:
        raise ValueError(f'{PROMPT_DIAGRAM}: N_CapGuide_00 pane not found exactly once (found {len(targets)})')
    usd = targets[0]
    if usd + 12 > end or bytes(blarc[usd:usd + 4]) != b'usd1':
        raise ValueError(f'{PROMPT_DIAGRAM}: N_CapGuide_00 is not followed by usd1')
    size = struct.unpack_from('<I', blarc, usd + 4)[0]
    usd_end = usd + size
    if size < 12 or usd_end > end:
        raise ValueError(f'{PROMPT_DIAGRAM}: invalid usd1 section size {size}')
    count = struct.unpack_from('<H', blarc, usd + 8)[0]
    table = usd + 12
    if table + count * 12 > usd_end:
        raise ValueError(f'{PROMPT_DIAGRAM}: usd1 entry table exceeds the section')

    entries = []
    for i in range(count):
        entry = table + i * 12
        name_offset, data_offset, length, kind, padding = struct.unpack_from('<IIHBB', blarc, entry)
        name_at, data_at = entry + name_offset, entry + data_offset
        if not (table + count * 12 <= name_at < usd_end) or 0 not in blarc[name_at:usd_end]:
            raise ValueError(f'{PROMPT_DIAGRAM}: usd1 entry {i} has an invalid name pointer')
        name_end = blarc.index(0, name_at, usd_end)
        try:
            name = bytes(blarc[name_at:name_end]).decode()
        except UnicodeDecodeError as error:
            raise ValueError(f'{PROMPT_DIAGRAM}: usd1 entry {i} has an invalid name') from error
        if not (table + count * 12 <= data_at <= usd_end) or data_at + length > usd_end:
            raise ValueError(f'{PROMPT_DIAGRAM}: usd1 entry {name} has an invalid data pointer')
        entries.append((entry, name, data_at, length, kind, padding))

    matches = [(i, entry) for i, entry in enumerate(entries) if entry[1] == 'DynamicCaptureOn']
    if len(matches) != 1:
        raise ValueError(f'{PROMPT_DIAGRAM}: DynamicCaptureOn found {len(matches)} times, expected 1')
    remove, (_, _, data_at, length, kind, _) = matches[0]
    if kind != 0:
        raise ValueError(f'{PROMPT_DIAGRAM}: DynamicCaptureOn has type {kind}, expected string (0)')
    value = bytes(blarc[data_at:data_at + length]).decode(errors='replace')
    if value != 'RGBA8':
        raise ValueError(f"{PROMPT_DIAGRAM}: DynamicCaptureOn has value {value!r}, expected 'RGBA8'")
    if data_at + length >= usd_end or blarc[data_at + length] != 0:
        raise ValueError(f'{PROMPT_DIAGRAM}: DynamicCaptureOn string is not NUL-terminated')

    # Compact only the active entry table. Payloads and the unused final entry
    # stay in place, keeping every section and every later offset unchanged.
    for i in range(remove + 1, count):
        old = table + i * 12
        new = old - 12
        name_offset, data_offset, length, kind, padding = struct.unpack_from('<IIHBB', blarc, old)
        struct.pack_into('<IIHBB', blarc, new, name_offset + 12, data_offset + 12, length, kind, padding)
    struct.pack_into('<H', blarc, usd + 8, count - 1)
    return [PROMPT_DIAGRAM]


def patch_common(blarc, members, frame, version):
    """Apply every guarded Common archive edit for `version`."""
    patched = patch_capture(blarc, members, frame)
    if version == 'v143':
        for member in patch_prompt_diagram(blarc, members):
            patched[member] = ['DynamicCaptureOn: removed']
    return patched


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=root / 'extracted/v0/romfs-patches')
    parser.add_argument('--capture-frame', type=float, default=0.0)
    parser.add_argument('--version', choices=('v0','v121','v143'), default='v0')
    parser.add_argument('--aspect', type=str.lower, help='<width>x<height> of a canvas wider (scripts/ultrawide_layout.py) or taller (scripts/tall_layout.py, 16:10) than 16:9: also lay the UI out for it; default output romfs-patches-<width>x<height> (-center with --hud center)')
    parser.add_argument('--hud', choices=('corner', 'center'), default='corner', help='with --aspect: the HUD in the screen corners or in the middle 16:9')
    args = parser.parse_args()
    version = args.version
    if args.output_dir == root / 'extracted/v0/romfs-patches': args.output_dir = root / (f'extracted/{version}/romfs-patches' + (f'-{args.aspect}' + ('-center' if args.hud == 'center' else '') if args.aspect else ''))
    assert 0 <= args.capture_frame <= 0.5
    manifest = json.loads((root / f'extracted/{version}/romfs/index.json').read_text())
    assert manifest['status'] == 'pass'
    files = {f['path']: f for f in manifest['files']}
    out_dir = args.output_dir
    ARCHIVE = common_archive(version)

    with (root / f'extracted/{version}/romfs/romfs.bin').open('rb') as image:
        def read(path): f = files[path]; image.seek(f['offset']); b = image.read(f['size']); assert len(b) == f['size']; return b
        dictionaries = {}
        for name, (lo, hi) in sarc_members(pack := zstd.decompress(read('Pack/ZsDic.pack.zs'))).items():
            d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        raw = read(ARCHIVE)
    info = zstd.get_frame_info(raw); dictionary = dictionaries[info.dictionary_id]
    blarc = bytearray(zstd.decompress(raw, zstd_dict=dictionary)); assert len(blarc) == info.decompressed_size
    members = sarc_members(blarc)
    patched = patch_common(blarc, members, args.capture_frame, version)
    layout = []
    if args.aspect:
        import importlib.util
        width, height = (int(v) for v in args.aspect.lower().split('x'))
        script = 'ultrawide_layout.py' if width * 9 > height * 16 else 'tall_layout.py'   # wider than 16:9, or taller
        spec = importlib.util.spec_from_file_location(script[:-3], Path(__file__).resolve().parent / script)
        ultrawide = importlib.util.module_from_spec(spec); spec.loader.exec_module(ultrawide)
        layout = ultrawide.apply(blarc, members, width, height, hud_corner=args.hud == 'corner')
    check = zstd.decompress(zstd.compress(bytes(blarc), level=19, zstd_dict=dictionary), zstd_dict=dictionary)
    assert check == bytes(blarc)
    original = zstd.decompress(raw, zstd_dict=dictionary)
    diff = [i for i in range(len(original)) if original[i] != blarc[i]]
    assert diff and all(any(lo <= i < hi for lo, hi in (members[m] for m in [*common_patch_members(version), *layout])) for i in diff), diff[:20]
    data = zstd.compress(bytes(blarc), level=19, zstd_dict=dictionary)
    assert zstd.get_frame_info(data).dictionary_id == info.dictionary_id and zstd.get_frame_info(data).decompressed_size == len(blarc)
    target = out_dir / ARCHIVE; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    lines = ['\t'.join([ARCHIVE, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()])]
    print(json.dumps(dict(status='pass', path=ARCHIVE, members=patched, frame=f'1.0 -> {args.capture_frame}', ultrawide_layout=dict(aspect=args.aspect, hud=args.hud, members=len(layout)) if args.aspect else None, size=len(data), changed_bytes=len(diff))))

    # The game over flow: Event14 (EventTriggerPauseMgrRequestPause GameOverPause) ends its branch instead of going on to
    # Event25 (EventWait, Frames 1). BFEVFL events are 0x28 bytes from 0x1f0 (name ref at +0, type at +8, next at +0xa);
    # the original's SHA-256 pins the layout these offsets come from.
    # v121 and v143 additionally have DmF_SY_GameOver.110 with EventWaitUpdate instead of EventWait.
    # Preserve that updated flow; this guarded patch only covers the retained original file.
    FLOW = 'Event/EventFlow/DmF_SY_GameOver.bfevfl.zs'
    FLOW_SHA256 = 'c59975494fb30063b7a339fd66bbde8b456b11c73a5b4332bc070d63ece66b7d'
    with (root / f'extracted/{version}/romfs/romfs.bin').open('rb') as image:
        f = files[FLOW]; image.seek(f['offset']); raw = image.read(f['size']); assert len(raw) == f['size']
    info = zstd.get_frame_info(raw); dictionary = dictionaries[info.dictionary_id]
    flow = bytearray(zstd.decompress(raw, zstd_dict=dictionary))
    assert hashlib.sha256(flow).hexdigest() == FLOW_SHA256, 'DmF_SY_GameOver.bfevfl is not the analysed v1.0.0 file'
    def event(i):
        e = 0x1f0 + 0x28 * i; ref = struct.unpack_from('<Q', flow, e)[0]; n = struct.unpack_from('<H', flow, ref)[0]
        return e, flow[ref + 2:ref + 2 + n].decode(), flow[e + 8], struct.unpack_from('<H', flow, e + 0xa)[0]
    e14, name14, type14, next14 = event(7); _, name25, type25, next25 = event(25)
    assert (name14, type14, next14) == ('Event14', 0, 25) and (name25, type25, next25) == ('Event25', 0, 0xffff)
    struct.pack_into('<H', flow, e14 + 0xa, 0xffff)
    data = zstd.compress(bytes(flow), level=19, zstd_dict=dictionary)
    assert zstd.decompress(data, zstd_dict=dictionary) == bytes(flow) and zstd.get_frame_info(data).dictionary_id == info.dictionary_id
    target = out_dir / FLOW; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    lines.append('\t'.join([FLOW, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()]))
    print(json.dumps(dict(status='pass', path=FLOW, event='Event14 next: Event25 (wait 1 frame) -> end', size=len(data), changed_bytes=2)))
    (out_dir / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_romfs_patches.py)\n' + '\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()

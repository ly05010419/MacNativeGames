"""The game's UI layouts for a canvas taller than 16:9 (TOTK_OUTPUT_RES=screen on a 16:10 display), edited in place in the
decompressed UI/LayoutArchive/Common.Product.*.blarc (scripts/make_romfs_patches.py --aspect). The vertical counterpart of
scripts/ultrawide_layout.py.

The game lays its UI out on a 1920x1080 grid and maps that grid onto the canvas, so on a 16:10 canvas every layout comes out 11%
too tall. Here each layout's root pane is squeezed to 16:9 (scale y = (w / h) / (16 / 9) = k); the layouts that sit against the
top or the bottom of the screen move out with it (the root pane's y); about sixty panes are set one by one: backdrops and
blurs are stretched back to the screen's edges (scale y 1 / k), a few widgets keep their shape (scale y k) or sit where the
squeeze left a gap (y), and three animations that move panes (the minimap's slide-in, the discovery banner, the dragon tears
zoom) get the same distances.

The pane list and the distances come from the community TOTK-Ultrawide mod's 16:10 archive for 1.0.0 .. 1.2.1 (GPL-3.0; none of
its code or files are used here): the list is what differs between that archive's layouts and the game's own, read pane by pane
and implemented independently, as scripts/ultrawide_layout.py does for 21:9. Every pane is found by its exact name and every
pane field and animation key holds the value the game ships before it is written, so a version with other values stops the
script instead of getting wrong positions.

The runtime's draw-time UI squeeze (TOTK_UI_SQUEEZE, nvn_mtl.m) must be off with these layouts, as with the ultrawide ones, or the UI
is squeezed twice.
"""
import struct

PANE_MAGICS = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
FIELD = {'x': 44, 'y': 48, 'sx': 68, 'sy': 72, 'h': 80}   # offsets in a pane section: name at +12 (24 bytes), translate x / y, scale x / y, height
# Layouts whose root stays as tall as the canvas: full-screen backgrounds and fades.
KEEP_ROOT = {'AppMenuBG_00', 'AppMenuOverlay_00', 'CameraShutter_00', 'EventFade_00', 'EventFadeWipe_00', 'PaAppPictureBookDetail_00', 'PauseMenuBG_00',
             'ShopMenuParasailBG_00', 'ShortCutBG_00', 'TitleMenuBG_00'}
# Layouts that hang from the top (UP) or the bottom (DOWN) of the screen: the whole layout moves out by what the squeeze left free.
ROOT_UP = ['AppAlbum_00', 'AppPictureBook_00', 'ChallengeLog_00', 'Challenge_00', 'InstantTipsWithPict_00', 'InstantTips_00', 'MiniGameTimer_00', 'NumDisplaySecond_00', 'NumDisplay_00', 'PickUpWin_00', 'PlayerStatus_00', 'Pouch_00', 'ShopMenu_00', 'SystemActionGuide_00', 'SystemLoadList_00', 'SystemOption_00', 'SystemSaveLoad_00']
ROOT_DOWN = ['CtrlGuide_00', 'DoCommand_00', 'EnvironmentInfo_00', 'HorseGauge_00', 'MessageDialog_00', 'MessageLog_01', 'PauseMenuOverlay_00', 'SaveLoadIcon_00', 'TreasureFullShortCut_00']

# Where this differs from the community list (checked in the game on 1.4.3):
# - The pages of the pause menu with a header and a guide row (the controls page, load, save and options) stay centred: the
#   community moves the whole layout up (its y) and the background down, which leaves the content a row too high.
CENTERED = ('SystemActionGuide_00', 'SystemLoadList_00', 'SystemOption_00', 'SystemSaveLoad_00')
# - The item lists end at the guide row instead of a gap above it: the list's window and its scissor (the rows they show) grow
#   down by what the squeeze and the root shift leave free, and move down half of it so the top stays. (layout, pane, shipped
#   height, shipped y)
LIST_EXTENSION = [('PaPouchSlideList_01', 'W_BaseFB_00', 984.0, -71.0), ('PaPouchSlideList_01', 'W_BaseLine_00', 1067.0, -29.0),
                  ('PaPouchSlideListBtnSet_00', 'N_Scissor_00', 894.0, -24.0), ('PaPouchSlideListBtnSetLight_00', 'N_Scissor_00', 894.0, -24.0)]

def edits(k):
    """(layout, pane, field, value the game ships, new scale for sx / sy or distance to move for x / y). k = (w / h) / (16 / 9); wide = how far a full-height pane stretches,
    out = how far a pane flush with a 1080-high layout's top or bottom must move in its own units to reach the canvas's edge, half =
    the same for the root pane (which is scaled by k itself)."""
    wide, out, half = 1 / k, 540 * (1 / k - 1), 540 * (1 - k)
    return [
    ('AppAlbum_00', 'N_Open_01', 'sy', 1.0, wide),
        ('AppAlbum_00', 'N_PicLoad_01', 'y', 0.0, half * -1),
        ('AppAlbum_00', 'N_FadeInOut_00', 'sy', 1.0, k),
        ('AppMap_00', 'N_BlurInOut_00', 'sy', 1.0, wide),
        ('AppMap_00', 'N_BlurInOut_01', 'sy', 1.0, wide),
        ('AppMap_00', 'N_BlurInOut_02', 'sy', 1.0, wide),
        ('AppMap_00', 'N_BlurInOut_03', 'sy', 1.0, wide),
        ('AppMap_00', 'N_AllMapTime_00', 'y', 0.0, out * -1),
        ('AppMenuOverlay_00', 'N_FadeInOut_00', 'sy', 1.0, k),
        ('AppMenuOverlay_00', 'N_TitleTab_00', 'y', 491.0, out),
        ('AppMenuOverlay_00', 'N_TitleOne_01', 'sy', 1.0, k),
        ('AppMenuOverlay_00', 'N_FadeInOut_01', 'sy', 1.0, k),
        ('AppMenuOverlay_00', 'A_Align_00', 'y', -505.0, out * -1),
        ('AppMenuOverlay_00', 'Pa_GuideLeft_00', 'y', -505.0, out * -1),
        ('AppMenuOverlay_00', 'N_Icon_00', 'sy', 1.0, k),
        ('AppOpenDemoWindow_00', 'P_BG_00', 'sy', 1.0, wide),
        ('AutoBuilder_00', 'N_BG_00', 'sy', 1.0, wide),
        ('CameraPointer_00', 'N_InOutScope_00', 'sy', 1.0, wide),
        ('CameraPointer_00', 'S_Scissor_00', 'sy', 1.0, wide),
        ('CameraPointer_00', 'N_Type_01', 'sy', 1.0, k),
        ('CameraPointer_00', 'W_Shadow_01', 'sy', 1.0, wide),
        ('CameraPointer_00', 'Pa_CameraFocus_00', 'sy', 1.0, wide),
        ('CameraSystemWindow_00', 'N_InOut_00', 'sy', 1.0, wide),
        ('CameraSystemWindow_00', 'N_FirstOut_00', 'sy', 1.0, k),
        ('GameOver_00', 'P_BG_00', 'sy', 1.0, wide),
        ('LoadingFade_00', 'Pa_Map_00', 'sy', 1.0, k),
        ('LoadingFade_00', 'Pa_LargeDungeonMap_00', 'sy', 1.0, k),
        ('LoadingFade_00', 'Pa_Map_01', 'sy', 1.0, k),
        ('LoadingFade_00', 'Pa_LargeDungeonMap_01', 'sy', 1.0, k),
        ('LoadingFade_00', 'N_Base_00', 'sy', 1.0, wide),
        ('LoadingFade_00', 'P_BG_00', 'sy', 1.0, wide),
        ('LoadingFade_00', 'Pa_IconPlayer_00', 'sy', 1.0, k),
        ('LoadingFade_00', 'N_Logo_00', 'y', 0.0, out * -1),
        ('LoadingFade_00', 'N_Tips_00', 'y', -360.0, out * -1),
        ('LoadingFade_00', 'N_Status_00', 'y', 0.0, out),
        ('LoadingFade_00', 'N_Sage_00', 'y', -477.0, out * -2),
        ('Location_00', 'N_Large_00', 'y', 270.0, out),
        ('Location_00', 'N_Small_00', 'y', -395.0, out * -1),
        ('MessageLog_01', 'P_Capture_00', 'y', 0.0, out),
        ('MessageLog_01', 'P_Capture_00', 'sy', 1.0, wide),
        ('OwnedHorseList_00', 'N_BG_00', 'sy', 1.0, wide),
        ('PaExtraStaminaGauge_00', 'N_BasePattern_00', 'y', 0.0, out),
        ('PaExtraStaminaGauge_00', 'N_GaugeMainPattern_00', 'y', 0.0, out),
        ('PaLargeDungeonMap_00', 'P_BG_00', 'sy', 1.0, wide),
        ('PaRecipeCardList_00', 'P_BG_00', 'sy', 1.0, wide),
        ('Pouch_00', 'N_Info_00', 'y', 0.0, out * -1),
        ('Pouch_00', 'Pa_ExtraStamina_00', 'y', 0.0, out * -1),
        ('ShopMenu_00', 'N_BG_00', 'sy', 1.0, wide),
        ('ShopMenu_00', 'A_AlignGuide_00', 'y', -477.0, out * -1),
        ('StaffRoll_00', 'N_BG_00', 'sy', 1.0, wide),
        ('SystemActionGuide_00', 'N_SystemBG_00', 'y', 0.0, out * -1),
        ('SystemActionGuide_00', 'N_SystemBG_00', 'sy', 1.0, wide),
        ('SystemLoadList_00', 'N_SystemBG_00', 'y', 0.0, out * -1),
        ('SystemLoadList_00', 'N_SystemBG_00', 'sy', 1.0, wide),
        ('SystemLoadList_00', 'Pa_Guide_00', 'y', -474.0, out * -1),
        ('SystemOption_00', 'N_SystemBG_00', 'y', 0.0, out * -1),
        ('SystemOption_00', 'N_SystemBG_00', 'sy', 1.0, wide),
        ('SystemOption_00', 'A_Guide_00', 'y', -474.0, out * -1),
        ('SystemSaveLoad_00', 'N_SystemBG_00', 'y', 0.0, out * -1),
        ('SystemSaveLoad_00', 'N_SystemBG_00', 'sy', 1.0, wide),
    ]

def anim_edits(k):
    """(animation, shipped value, new value, how many keys hold it or their offsets in the member): the floats inside BFLAN key
    tables the layout edits leave wrong (the zoom has the same two values on its x track, which stays)."""
    wide, out = 1 / k, 540 * (1 / k - 1)
    return [('AppMap_00_MiniMap', -360.0, -360.0 - out, 8), ('Location_00_IsDiscovery', 285.0, 285.0 + out, 2),
            ('Location_00_IsDiscovery', 260.0, 260.0 + out, 1), ('PaMapIconDragonTears_00_Zoom', 1.08, 1.08 * wide, (324,)),
            ('PaMapIconDragonTears_00_Zoom', 1.28, 1.28 * wide, (336,))]

def panes(blarc, lo, hi):
    """pane name -> absolute section offset, first occurrence, for the BFLYT at blarc[lo:hi]."""
    assert blarc[lo:lo + 4] == b'FLYT', 'not a BFLYT'
    head, count = struct.unpack_from('<H', blarc, lo + 6)[0], struct.unpack_from('<H', blarc, lo + 0x10)[0]
    pos, out = lo + head, {}
    for _ in range(count):
        magic, size = bytes(blarc[pos:pos + 4]), struct.unpack_from('<I', blarc, pos + 4)[0]
        if magic in PANE_MAGICS:
            out.setdefault(bytes(blarc[pos + 12:pos + 36]).split(b'\0')[0].decode(), pos)
        pos += size
    assert pos <= hi
    return out

def apply(blarc, members, width, height, hud_corner=True):
    """Edits blarc (bytearray) in place for a width x height canvas. Returns the members it changed. hud_corner False leaves the
    layouts that hang from an edge, and the panes placed at one, where the squeeze puts them (the middle 16:9)."""
    k = (width / height) / (16 / 9)
    assert k < 1, 'only for canvases taller than 16:9'
    changed, found = set(), {}
    def pane_offset(layout, pane):
        member = f'blyt/{layout}.bflyt'
        if member not in members: raise SystemExit(f'no layout {layout}')
        if member not in found: found[member] = panes(blarc, *members[member])
        changed.add(member)
        if pane not in found[member]: raise SystemExit(f'{layout}: no pane {pane}')
        return found[member][pane]
    unknown = sorted(n for n in (*KEEP_ROOT, *ROOT_UP, *ROOT_DOWN) if f'blyt/{n}.bflyt' not in members)
    if unknown: raise SystemExit(f'layouts not in the archive: {unknown}')
    for member in sorted(m for m in members if m.startswith('blyt/') and m.endswith('.bflyt')):
        layout = member[5:-6]
        if layout in KEEP_ROOT: continue
        at = pane_offset(layout, 'RootPane')
        struct.pack_into('<f', blarc, at + FIELD['sy'], k)
        if hud_corner and (layout in ROOT_UP or layout in ROOT_DOWN) and layout not in CENTERED:
            struct.pack_into('<f', blarc, at + FIELD['y'], (1 if layout in ROOT_UP else -1) * 540 * (1 - k))
    # Every edit's old value is the value the game ships: checked before writing, so a version with other numbers stops here.
    for layout, pane, field, shipped, value in edits(k):
        if not hud_corner and field == 'y': continue
        if layout in CENTERED and pane == 'N_SystemBG_00' and field == 'y': continue   # it moved down to make up for the root's shift
        at = pane_offset(layout, pane) + FIELD[field]
        have = struct.unpack_from('<f', blarc, at)[0]
        if abs(have - shipped) > 1e-3: raise SystemExit(f'{layout} {pane} {field}: {have}, expected {shipped}')
        struct.pack_into('<f', blarc, at, shipped + value if field == 'y' else value)   # a y is a distance, a scale the new scale
    if hud_corner:
        add = 540 * (2 - k) / k - 563   # the list window's bottom reaches the canvas's edge
        for layout, pane, height, y in LIST_EXTENSION:
            at = pane_offset(layout, pane)
            for field, shipped, value in (('h', height, height + add), ('y', y, y - add / 2)):
                have = struct.unpack_from('<f', blarc, at + FIELD[field])[0]
                if abs(have - shipped) > 1e-3: raise SystemExit(f'{layout} {pane} {field}: {have}, expected {shipped}')
                struct.pack_into('<f', blarc, at + FIELD[field], value)
    for anim, shipped, value, count in anim_edits(k):
        if not hud_corner and anim != 'PaMapIconDragonTears_00_Zoom': continue
        member = f'anim/{anim}.bflan'
        if member not in members: raise SystemExit(f'no animation {anim}')
        lo, hi = members[member]
        hits = [lo + n for n in count] if isinstance(count, tuple) else [i for i in range(lo, hi - 3, 4) if abs(struct.unpack_from('<f', blarc, i)[0] - shipped) < 1e-4]
        if not isinstance(count, tuple) and len(hits) != count: raise SystemExit(f'{anim}: {len(hits)} keys of {shipped}, expected {count}')
        if any(abs(struct.unpack_from('<f', blarc, i)[0] - shipped) > 1e-4 for i in hits): raise SystemExit(f'{anim}: not {shipped} at {count}')
        for i in hits: struct.pack_into('<f', blarc, i, value)
        changed.add(member)
    return sorted(changed)

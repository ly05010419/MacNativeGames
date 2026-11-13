#!/usr/bin/env python3
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables (and patches.py, the rewriter's patches of the game's builds) in the same folder. The shader tools, the host build, iOS packaging and the NSP script take each game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> [<default>] (print that key's value, for shell; without a default a missing key is an error); game.py header <id> <out.h> (generate the host's game_profile.h);
#   game.py modules <id> <folder of rewritten modules> (the modules to load from it: the build row of its main's build, else the profile's modules);
#   game.py dir <id> (print the profile folder); game.py link <id> <link> (point a symlink at the profile folder: make cannot take paths with spaces)
#   From Python: import game; g = game.load() (takes DM_GAME; there is no default game: unset is an error that lists the profiles); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
# DM_GAMES_DIR (folders separated by ':') adds profile folders outside the repository, searched after dm/games: the NSP script writes a provisional profile for a game without one into
#   <NSP folder>/<id>_build/profile/<id>/ and points DM_GAMES_DIR at <id>_build/profile, so the shader tools, the host build and build_ipa.sh all find it.
#   DM_GAMES_FIRST=1 searches DM_GAMES_DIR before dm/games (the NSP script's --provisional: a provisional profile for a game that has one in dm/games, to test the guesses)
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve(); self.id = self.dir.name; self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t'); self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # every row of this key (keys such as build repeat)
        return [v for k, v in self.rows if k == key]

    def get(self, key, default=None):
        r = self.values(key)
        if r: return '\t'.join(r[0])
        if default is None: raise KeyError(f'{self.dir / "game.tsv"} has no key {key}')
        return default

    def words(self, key, default=None): return self.get(key, default).split()
    def int(self, key): return int(self.get(key), 0)
    def table(self, name): return self.dir / name

    @property
    def title_id(self): return int(self.get('title_id'), 16)

    def builds(self):
        """{version: {'display': display version, module name: build ID, ...}}"""
        out = {}
        for ver, disp, *mods in self.values('build'):
            out[ver] = dict([('display', disp)] + [tuple(m.split('=', 1)) for m in mods])
        return out

    def build_modules(self):
        """{main build ID: [modules]} for each build row with a main: the modules that build has, in the load order of modules (Mario Kart World: the base game's main subsdk0 sdk,
        1.8.0's main subsdk0 subsdk1 sdk). A row may name modules the profile does not load (Hades' subsdk0 and subsdk1): they stay unloaded"""
        order, out = self.words('modules'), {}
        for b in self.builds().values():
            if 'main' in b: out[b['main']] = [m for m in order if m in b]
        return out

    def modules_in(self, folder):   # the modules of the profile a version's ExeFS (or its rewritten modules' folder) holds, in load order: a game's versions may differ in modules
        folder = Path(folder)
        return [m for m in self.words('modules', 'main') if (folder / m).is_file() or (folder / f'{m}.patched.bin').is_file()]

    def pairs(self, key):   # "a=b c=d" → [(a, b), (c, d)]
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def extra_dirs():   # profile folders outside the repository (DM_GAMES_DIR), searched after dm/games
    return [Path(p) for p in os.environ.get('DM_GAMES_DIR', '').split(':') if p]


def search_dirs():   # dm/games, then DM_GAMES_DIR; DM_GAMES_FIRST=1 the other way round
    return extra_dirs() + [HERE] if os.environ.get('DM_GAMES_FIRST') else [HERE] + extra_dirs()


def find_dir(name):   # the profile folder of a title ID (the first of search_dirs that has it); a path is taken as is
    if os.sep in str(name): return Path(name)
    return next((d / name for d in search_dirs() if (d / name / 'game.tsv').exists()), HERE / name)


def load(name=None):
    name = name or os.environ.get('DM_GAME')
    if not name:   # there is no default game
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] DM_GAME is not set: pick a game profile by base game title ID: {have}')
    d = find_dir(name)
    if not (d / 'game.tsv').exists():   # profile folders are named after the base game's title ID (the old names smbw / fe_engage are gone)
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games():
    out, seen = [], set()
    for root in search_dirs():
        if not root.is_dir(): continue
        for d in sorted(root.iterdir()):
            if (d / 'game.tsv').exists() and d.name not in seen: seen.add(d.name); out.append(Game(d))
    return out


def by_title(tid):
    return next((g for g in all_games() if g.title_id == tid), None)


def by_build(main_id):   # the profile whose build rows register this main build ID (a release of the same build under another title ID: the NSP script's same_build)
    return next((g for g in all_games() if any(b.get('main') == main_id for b in g.builds().values())), None)


# agl's upscaler network (NDSR), by where the shader map found each key: the compute shaders of agl_ndsr.pck (Donkey Kong Bananza's 24 in its release library, Splatoon Raiders' 20 in its debug one)
UPSCALER_SOURCE = re.compile(r'/agl_ndsr\.pck#')


def upscaler_keys(g):
    """the game's own upscaler shaders, from its shader map (shader_split.py's source column): sorted 16-digit keys, none without a map"""
    p = g.table('shader_map.tsv')
    if not p.exists(): return []
    keys = set()
    for line in p.read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 5 and c[2] == 'compute' and UPSCALER_SOURCE.search(c[4]) and re.fullmatch(r'[0-9a-f]{16}', c[3]): keys.add(c[3])
    return sorted(keys)


def header(g):
    """game_profile.h of a profile: one profile's constants, or with the key programs (a multi-program application packed as one game: Super Mario 3D All-Stars) a table of its programs' profiles"""
    return header_multi([(0, g)] + [(int(n), load(i)) for n, i in g.pairs('programs')]) if g.get('programs', '') else header_one(g)


def header_multi(progs):
    """One host over the programs of a multi-program application: every program's constants in one table, dm_games, and DM_GAME_* macros that read the entry of the running program
    (dm_game_cur, main.c: DM_PROGRAM, program 0 without it). progs: [(program index, profile)], program 0 first. A host switches programs by starting again (hle_oe.c), so the entry never changes in a run"""
    rows, keys = [], []
    for n, g in progs:
        d = {m[1]: m[2] for m in (re.match(r'#define (DM_GAME_\w+) (.*)$', ln) for ln in header_one(g).splitlines()) if m}
        rows.append((n, g, d)); keys += [k for k in d if k not in keys]
    ARR = {'DM_GAME_MODULES': 0, 'DM_GAME_BUILD_MODULES': 2, 'DM_GAME_DISPLAY_VERSIONS': 2}   # brace lists, of strings (their entries in rows of 2 for the two), each padded with NULLs in the table
    for k in ('DM_GAME_SHARED_FONT_TTF', 'DM_GAME_NVN_API_MAJOR', 'DM_GAME_NVN_API_MINOR'):
        if k in keys: sys.exit(f'{k}: a key behind this macro (shared_font, device_api) is tested by the preprocessor and cannot differ per program; not supported with programs')
    typ = lambda v: 'const char *' if v.startswith('"') else 'uint64_t ' if v.endswith('ull') else 'int '
    fld = lambda k: k[len('DM_GAME_'):].lower()
    # A macro's size in the table: the longest list among the programs
    strs = lambda v: re.findall(r'"(?:[^"\\]|\\.)*"', v)
    sz = {k: max(len(strs(d.get(k, ''))) for _, _, d in rows) for k in ARR}
    scalars = [k for k in keys if k not in ARR]
    for k in scalars:
        vs = {typ(d[k]) for _, _, d in rows if k in d}
        if len(vs) > 1: sys.exit(f'{k}: different types among the programs')
    L = [f'// Generated by dm/games/game.py from the profiles of programs {", ".join(f"{n} ({g.id})" for n, g, _ in rows)}; do not edit', '#pragma once', '#include <stdint.h>', '#include <stddef.h>', '#define DM_GAME_MULTI 1',
         'typedef struct { int program; ' + ' '.join(f'{typ(next(d[k] for _, _, d in rows if k in d))}{fld(k)};' for k in scalars) + ' ' + ' '.join(f'const char *{fld(k)}[{sz[k] or 1}];' for k in ARR) + ' } dm_game_t;']
    ents = []
    for n, g, d in rows:
        ents.append('    {' + f'.program = {n}, ' + ', '.join(f'.{fld(k)} = {d[k]}' for k in scalars if k in d) + ', ' + ', '.join(f'.{fld(k)} = {{{", ".join(strs(d.get(k, "")))}}}' for k in ARR) + '},')
    L += ['static const dm_game_t dm_games[] __attribute__((unused)) = {', *ents, '};',
          'const dm_game_t *dm_game_cur(void);   // main.c: the entry of the program DM_PROGRAM names']
    L += [f'#define {k} (dm_game_cur()->{fld(k)})' for k in scalars]
    L += [f'#define {k} {{' + ', '.join(f'dm_game_cur()->{fld(k)}[{i}]' for i in range(sz[k])) + '}' for k in ARR]
    return '\n'.join(L + [''])


def header_one(g):
    """the host's game_profile.h: the profile constants the host uses, as macros"""
    q = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    mods = g.words('modules')
    vers = ', '.join('{' + q(b['main']) + ', ' + q(b['display']) + '}' for b in g.builds().values() if 'main' in b)
    pi = g.get('present_interval', 'game')   # present interval: game = as the game sets it (as on the Switch), display = one frame per display tick
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval must be game or display: {pi}')
    ft = g.get('fps_target', '0')   # frame rate cap of the rewriter's frame step (FRAME_STEP in the profile's patches.py: Tears of the Kingdom 1.4.3, Breath of the Wild 1.9.0) or of the engine's own frame limit (hle_fpslock.c): 0 = the game's own step, 30 / 60
    if ft not in ('0', '30', '60'): sys.exit(f'{g.id}: fps_target must be 0, 30 or 60: {ft}')
    fm = g.get('fps_max', '60')   # the highest rate fps_target and the player's Frame rate may take: 120 only for a game checked at that speed (hle_nvn.c fps_high)
    if fm not in ('60', '120'): sys.exit(f'{g.id}: fps_max must be 60 or 120: {fm}')
    fh = g.get('fps_hold', 'on')   # whether the frame step may hold a frame longer than the cap's vsyncs when frames come late (hle_nvn.c fstep_tick): on / off
    if fh not in ('on', 'off'): sys.exit(f'{g.id}: fps_hold must be on or off: {fh}')
    utp = g.get('ue_texture_pool', '0')   # Unreal's r.Streaming.PoolSize in MB, raised to this on the Mac (hle_uecvar.c; DM_UE_TEXTURE_POOL overrides, and turns it on for iOS): 0 = the game's own pool
    if not utp.isdigit(): sys.exit(f"{g.id}: ue_texture_pool must be megabytes (0 = the game's own pool): {utp}")
    ufc = g.get('ue_fps_cvar', 't.MaxFPS')   # the Unreal console variable the host writes fps_target into (hle_fpslock.c; found by name by the rewriter, ue_cvar.py NAMES): a game's own frame limit its settings apply to t.MaxFPS
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.]{0,30}', ufc): sys.exit(f'{g.id}: ue_fps_cvar must be a console variable name: {ufc}')
    ps = g.get('present_snap', 'off')   # present snapshots by default (nvn_mtl.m mtl_snap_on; DM_PRESENT_SNAP and the iOS launcher's Frame buffering override it): on / off
    if ps not in ('on', 'off'): sys.exit(f'{g.id}: present_snap must be on or off: {ps}')
    p4 = g.get('pso_mtl4', 'off')   # render pipelines through MTL4Compiler on iOS by default, with the pipeline archive and hybrid pipelines (on by default on the Mac; DM_PSO_MTL4 overrides): on / off
    if p4 not in ('on', 'off'): sys.exit(f'{g.id}: pso_mtl4 must be on or off: {p4}')
    pg = g.words('pso_gpu', '')   # GPU architectures the app gets the pipeline archive made offline for (the NSP script's --gpu, build_ipa.sh): when pso_mtl4 is on the app gets these instead of the default g18p, the A19 / A19 Pro of the iPhone 17 family (e.g. 'applegpu_g19p' for a device of the family after it); unset → the default. Whitespace-separated tokens, with or without the applegpu_ prefix
    if pg and (len(pg) > 4 or any(w == 'none' or not re.fullmatch(r'(applegpu_)?[A-Za-z0-9]+', w) for w in pg)):
        sys.exit(f"{g.id}: pso_gpu takes 1-4 whitespace-separated GPU architectures such as 'g18p applegpu_g19p' (each an optional applegpu_ prefix then an alphanumeric token; there is no none: omit the key for g18p with pso_mtl4 on, or no archive with pso_mtl4 off): {pg}")
    et = g.get('enc_thread', 'off')   # Metal encoding on its own thread on the Mac by default (nvn_rec.h rec_on; on iOS it is on for every game; DM_NVN_ENC_THREAD overrides): on / off
    if et not in ('on', 'off'): sys.exit(f'{g.id}: enc_thread must be on or off: {et}')
    cp = g.get('core_preempt', 'off')   # same-core preemption approximation (hle_thread.c): on / off
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt must be on or off: {cp}')
    aba = g.get('alloc_big_align', 'off')   # nn::mem::StandardAllocator::Allocate aligns blocks of 64 KB or more to 64 KB (hle_os.c; Vanillaware's memory pool storage): on / off
    if aba not in ('on', 'off'): sys.exit(f'{g.id}: alloc_big_align must be on or off: {aba}')
    sf = g.get('shared_font', 'bfttf')   # what the shared-font address points at (hle_sdk16.c): bfttf = 8-byte header + TTF, ttf = the TTF alone
    if sf not in ('bfttf', 'ttf'): sys.exit(f'{g.id}: shared_font must be bfttf or ttf: {sf}')
    pl = g.get('pad_layout', 'simple')   # the virtual gamepad's first layout (DmGamepad.m; afterwards whatever the player switched to in game is kept): simple = 2D, full = 3D (D-pad, right stick, L3 / R3),
    if pl not in ('simple', 'full', 'touch'): sys.exit(f'{g.id}: pad_layout must be simple, full or touch: {pl}')   # touch = Tap (no control: every touch goes to the game's touch screen)
    sks = g.get('shader_key_skip', '')   # control-section bytes the shader keys hash as zeros, <offset>+<length> (hle_nvn.c keys_hash, shader_split.py key_ctl; Batman: Arkham Knight: GLSLC's compile information, 0x770+0x78)
    m = re.fullmatch(r'(0x[0-9a-fA-F]+|\d+)\+(0x[0-9a-fA-F]+|\d+)', sks) if sks else None
    if sks and (not m or not 0x30 <= int(m[1], 0) or not 0 < int(m[2], 0) or int(m[1], 0) + int(m[2], 0) > 0x880 or (int(m[1], 0) < 0x760 and int(m[1], 0) + int(m[2], 0) > 0x700)):   # not over +0x700..+0x75f, the code area and compute fields the tools read
        sys.exit(f'{g.id}: shader_key_skip takes <offset>+<length> within the first 0x880 bytes of the control section, past its header (+0x30) and clear of +0x700..+0x75f: {sks}')
    sko, skl = (int(m[1], 0), int(m[2], 0)) if m else (0, 0)
    rcd = g.get('render_cond', 'off')   # NVN conditional rendering evaluated at replay, timestamp reports with counter 0 (nvn_mtl.m OP_RENDER_COND): on / off
    if rcd not in ('on', 'off'): sys.exit(f'{g.id}: render_cond must be on or off: {rcd}')
    tfu = g.get('tex_flush_upload', 'off')   # textures written by the CPU into a pool and published with nvnMemoryPoolFlushMappedRange are uploaded like packaged ones (hle_nvn.c): on / off
    if tfu not in ('on', 'off'): sys.exit(f'{g.id}: tex_flush_upload must be on or off: {tfu}')
    tdu = g.get('tex_dirty_upload', 'off')   # with tex_flush_upload: a registered texture the CPU writes after init without any flush is uploaded too (dirty pages, hle_nvn.c nvn_ft_written): on / off
    if tdu not in ('on', 'off') or (tdu == 'on' and tfu != 'on'): sys.exit(f'{g.id}: tex_dirty_upload must be on or off, and needs tex_flush_upload on: {tdu}')
    tds = g.get('tex_dyn_slots', 'off')   # shaders whose texture handle the translator could not trace to one slot: slot indices in c2, the bound slots in the fallback arrays (nvn_mtl.m): on / off
    if tds not in ('on', 'off'): sys.exit(f'{g.id}: tex_dyn_slots must be on or off: {tds}')
    vcr = g.get('vconv_range', 'off')   # vertex attributes converted on the CPU only over the vertices each draw reads (nvn_mtl.m vc_draw_range): on / off
    if vcr not in ('on', 'off'): sys.exit(f'{g.id}: vconv_range must be on or off: {vcr}')
    mml = g.get('mem_map_limit', 'off')   # MapPhysicalMemory refused beyond the application pool, as on Horizon (hle_svc.c): on / off
    if mml not in ('on', 'off'): sys.exit(f'{g.id}: mem_map_limit must be on or off: {mml}')
    ups = upscaler_keys(g)   # the game's own upscaler shaders, which DM_UPSCALER=metalfx (the launcher's Game's upscaler) replaces with MetalFX's spatial scaler (nvn_mtl.m up_mfx_encode)
    dbr = g.get('draw_barriers', 'full')   # how draw passes are ordered (nvn_mtl.m barrier_game): full = each waits for all earlier GPU work, game = by the game's own barriers, so vertex work overlaps the previous pass's fragments
    if dbr not in ('full', 'game'): sys.exit(f'{g.id}: draw_barriers must be full or game: {dbr}')
    gts = g.get('gpu_timestamps', 'submission')   # counter timestamps (nvn_mtl.m rep_ticks): submission = one time per submission, segment = the GPU start / end of the segment holding the report
    if gts not in ('submission', 'segment'): sys.exit(f'{g.id}: gpu_timestamps must be submission or segment: {gts}')
    om = g.get('op_mode', 'console')   # the operation mode reported at start (hle_oe.c dm_opmode; DM_OPMODE overrides): console = docked, handheld = touch screen in use (Super Mario Galaxy's pointer)
    if om not in ('console', 'handheld'): sys.exit(f'{g.id}: op_mode must be console or handheld: {om}')
    if pl == 'touch' and om != 'handheld': sys.exit(f'{g.id}: pad_layout touch needs op_mode handheld (the touch screen reaches the game only in handheld mode)')
    pm = g.get('perf_mode', 'follow')   # the performance mode reported (hle_oe.c): follow = the operation mode's (Boost docked, Normal handheld), boost = Boost always
    if pm not in ('follow', 'boost'): sys.exit(f'{g.id}: perf_mode must be follow or boost: {pm}')
    dmo = g.get('depth_mode', 'gl')   # the NVN depth mode the game sets, for the pipeline archive made offline (dm/gfx/tools/pso_offline.py; the host follows the game at run time): gl / zero
    if dmo not in ('gl', 'zero'): sys.exit(f'{g.id}: depth_mode must be gl or zero: {dmo}')
    api = g.get('device_api', '55.15')   # the NVN API major.minor version nvnDeviceGetInteger reports (hle_nvn.c)
    if not re.fullmatch(r'\d+\.\d+', api): sys.exit(f'{g.id}: device_api must be major.minor: {api}')
    src = f'dm/games/{g.id}' if g.dir.parent == HERE else str(g.dir)
    L = [f'// Generated by dm/games/game.py from {src}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(g.id)}',
         f'#define DM_GAME_NAME {q(g.get("name"))}',
         f'#define DM_GAME_PROGRAM_ID 0x{int(g.get("program_id"), 16):016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, mods))}}}',
         f'#define DM_GAME_NMOD {len(mods)}',
         # each build's own modules (main.c picks the row of main's build ID, else DM_GAME_MODULES): a version may lack a module another has
         f'#define DM_GAME_BUILD_MODULES {", ".join("{" + q(bid) + ", " + q(" ".join(ms)) + "}" for bid, ms in g.build_modules().items()) or "{0}"}',
         f'#define DM_GAME_ROMFS_MARKER {q(g.get("romfs_marker"))}',
         f'#define DM_GAME_MEM_RESERVE {g.int("mem_reserve"):#x}ull',
         f'#define DM_GAME_MEM_ALIAS {g.int("mem_alias"):#x}ull',
         f'#define DM_GAME_MEM_STACK {g.int("mem_stack"):#x}ull',
         f'#define DM_GAME_APP_POOL {g.int("app_pool"):#x}ull',
         f'#define DM_GAME_MEM_USED {int(g.get("mem_used", "0x4000000"), 0):#x}ull',   # used memory reported beside the heap (hle_svc.c GetInfo UsedMemorySize)
         f'#define DM_GAME_SYS_RESOURCE {int(g.get("sys_resource", "0x2000000"), 0):#x}ull',   # the NPDM's system resource size (hle_svc.c GetInfo TotalSystemResourceSize; 0 = no virtual address memory)
         f'#define DM_GAME_ARGS {q(g.get("game_args", ""))}',   # game arguments when the command line gives none (main.c; Octopath Traveler 0: Unreal's -SwitchVirtualAddressSpaceSize=)
         f'#define DM_GAME_ENV {q(g.get("game_env", ""))}',   # KEY=VALUE pairs getenv answers (hle_os.c; Absolum: .NET's DOTNET_GCRegionRange)
         f'#define DM_GAME_LAUNCH_PARAM {q(g.get("launch_param", ""))}',   # what oe::TryPopLaunchParameter hands over once (hle_oe.c; a program another program of the application launches: Super Mario 3D All-Stars)
         f'#define DM_GAME_MEM_MAP_LIMIT {int(mml == "on")}',
         f'#define DM_GAME_STDALLOC_CAP {int(g.get("stdalloc_cap", "0"), 0):#x}ull',   # StandardAllocator's address space with virtual address memory (hle_svc.c); 0 = what the alias region has room for
         f'#define DM_GAME_SCREEN_ASPECT {("off", "on").index(g.get("screen_aspect", "off"))}',   # stubs that follow a screen taller than 16:9 (DM_SCREEN_ASPECT): off / on
         f'#define DM_GAME_DISPLAY_VERSIONS {vers}',
         f'#define DM_GAME_PRESENT_DISPLAY {int(pi == "display")}',
         f'#define DM_GAME_CORE_PREEMPT {int(cp == "on")}',
         f'#define DM_GAME_ALLOC_BIG_ALIGN {int(aba == "on")}',
         f'#define DM_GAME_FPS_TARGET {int(ft)}',
         f'#define DM_GAME_FPS_MAX {int(fm)}',
         f'#define DM_GAME_FPS_HOLD {int(fh == "on")}',
         f'#define DM_GAME_UE_TEXTURE_POOL {int(utp)}',
         f'#define DM_GAME_UE_FPS_CVAR {q(ufc)}',
         f'#define DM_GAME_PRESENT_SNAP {int(ps == "on")}',
         f'#define DM_GAME_PSO_MTL4 {int(p4 == "on")}',
         f'#define DM_GAME_ENC_THREAD {int(et == "on")}',
         *([f'#define DM_GAME_SHARED_FONT_TTF {int(sf == "ttf")}'] if g.get('shared_font', '') else []),   # without the key the host decides by the SDK version (hle_sdk16.c)
         f'#define DM_GAME_PAD_LAYOUT {("simple", "full", "touch").index(pl)}',
         f'#define DM_GAME_OPMODE {int(om == "console")}',
         f'#define DM_GAME_PERF_BOOST {int(pm == "boost")}',
         f'#define DM_GAME_TEX_FLUSH_UPLOAD {int(tfu == "on")}',
         f'#define DM_GAME_SHADER_KEY_SKIP_OFF {sko:#x}', f'#define DM_GAME_SHADER_KEY_SKIP_LEN {skl:#x}',
         f'#define DM_GAME_RENDER_COND {int(rcd == "on")}',
         f'#define DM_GAME_TEX_DIRTY_UPLOAD {int(tdu == "on")}',
         f'#define DM_GAME_TEX_DYN_SLOTS {int(tds == "on")}',
         f'#define DM_GAME_GPU_TS_SEGMENT {int(gts == "segment")}',
         f'#define DM_GAME_DRAW_BARRIERS_GAME {int(dbr == "game")}',
         f'#define DM_GAME_UPSCALER_KEYS {q(",".join(ups))}', f'#define DM_GAME_UPSCALER_N {len(ups)}',
         f'#define DM_GAME_VCONV_RANGE {int(vcr == "on")}',
         *([f'#define DM_GAME_NVN_API_MAJOR {api.split(".")[0]}', f'#define DM_GAME_NVN_API_MINOR {api.split(".")[1]}'] if g.get('device_api', '') else []),   # without the key: by the SDK version (hle_nvn.c nvn_api)
         '']
    return '\n'.join(L)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['list']:
        for g in all_games(): print(f'{g.id}\t{g.title_id:016X}\t{g.get("name", "")}')
    elif len(a) == 3 and a[0] == 'header':
        out, text = Path(a[2]), header(load(a[1])); out.parent.mkdir(parents=True, exist_ok=True)
        if not (out.exists() and out.read_text() == text):   # written only when it changes: make runs this every time (the profile folder can change under the same GAME), and an unchanged header must not rebuild the host
            tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)
    elif len(a) == 3 and a[0] == 'modules':   # the modules to load from a folder of rewritten modules: its main build's row, else all the profile's (main.c profile_modules)
        g, bid = load(a[1]), re.search(r'^# BUILD_ID\s+(\w+)', (Path(a[2]) / 'main.fixups.tsv').read_text(errors='replace'), re.M)
        print(' '.join(g.build_modules().get(bid.group(1) if bid else '', g.words('modules'))))
    elif len(a) == 2 and a[0] == 'dir':
        print(load(a[1]).dir)
    elif len(a) == 3 and a[0] == 'link':   # link → the profile folder (replaced when it points elsewhere)
        d, ln = load(a[1]).dir, Path(a[2]); ln.parent.mkdir(parents=True, exist_ok=True)
        if not (ln.is_symlink() and Path(os.readlink(ln)) == d):
            if ln.is_symlink() or ln.exists(): ln.unlink()
            ln.symlink_to(d)
    elif len(a) in (2, 3):
        print(load(a[0]).get(*a[1:]))
    else:
        sys.exit(__doc__ or 'usage: game.py list | game.py <id> <key> [<default>] | game.py header <id> <out.h> | game.py modules <id> <rewritten modules> | game.py dir <id> | game.py link <id> <link>')

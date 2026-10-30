#!/usr/bin/env python3
# All of Mario's shaders → the "package directory" dmsh_build.py understands + the shader map + the translation query answer table (thread group / shared memory of compute shaders).
# Sources (a full scan, not relying on file names): every file under RomFS (a zstd file is decompressed first, whatever its name: Mario's .zs, Paper Mario: The Thousand-Year Door's .zst; Yaz0 is decompressed, SARC is expanded member by member) and ExeFS main (NSO, lz4-decompressed), looking for
#   · BNSH ('BNSH\0\0\0\0', carried by bfsha / particle effect esetb / UI layout blarc / embedded in main):
#       +0x60 grsc block; +0x7c variation count u32; +0x80 variation table pointer (pointers are relative to the BNSH start). A variation is 0x40 bytes; the binary program is at +0x10;
#       the binary program has 6 stage pointers from +0x08: vertex, hull, domain, geometry, pixel, compute; the stage code descriptor has +0x08 control pointer, +0x10 code area pointer, +0x18 code area length, +0x1c control length.
#   · BAHS (agl's sharcb: the agl / gsys resource packs, GameAglShader in the Secred pack): file header +0x1c code region start, +0x18 code region length;
#       each binary entry in the table area is 16 bytes {code offset (relative to the code region, 0x100-aligned), 0, control length 0x880 / 0x88c (3D World 0x848), signed offset of the control relative to this entry's address}.
#       Control sections are shared between programs, so pairing goes by entry, never by finding code from a control; entries are recognized by a triple check of "control magic + code magic + consistent lengths".
#   · Unity (UnityFS bundles and the serialized files under Data, read by unityfs.py): the Switch subprograms of each Shader object; the program code = a 0x60 header + one block per stage.
#       The header is 4 i32 ×6 arrays indexed by NVN stage (0 vertex 1 pixel 2 geometry 3 hull 4 domain 5 compute): [0] unused (0 or a scratch size), [1] block offset (from +0x60, -1 = no such stage), [2] control length, [3] code length;
#       block = control section + code area. A program's vertex / pixel stages form a pair. The game's own serialized files (data.unity3d, unity default resources) carry no type trees and borrow the bundles' Shader type tree of the same hash (second pass),
#       or take the layout unityfs.py has for that hash (KNOWN_TREES) when no bundle has one (Brain Memory Training Academy, Unity 2022.3: no bundles at all)
#       Before 2019.3 (Cuphead, Unity 2017.4; found by armyja, GitHub pull request #2) a subprogram holds one stage: program code = a 0x10 header {?, ?, control length, code length} + control section
#       + code area, and the stage is not in the code but in the Shader's m_ParsedForm (unity_stages). Its pass's pixel-input cover supplies the vertex padding pairs (unity_pass_pairs)
#       Unity 2022 (Ace Attorney Investigations Collection, 2022.3) keeps the 0x60 header in the program code, but the blob also holds parameter entries, which unity_code_blobs leaves out
#       A game's own GLSLC outputs in a TextAsset are read too (FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition, Unity 2018.4: shader.switch.vfxsb, unity_text_glslc)
#   · MSHD (MercurySteam, Metroid Dread system/shd/*.bshdat): each stage = a control section (+0x20 = control length; sometimes a small compiler option table follows) + the code area starting at the next code header;
#       NV assembly text follows the first 0x880 bytes of the control section, and SASS disassembly text follows the code area. All 478 files hold "vertex, pixel" in pairs (1, 2 or 4 pairs), paired two by two in order
#   · G2S (Koei Tecmo's KTGL, Hyrule Warriors: Age of Calamity): GLSLC compiler outputs inside the IDRK entries of the RDB data files (zlib in chunks), see idrk_shaders;
#       ROMANCE OF THE THREE KINGDOMS 8 REMAKE's newer RDB keeps them in .fdata files (a PDRK header, then IDRK entries of zlib or LZ4 chunks)
#   · SPKG (the older KTGL of Fire Emblem: Three Houses): shader packages of G2S containers, loose and inside the LINKDATA archive DATA0.bin / DATA1.bin, see kt_shaders and linkdata_jobs
#   · SDF (Capcom's RE Engine, Monster Hunter Rise): shader definitions inside the PAK archives (KPKA, entries zstd / deflate compressed), only the newest copy of each path, see kpka_jobs and sdf_shaders
#   · Trinity packs (Game Freak, Pokémon Legends: Z-A): BNSH inside the files of arc/data.trpfs (Oodle Mermaid, decoded by the game's own decoder in the Mac host), see trpfs_jobs and trinity_shaders
#   · SPK (Capcom's MT Framework, The Great Ace Attorney Chronicles): the shader package inside an ARC archive (zlib entries), code area + control section per shader, see mtarc_members and mtspk_shaders
#   · BEA (NintendoWare Bezel Engine archives, Big Brain Academy: Brain vs. Brain): BNSH in the .bfsha entries of the archives (zstd entries), see bea_members
#   · MGFX (MonoGame effects, Celeste): the effects in its XNB files and SpriteBatch's in main, each shader a control section and a code area, see mgfx_effect
#   · dvdbnd (FromSoftware, Dark Souls Remastered): GLSLC outputs in the BND3 archives (DCX zlib entries) of the .bdt files, indexed by the .bhd5 beside each, see bhd5_entries and bdt_shaders
#   · data.arc (Super Smash Bros. Ultimate, smash_arc.py): GLSLC outputs in its SSBH shader databases and BNSH in its effects and UI layouts, each of its current files (zstd) scanned, see smash_arc_jobs
#   · PKZ (PlatinumGames, Bayonetta 3): shader objects (VSO / PSO / CSO / HSO / DSO, a control section and a code area each) in the DAT archives of the PKZ entries (Oodle Kraken, decoded by the
#       game's own decoder in the Mac host), see pkz_entries, pgdat_members and pg_shader
#   · CPK (CRI's file system, Bayonetta 2): BNSH files ('.vso' / '.pso') in the PlatinumGames DAT archives of the CPK entries (CRILAYLA entries, cpk.py), see cpk_jobs
#       NieR:Automata's DAT members instead prefix length-delimited NVN code and control sections with a binding table (pg_sized_shader)
#   · HNSHV002 (Needle, Persona 5 Royal): NVN section descriptors in the shader containers embedded in main and the GSC files of its CPK archives, see needle_shaders
#   · GFPAK (Game Freak, Pokémon Sword, Pokémon Legends: Arceus): BNSH entries of the archives under bin/archive (Sword's LZ4; Arceus's Oodle Kraken, decoded by the game's own decoder in the Mac host), see gfpak_members
#   · .tfxo (Silicon Studio's YEBIS post effects, Pokémon Sword): control sections each followed by its code area, in a zlib stream in main, see tfxo_streams and tfxo_shaders
#   · control sections loose in main, in no GLSLC output or BNSH file (Advance Wars 1+2: Re-Boot Camp's Bink 2 shaders): paired with their code areas by data records or adjacency, see raw_shaders
#   · SNVN (Retro Studios, Donkey Kong Country: Tropical Freeze): zlib program records in the SNVN chunks of MaterialArchive.arc, of the MTRL resources in its .pak archives
#       and of the startup material archive LZSS-compressed in main, see rfrm_shaders and retro_main_forms
#   · BERG (Snowdrop, Mario + Rabbids Sparks of Hope): shader objects in the individually zstd-compressed records of its SDF data slices, see snowdrop_shaders
# Code area: 0x30 header + (graphics) 0x50 SPH + instructions; header magic 0x12345678 for graphics, 0x12345679 for compute. The control section starts with 0x98761234, +0x704 = code area length,
#   +0x700 = start of the c1 constant segment within the code area; for compute +0x748 thread group xyz (u32 ×3), +0x754 shared memory bytes (checked one by one against PC reflection on D2R's 22 compute shaders).
# Stage: BNSH uses the stage slot; BAHS uses bits 10..13 of SPH word 0 (1 vertex 2 hull 3 domain 4 geometry 5 pixel), and a compute header means compute.
# Key = sha256(first 0x880 bytes of the control section (its own length when control +0x20 says it is shorter, see ctl_len) + code area)[:16], the same as the host's prog_get: in this game many shaders share a control section but differ in code
#   (SecredUber's 16600 stage shaders have only 3079 distinct control sections, with 6367 cases of same control, different code), so hashing the control alone picks the wrong library.
# Output: <package dir>/<stage>_<source group>.bin, each record = 'BTCDBTCD' + header (+0x28 control length, +0x2c code length) padded to 0x38 + code area + control section, one copy per key;
#   map rows have the same columns as D2R's shader_map.tsv (column 5 records where the key first appeared); the answer table has one row per compute shader {"local", "shared"} (shared memory rounded up to 16 bytes, as D2R did).
# Usage: shader_split.py <RomFS dir> <ExeFS main> <output package dir> <output map> <output answer table> [<output pair table> [<output pipeline pair table (pso_pairs.tsv)> [<DLC RomFS dirs, comma-separated>]]]   Environment J=concurrency (default 8)
#   With an update: give RomFS and main as comma-separated lists (base game first, e.g. data/v0/RomFS,data/v327680/RomFS); the output is the union of both versions (see main())
#   Unity type trees need Unity's common string table, which is taken from the first main
# Pair table: (vertex key, pixel key) of every BNSH variation (every Unity program); dmsh_build uses it to give vertex shaders the outputs their pixel shaders read but they do not write (see dmsh_build.py vs_pad)
import collections, concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, time, zlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open, is_image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../games')); import game as _game
from profile_guess import glsl_sources
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import uepak
GAME = _game.load()   # the current game profile (DM_GAME, required): source group rules and the table header note
GROUPS, NOTE = GAME.pairs('shader_groups'), GAME.get('shader_note')
# The sources are what the game files hold, with no profile switch: GLSLC outputs in main (glslc_shaders), the outputs the game compiles at run time when a capture is there (DM_GLSLC_CAPTURE,
# hle_glslc.c), the shader code archives of Unreal paks once decoded on the Mac (uepak.py, DM_OODLE_JOB, hle_oodle.c); the NSP script decides from the files which of the last two to make

CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
def sph_stage(code):
    """A graphics code area's stage from its SPH word 0: shader type in bits 10..13. Catherine: Full Body's GLSLC (SDK 8.2.1, GPU code version 1.15) writes 0 there in every vertex shader
    (word 0 = 0x0061: SPH type 1 VTG, version 3), in its shaders_binary files and in what it compiles at run time, where every other game's vertex shaders have 1 (0x0461); Ryujinx's ShaderHeader
    reads a graphics header's 0 as a vertex shader as well, so it is one here"""
    w = struct.unpack_from('<I', code, 0x30)[0]
    return 'vs' if (w >> 10) & 0xf == 0 and w & 0x1f == 1 else SPH_STAGE.get((w >> 10) & 0xf)
STAGE_NAME = {'vs': 'vertex', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry', 'ps': 'pixel', 'cs': 'compute'}   # shader_map.tsv column 3

def red(msg): sys.exit('[RED] ' + msg)

def zstd_mod():
    """The zstd module: compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API"""
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: red(f'Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    return zstd

def ctl_len(ctl):
    """Number of control bytes in the key: control +0x20 holds the section's own length. Every control section of Mario, Fire Emblem Engage, Metroid Dread and 13 Sentinels is at least 0x880 bytes, so the first 0x880 are used;
    SDK 10's Super Mario 3D World + Bowser's Fury has 2,888 that are only 0x850 bytes (the bfsha in ShaderData, 6 layouts), with other data right after them, so only their own length is used (the same as the host's keys_hash)"""
    n = struct.unpack_from('<I', ctl, 0x20)[0]
    return n if 0x760 <= n < 0x880 else 0x880

def skey(ctl, code):
    return hashlib.sha256(ctl[:ctl_len(ctl)] + code).hexdigest()[:16]

def same_shader(a, b):
    """Two copies of one key hold the same shader: the code areas are equal, and so are the control sections up to their own length (control +0x20). Pokémon Legends: Z-A's UI layout archives
    (__ArchiveShader.bnsh inside the .truiv packs) give 104 KB control sections whose stage descriptor length runs 456 bytes past their own length, and those trailing bytes differ between copies"""
    (ca, ka), (cb, kb) = a, b
    n = struct.unpack_from('<I', ca, 0x20)[0] if len(ca) >= 0x24 else len(ca)
    n = n if 0x760 <= n <= min(len(ca), len(cb)) else max(len(ca), len(cb))
    return ka == kb and ca[:n] == cb[:n]

def check(where, st, ctl, code):
    """control section and code area consistent; returns the reason if not"""
    if ctl[:4] != CTL_MAGIC or len(ctl) < ctl_len(ctl): return 'bad control section'
    # a compute code area may have no header (SDK 10's Super Mario 3D World + Bowser's Fury and D2R: instructions start at +0; dmsh_build strips only a 0x12345679 header)
    if st == 'cs' and code[:4] in (CS_MAGIC, GFX_MAGIC) and code[:4] != CS_MAGIC or st != 'cs' and code[:4] != GFX_MAGIC: return 'code area header magic does not match the stage'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code): return 'control +0x704 does not match the code area length'
    return None

def bnsh_shaders(d, base, label):
    if d[base + 0x60:base + 0x64] != b'grsc': red(f'{label} BNSH @{base:#x} has no grsc block')
    nvar, pvar = struct.unpack_from('<IQ', d, base + 0x7c)
    out = []
    for v in range(nvar):
        prog = struct.unpack_from('<Q', d, base + pvar + 0x40 * v + 0x10)[0]
        if not prog: continue
        for si, st in enumerate(STAGES):
            sc = struct.unpack_from('<Q', d, base + prog + 8 + 8 * si)[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            why = check(label, st, ctl, code)
            if why: red(f'{label} BNSH @{base:#x} variation {v} {st}: {why}')
            out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
    return out

def bahs_shaders(d, label):
    region, rsz = struct.unpack_from('<I', d, 0x1c)[0], struct.unpack_from('<I', d, 0x18)[0]
    if region + rsz > len(d): red(f'{label} BAHS code region out of bounds')
    out = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        if z or not (csz in (0x880, 0x88c) or 0x760 <= csz < 0x880) or off % 0x100: continue   # control length: Mario 0x880 / 0x88c, 3D World's BAHS version 9 0x848
        c = a + rel
        if c < 0 or c + csz > region or d[c:c + 4] != CTL_MAGIC: continue
        n = struct.unpack_from('<I', d, c + 0x704)[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = 'cs' if code[:4] == CS_MAGIC else sph_stage(code) if code[:4] == GFX_MAGIC else None
        if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#BAHS@{a:#x}', st, ctl, code))
    bahs_variations(d, label, [o[0] for o in out])
    return out

BAHS_VAR = {}   # BAHS entry label → the program variation it belongs to (bahs_variations; var_pairs pairs by it)
def bahs_variations(d, label, entries):
    """The variations of an agl BAHS file's programs, for var_pairs. The table area holds a binary array (u32 byte size, u32 count, then per binary a record: u32 record size, u32 kind
    (0 vertex, 1 fragment, 3 compute), two u32, the 16-byte entry bahs_shaders reads) and right after it a program array (u32 byte size, u32 count, then per program: u32 record size,
    u32 name length, u32 stage mask, u32 index of its first binary, its name, its macros). A program's binaries run from its first index to the next program's; the n-th vertex binary
    and the n-th fragment binary in that range are one variation. Breath of the Wild: 109 of the 543 pairs drawn on a cold start in Zora's Domain came from these files, and none was in
    shader_pairs.tsv, which paired only BNSH variations. A file whose arrays do not read back (another layout, or an entry bahs_shaders took that no record holds) gives no pairs"""
    if not entries: return
    taken = {int(e.rsplit('@', 1)[1], 16) for e in entries}
    arr = min(taken) - 0x18
    if arr < 0x20: return
    asz, acnt = struct.unpack_from('<II', d, arr)
    if arr + asz + 8 > len(d) or acnt < len(taken): return
    o, recs = arr + 8, []
    for _ in range(acnt):
        if o + 0x20 > arr + asz: return
        rs, kind = struct.unpack_from('<II', d, o)
        if rs < 0x20: return
        recs.append((o + 0x10, kind)); o += rs
    if o != arr + asz or not taken <= {a for a, _ in recs}: return
    psz, pcnt = struct.unpack_from('<II', d, o)
    if o + psz > len(d) or not pcnt: return
    q, bases = o + 8, []
    for _ in range(pcnt):
        rs, _nl, _mask, base = struct.unpack_from('<IIII', d, q)
        if rs < 16 or base > acnt or (bases and base < bases[-1]): return
        bases.append(base); q += rs
    if q != o + psz: return
    bases.append(acnt)
    for p in range(pcnt):
        seen = [0, 0]
        for i in range(bases[p], bases[p + 1]):
            a, k = recs[i]
            if k > 1: continue   # compute and the other kinds pair with nothing
            if a in taken: BAHS_VAR[f'{label}#BAHS@{a:#x}'] = f'{label}#BAHS#prog{p}#var{seen[k]}'
            seen[k] += 1

def mshd_shaders(d, label):
    out, c = [], d.find(CTL_MAGIC)
    while c >= 0:
        nctl = struct.unpack_from('<I', d, c + 0x20)[0]
        g = d.find(GFX_MAGIC, c + nctl)
        if nctl < 0x880 or g < 0 or g - c - nctl > 0x400: red(f'{label} MSHD control section @{c:#x}: length {nctl:#x}, no code area header after it')
        ncode = struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, code = d[c:c + nctl], d[g:g + ncode]
        st = sph_stage(code)
        if st != ('vs', 'ps')[len(out) % 2]: red(f'{label} MSHD @{c:#x}: stage {len(out)} is {st}, not alternating vertex / pixel')
        why = check(label, st, ctl, code)
        if why: red(f'{label} MSHD @{c:#x} {st}: {why}')
        out.append((f'{label}#MSHD#var{len(out) // 2}', st, ctl, code))
        c = d.find(CTL_MAGIC, g + ncode)
    if len(out) % 2: red(f'{label} MSHD: {len(out)} stages, not in pairs')
    return out

def glslc_stage(ctl, code):
    """A GLSLC output's stage, from its code area's header (graphics: the SPH's stage bits), or compute when the code has no header and the control's thread group (+0x748) is filled"""
    headerless_cs = code[:4] not in (CS_MAGIC, GFX_MAGIC) and len(ctl) >= 0x754 and all(struct.unpack_from('<3I', ctl, 0x748))
    return 'cs' if code[:4] == CS_MAGIC or headerless_cs else sph_stage(code) if code[:4] == GFX_MAGIC and len(code) >= 0x34 else None

def glslc_section(d, h, size):
    """The GPU code sections of the GLSLC output at h, from its section headers: output +0x4c = section count, section i's header at +0x90 + 0x90 i = {u32 size, u32 data offset (from the
    output), u32 type (0 = GPU code)}; a GPU code header adds +0x30 control offset, +0x34 code offset (both from the section's data), +0x38 code length, +0x3c control length.
    → [(control, code)] of each GPU code section, or None"""
    n, secs = struct.unpack_from('<I', d, h + 0x4c)[0], []
    if not 0 < n <= 16 or h + 0x90 * (n + 1) > len(d): return None
    for i in range(n):
        s = h + 0x90 + 0x90 * i
        ssz, soff, typ = struct.unpack_from('<3I', d, s)
        if typ: continue
        co, ko, kn, cn = struct.unpack_from('<4I', d, s + 0x30)
        if soff + ssz > size or co + cn > ssz or ko + kn > ssz: return None
        secs.append((d[h + soff + co:h + soff + co + cn], d[h + soff + ko:h + soff + ko + kn]))
    return secs

def glslc_program(d, h, size, label):
    """A GLSLC output of several stages, compiled together as one program (Baba Is You: each of the 120 outputs in Assets.dat holds a pixel and a vertex shader, then a reflection section): every
    GPU code section by the section headers. Its stages pair with each other (var_pairs), not as single-stage programs (separable_pairs). [] when a section fails the triple check or two share a stage"""
    secs = glslc_section(d, h, size) or []
    sts = [glslc_stage(ctl, code) for ctl, code in secs]
    if len(secs) < 2 or len(set(sts)) < len(sts) or any(not st or check(label, st, ctl, code) for st, (ctl, code) in zip(sts, secs)): return []
    return [(f'{label}#GLSLC@{h:#x}#var', st, ctl, code) for st, (ctl, code) in zip(sts, secs)]

def glslc_shaders(d, label, need=True, skip=()):
    """GLSLC compiler outputs (838 in 13 Sentinels' main read-only segment, one stage each): from the magic 0x19866891, +0x44 = the whole output's length; an output of one stage holds exactly one control section with the code area right after it (control +0x20 = control length),
    one of several stages is read by its section headers (glslc_program).
    The magic is only 4 bytes, so only outputs that pass the triple check are kept. Every game's main is searched: Unity's player has 11 of its own (Fire Emblem Engage, Ace Attorney Investigations
    Collection) beside the game's shaders in its bundles, KTGL and Vanillaware's engine compile theirs into main (13 Sentinels: 838), the others have none.
    need: stop when there is none (a capture file); a Koei Tecmo G2S container (idrk_shaders) or a main may hold none.
    SDK 6's GLSLC (output version 0x1d.2, Fire Emblem: Three Houses) writes 0x848- and 0x850-byte control sections (ctl_len's lower range) and compute code with no 0x12345679 header: the instructions
    start at code +0 (the first word is a scheduling word), and the stage then comes from the control's thread group (+0x748), which only compute sections fill. Its few version-0x1d.0 outputs keep
    something else at control +0x20 (odd values such as 0x136b), so a length that is not a multiple of 4 is not a control length"""
    out = []
    for m in re.finditer(b'\x91\x68\x86\x19', d):
        if any(a <= m.start() < b for a, b in skip): continue
        if m.start() + 0x48 > len(d): continue
        h = m.start(); size = struct.unpack_from('<I', d, h + 0x44)[0]
        c = d.find(CTL_MAGIC, h, h + size) if 0x760 < size <= len(d) - h else -1
        if c < 0: continue
        if d.find(CTL_MAGIC, c + 4, h + size) >= 0:
            out += glslc_program(d, h, size, label); continue
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, code = d[c:c + nctl], d[c + nctl:c + nctl + ncode]
        st = glslc_stage(ctl, code) if nctl >= 0x760 and not nctl & 3 and c + nctl + ncode <= h + size else None
        if not st or check(label, st, ctl, code):
            # SDK 4's GLSLC (output version 0x20.4, Dark Souls Remastered: 44 of main's 72 outputs and the pixel shaders in its archives) writes control sections whose +0x20 is
            # 12 bytes short of their length, so the code area is not right after it; the output's own section header says where both are (glslc_section). Only outputs the rule
            # above rejects are read this way: on every output it keeps (1,073 in 13 Sentinels, Three Houses, FINAL FANTASY TACTICS, Engage and Age of Calamity) both give the same key
            sec = glslc_section(d, h, size)
            if not sec or len(sec) != 1: continue
            (ctl, code), = sec; st = glslc_stage(ctl, code)
            if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#GLSLC@{h:#x}', st, ctl, code))
    if not out and need: red(f'{label}: no GLSLC output')
    return out

SNOWDROP_RECORD = rb'[\xc5\xc6]\x00\xcd\xab'
def snowdrop_wrapped(d, label):
    """Shader cache records can themselves be SDF zstd chunks. Peek at the decoded header of each frame, and finish only frames starting with a cache record, not other assets.
    Incremental input keeps false frame magics and a large enclosing slice from causing an unbounded copy of all trailing bytes."""
    zstd = zstd_mod()
    for m in re.finditer(re.escape(ZSTD_MAGIC), d):
        h = pos = m.start(); dec = zstd.ZstdDecompressor(); head = b''
        try:
            while len(head) < 20 and not dec.eof:
                chunk = d[pos:pos + 0x20000] if dec.needs_input else b''
                if dec.needs_input and not chunk: break
                pos += len(chunk); head += dec.decompress(chunk, 20 - len(head))
        except zstd.ZstdError: continue   # a magic inside another compressed asset
        if len(head) != 20 or not re.match(SNOWDROP_RECORD, head): continue
        parts = [head]
        try:
            while not dec.eof:
                chunk = d[pos:pos + 0x20000] if dec.needs_input else b''
                if dec.needs_input and not chunk: red(f'{label} Snowdrop chunk @{h:#x}: truncated frame')
                pos += len(chunk); parts.append(dec.decompress(chunk))
        except zstd.ZstdError as e: red(f'{label} Snowdrop chunk @{h:#x}: {e}')
        yield h, pos - len(dec.unused_data), b''.join(parts)


def snowdrop_shaders(d, label, wrapped=True):
    """Snowdrop's BERG data slices (SDF version 22): compressed shader records have a 20-byte header {0xabcd00c5 / 0xabcd00c6, flags, 0, raw length, stored length}, then zlib / zstd data.
    Kingdom Battle uses c5 with zlib (0x838-byte controls, including headerless compute), sometimes inside another zstd chunk; Sparks of Hope uses c6 with zstd. Their shader section layout is the same.
    The shader object inside keeps each control section immediately before its code area; its own +0x20 and +0x704 give their lengths. Both sections are byte-packed, without alignment.
    Sparks of Hope's base shader slice has 17,931 records with 18,016 stages (85 hold a second stage), and its update slice adds 443 records. Other assets use bare zstd frames or
    uncompressed data without this header. Some cache records are wrapped in zstd chunks; only decoded frames starting with the cache header are shader assets. Shader programs hold one stage; pairs are selected at draw time.
    Scan by the record header and peek at decoded chunk headers, not by slice names: compressed bytes can contain a coincidental frame header, which is not another asset."""
    chunks = list(snowdrop_wrapped(d, label)) if wrapped else []
    out = []; ci = 0
    for m in re.finditer(SNOWDROP_RECORD, d):
        h = m.start()
        while ci < len(chunks) and h >= chunks[ci][1]: ci += 1
        if ci < len(chunks) and h >= chunks[ci][0]: continue   # literal bytes inside a zstd block are not a separately stored record
        if h + 24 > len(d): continue
        zstd = d[h] == 0xc6
        if zstd:
            if d[h + 20:h + 24] != ZSTD_MAGIC: continue
        elif d[h + 20] & 0xf != 8 or int.from_bytes(d[h + 20:h + 22], 'big') % 31: continue
        nraw, nstored = struct.unpack_from('<II', d, h + 12)
        if not nstored or h + 20 + nstored > len(d): red(f'{label} Snowdrop record @{h:#x}: stored bytes out of bounds')
        stored = d[h + 20:h + 20 + nstored]
        raw = zstd_mod().decompress(stored) if zstd else zlib.decompress(stored)
        if len(raw) != nraw: red(f'{label} Snowdrop record @{h:#x}: expected {nraw} raw bytes, got {len(raw)}')
        for cm in re.finditer(re.escape(CTL_MAGIC), raw):
            c = cm.start()
            if c + 0x708 > len(raw): continue
            nc, nk = struct.unpack_from('<I', raw, c + 0x20)[0], struct.unpack_from('<I', raw, c + 0x704)[0]
            if nc < 0x760 or nc & 3 or nk < 0x34 or c + nc + nk > len(raw): continue
            ctl, code = raw[c:c + nc], raw[c + nc:c + nc + nk]
            st = glslc_stage(ctl, code)
            if st and not check(label, st, ctl, code): out.append((f'{label}#SNOWDROP@{h:#x}:{c:#x}', st, ctl, code))
    for h, _, raw in chunks:
        out += snowdrop_shaders(raw, f'{label}#SDFZSTD@{h:#x}', wrapped=False)
    return out

NEEDLE = b'HNSHV002'
def needle_shaders(d, label):
    """Needle shader containers, embedded in main and in GFD's GSC files inside CRI CPKs (Persona 5 Royal). HNSHV002 +8 is the container's whole length, big-endian. Variant and reflection
    metadata varies; each NVN payload has a 0x90-byte little-endian section descriptor immediately before its control section: +0x30 control offset (0x90), +0x34 code offset, +0x38 code
    length, +0x3c control length, all relative to the descriptor. Both regions must fit this container, not merely the enclosing file. The 0x850-byte controls are followed by graphics
    code or headerless compute code. Each payload is a separate program, including the 121 post-effect variants in one main container; vertex/pixel pairs exist only at draw time."""
    out = []
    for m in re.finditer(NEEDLE, d):
        h = m.start()
        if h + 12 > len(d): continue
        size = struct.unpack_from('>I', d, h + 8)[0]
        if size < 0x90 + 0x760 or size > len(d) - h: continue   # a literal magic string in executable code is not a container
        end = h + size; c = d.find(CTL_MAGIC, h + 12, end)
        while c >= 0:
            s = c - 0x90
            if s < h + 12 or c + 0x760 > end: red(f'{label} NEEDLE @{h:#x}: incomplete NVN section at {c:#x}')
            co, ko, kn, cn = struct.unpack_from('<4I', d, s + 0x30)
            if co != 0x90 or cn < 0x760 or cn != struct.unpack_from('<I', d, c + 0x20)[0]:
                red(f'{label} NEEDLE @{h:#x}: invalid control offset or control length at {s:#x}')
            if ko < co + cn or not kn or max(co + cn, ko + kn) > end - s:
                red(f'{label} NEEDLE @{h:#x}: NVN section at {s:#x} is outside its HNSHV002 container')
            ctl, code = d[c:c + cn], d[s + ko:s + ko + kn]
            st = glslc_stage(ctl, code)
            why = check(label, st, ctl, code) if st else 'unknown shader stage'
            if why: red(f'{label} NEEDLE @{h:#x}: NVN section at {s:#x}: {why}')
            out.append((f'{label}#NEEDLE@{s:#x}', st, ctl, code))
            c = d.find(CTL_MAGIC, s + ko + kn, end)
    return out

IDRK, PDRK = b'IDRK0000', b'PDRK0000'
def idrk_entries(d):
    """Koei Tecmo RDB data files (Hyrule Warriors: Age of Calamity: asset/*.rdb.bin*, asset/data/*.file) are a run of IDRK entries: 'IDRK0000', u64 entry length, u64 stored length, u64 length,
    u32 0, u32 file KTID, u32 type KTID, u32 flags, a few parameter words, and the stored data at the end of the entry. Entries follow each other directly, some after padding (to 64 KB).
    ROMANCE OF THE THREE KINGDOMS 8 REMAKE's RDB (0002/root.rdb and system.rdb, indexed by .rdx) keeps the entries in .fdata files named by KTID, each starting with 'PDRK0000' and u64 0x10
    (its length), and in data/*.file as before; its entries have u32 1 or 0 where Age of Calamity's have 0, and pad to 16 bytes.
    → (start of the stored data, its end, flags) for each entry"""
    o = 0
    while o + 0x30 <= len(d):
        if d[o:o + 8] != IDRK:
            o = d.find(IDRK, o)
            if o < 0: return
        esz, csz = struct.unpack_from('<QQ', d, o + 8)
        yield o + esz - csz, o + esz, struct.unpack_from('<I', d, o + 0x2c)[0]
        o += esz

def idrk_data(d, a, b, flags, head=None):
    """An entry's data. Flag 0x100000 alone = zlib in chunks (Age of Calamity): u32 stored length + a zlib stream each, a zero length ends; the chunks inflate to at most 16 KB.
    Flag 0x400000 = chunks of another form (ROMANCE OF THE THREE KINGDOMS 8 REMAKE): u16 stored length, 8 bytes that the reader does not need, the stream; a zero length ends, and each chunk
    inflates to at most 16 KB (0x4000, which LZ4 is given as the most). The streams are LZ4 blocks when 0x100000 is set too, zlib otherwise: of that game's 51,655 entries, 49,620 have flags
    0x500000, 2,026 have 0x400000 and 9 are stored (0); all of them inflate to the length in their header. Anything else is stored. head: only the first bytes"""
    import zlib
    new, lz4b = flags & 0x400000, flags & 0x500000 == 0x500000
    if not flags & 0x500000: return d[a:min(b, a + head) if head else b]
    out = []
    while a + 4 <= b:
        if new: cs = struct.unpack_from('<H', d, a)[0]; a += 10
        else: cs = struct.unpack_from('<I', d, a)[0]; a += 4
        if not cs: break
        if lz4b:
            import lz4.block
            r = lz4.block.decompress(d[a:a + cs], uncompressed_size=0x4000)
            if head: return r[:head]
        elif head: return zlib.decompressobj().decompress(d[a:a + min(cs, 1024)], head)
        else: r = zlib.decompress(d[a:a + cs])
        out.append(r); a += cs
    return b''.join(out)

def idrk_shaders(path, label, out):
    """Koei Tecmo's KTGL keeps its shaders in G2S containers ('_S2G3000', type KTID 0x7bcd279f in Age of Calamity: 23,108 entries in three RDB files, 2.3 GB inflated), each a material's variations
    as GLSLC compiler outputs (glslc_shaders). One '_DOK0000' container (type KTID 0x20a6a0bb, the engine's system resources; once in system.rdb.bin and once in the update's .bin1) holds 151 bare
    GLSLC outputs without a G2S wrapper, among them 21 shaders the first battle draws. No other entry type holds any. The RDB files are up to 3.8 GB, so they are mapped rather than read,
    and only entries whose data starts with one of those two magics are inflated"""
    from romfs import mmap
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for a, b, flags in idrk_entries(d):
            if idrk_data(d, a, b, flags, 4) in (b'_S2G', b'_DOK'): out.extend(glslc_shaders(idrk_data(d, a, b, flags), f'{label}#IDRK@{a:#x}', need=False))

G2S, SPKG = b'_S2G3000', b'SPKG'
def kt_gz(d):
    """Koei Tecmo's older chunked zlib (Fire Emblem: Three Houses: the compressed entries of DATA1.bin and the patch*/…/*.gz files): u32 chunk size (0x10000), u32 chunk count, u32 inflated length,
    u32 stored length of each chunk, then the chunks at 0x80 alignment, each a u32 length + one zlib stream, or the plain bytes when the stored length equals the chunk's inflated length (a short last
    chunk, or one that does not compress). → the inflated data, or None when d is not in that form"""
    import zlib
    if len(d) < 16: return None
    cs, n, tot = struct.unpack_from('<III', d)
    if cs != 0x10000 or not n or n != (tot + cs - 1) // cs or 12 + 4 * n > len(d): return None
    sizes = struct.unpack_from(f'<{n}I', d, 12); o = (12 + 4 * n + 0x7f) & ~0x7f; out = []
    for i, s in enumerate(sizes):
        if o + s > len(d): return None
        if s == min(cs, tot - i * cs): out.append(bytes(d[o:o + s]))
        else:
            ln = struct.unpack_from('<I', d, o)[0]
            try: out.append(zlib.decompress(d[o + 4:o + 4 + ln]))
            except zlib.error: return None
        o = (o + s + 0x7f) & ~0x7f
    return b''.join(out)

def kt_shaders(d, label, out):
    """Three Houses' KTGL keeps its shaders in SPKG packages (patch*/nx/shader/*.spkg, and 132 entries of DATA1.bin), each holding G2S containers ('_S2G3000', as in Age of Calamity) of GLSLC outputs,
    and in '_SPK4100' packages without G2S (DATA1.bin entry 549, the post effects: 27 outputs); map and model archives in DATA1.bin carry packages of their own. So every inflated file or entry is
    searched for GLSLC outputs, and its SARC archives (the Nintendo layouts, 2D UI) for BNSH, as everywhere else"""
    z = kt_gz(d)
    if z is not None: d = z
    if b'\x91\x68\x86\x19' in d: out.extend(glslc_shaders(d, label, need=False))
    if d[:4] == b'SARC' or b'BNSH\0\0\0\0' in d: scan_blob(d, label, out)

def linkdata_jobs(path, rel, parts=64):
    """Three Houses' LINKDATA: DATA0.bin, beside DATA1.bin (6.9 GB), indexes it with 32-byte entries {u64 offset, u64 inflated length, u64 stored length, u64 compressed}. The entries are split into
    ranges of about equal stored size, one scan job each. → [(path, rel, (first entry, end entry)), …], or None when there is no index beside the file"""
    idx = os.path.join(os.path.dirname(path), 'DATA0.bin')
    if os.path.basename(path) != 'DATA1.bin' or not os.path.exists(idx): return None
    n = os.path.getsize(idx) // 32; step = os.path.getsize(path) / parts; jobs, lo = [], 0
    with open(idx, 'rb') as f: ents = [struct.unpack('<QQQQ', f.read(32)) for _ in range(n)]
    for i in range(1, n + 1):
        if i == n or ents[i][0] >= (len(jobs) + 1) * step: jobs.append((path, rel, (lo, i))); lo = i
    return jobs

def linkdata_shaders(path, rel, rng, out):
    from romfs import mmap
    with open(os.path.join(os.path.dirname(path), 'DATA0.bin'), 'rb') as f: idx = f.read()
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for i in range(*rng):
            off, n, stored, comp = struct.unpack_from('<QQQQ', idx, i * 32)
            if n: kt_shaders(d[off:off + (stored if comp else n)], f'/{rel}#{i}', out)

KPKA = b'KPKA'
SDF_STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']   # an SDF entry's stage offsets, in BNSH order
def kpka_entries(path):
    """Capcom's RE Engine PAK (Monster Hunter Rise: re_chunk_000.pak plus 16 .patch_NNN.pak, 19.6 GB): 'KPKA', u8 version 4, u8 0, u16 features (0: the table is not encrypted), u32 count, u32 fingerprint,
    then count 0x30-byte entries {u32 hash of the lower-case path, u32 hash of the upper-case path, u64 offset, u64 stored length, u64 length, u64 attributes, u64 checksum}.
    Attribute bits 0-3: 0 stored, 1 raw deflate, 2 zstd. → [(path hash, offset, stored length, length, attributes)]"""
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != KPKA: return None
        ver, feat, n = h[4], struct.unpack_from('<H', h, 6)[0], struct.unpack_from('<I', h, 8)[0]
        if ver != 4 or feat: red(f'{path}: PAK version {ver}, features {feat:#x}: only version 4 with a plain entry table is known')
        t = f.read(0x30 * n)
    return [(lo | hi << 32, off, csz, usz, attr) for lo, hi, off, csz, usz, attr, _ in struct.iter_unpack('<IIqqqqQ', t)]

def kpka_data(d, e, head=None):
    """An entry's data (head: only its first bytes)"""
    import zlib
    _, off, csz, usz, attr = e; raw = d[off:off + csz]
    if attr & 0xf == 0: return raw[:head] if head else raw
    if attr & 0xf == 1: return zlib.decompressobj(-15).decompress(raw, head or 0)
    if attr & 0xf == 2:
        zstd = zstd_mod()
        if not head: return zstd.decompress(raw)
        return zstd.ZstdDecompressor().decompress(raw, max_length=head)
    red(f'PAK entry {e[0]:016x}: compression {attr & 0xf} is not known')

def kpka_jobs(files, parts=64):
    """The game reads its PAKs as one view: a path in a later patch PAK (re_chunk_000.pak.patch_NNN.pak, in name order after re_chunk_000.pak) replaces the same path hash in earlier ones.
    Only the newest copy of each path is scanned, so shaders of replaced files do not enter the tables. → ([(path, rel, entries), …] in about equal stored sizes, the PAK paths taken)"""
    paks = [(p, r) for p, r in files if p.endswith('.pak') and kpka_entries(p) is not None]
    if not paks: return [], set()
    view = {}
    for p, r in sorted(paks, key=lambda x: x[1]):
        for e in kpka_entries(p): view[e[0]] = (p, r, e)
    jobs, cur, size, step = [], collections.defaultdict(list), 0, sum(e[2] for _, _, e in view.values()) / parts
    for p, r, e in sorted(view.values(), key=lambda x: (x[1], x[2][1])):
        cur[(p, r)].append(e); size += e[2]
        if size >= step: jobs += [(p_, r_, tuple(es)) for (p_, r_), es in cur.items()]; cur, size = collections.defaultdict(list), 0
    jobs += [(p_, r_, tuple(es)) for (p_, r_), es in cur.items()]
    return jobs, {p for p, _ in paks}

def sdf_shaders(d, label):
    """RE Engine shader definition ('SDF\\0', 2,685 entries in Monster Hunter Rise's PAKs): u16 version 1, u16 entry count, u64, then 0x108-byte entries from +0x10. Entry +0x10: six u64 offsets
    (vertex, hull, domain, geometry, pixel, compute; 0 = no stage), each to {u32 control length, u32 code length} followed by the control section and the code area. Compute code has no header.
    The stages of one entry form a program, so its vertex / pixel stages are a pair"""
    ver, n = struct.unpack_from('<HH', d, 4); out = []
    if not ver and not n: return out   # an empty definition: 16 bytes, version 0, no entries
    if ver != 1: red(f'{label}: SDF version {ver}')
    for i in range(n):
        offs = struct.unpack_from('<6Q', d, 0x10 + 0x108 * i + 0x10)
        for st, o in zip(SDF_STAGES, offs):
            if not o: continue
            nctl, ncode = struct.unpack_from('<II', d, o); ctl, code = d[o + 8:o + 8 + nctl], d[o + 8 + nctl:o + 8 + nctl + ncode]
            why = check(label, st, ctl, code)
            if not why and st != 'cs' and sph_stage(code) != st: why = 'the SPH stage does not match the entry slot'
            if why: red(f'{label} SDF entry {i} {st}: {why}')
            out.append((f'{label}#SDF#var{i}', st, ctl, code))
    return out

def kpka_shaders(path, rel, ents, out):
    from romfs import mmap
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for e in ents:
            if e[3] >= 0x10 and kpka_data(d, e, 4) == b'SDF\0': out.extend(sdf_shaders(kpka_data(d, e), f'/{rel}#{e[0]:016x}'))

PKZL, PGDAT = b'pkzl', b'DAT\0'
PG_STAGES = {b'VSO\0': 'vs', b'HSO\0': 'hs', b'DSO\0': 'ds', b'GSO\0': 'gs', b'PSO\0': 'ps', b'CSO\0': 'cs'}
def pkz_entries(path):
    """A PlatinumGames PKZ archive (Bayonetta 3: 72 in the RomFS root, 14 GB): 'pkzl', u32 version 0x20000, u64 archive length, u32 file count, u32 entry table offset (0x20), u32 name table length,
    u32 alignment (0x1000); then one 0x20-byte entry per file {u32 name offset, u32 compression name offset (both into the name table right after the entries), u64 length, u64 offset,
    u64 stored length}. ASTRAL CHAIN's version 0x10000 uses the same layout (82 archives, 8,556 ZStandard entries); its last header word is zero instead of the v2 alignment.
    The compression is named per entry: 'OodleKraken' (main+0xcbdf80 maps it to 0xc9 and reads it through Oodle's streaming decoder), 'ZStandard' (0x65) or 'None'.
    → [(name, compression, offset, stored length, length)], or None when path is no PKZ"""
    with open(path, 'rb') as f:
        h = f.read(0x20)
        if h[:4] != PKZL: return None
        ver, _, n, eo, nl = struct.unpack_from('<IQIII', h, 4)
        if ver not in (0x10000, 0x20000): red(f'{path}: PKZ version {ver:#x}: only 0x10000 and 0x20000 are known')
        f.seek(eo); t = f.read(0x20 * n + nl)
    nm = lambda o: t[0x20 * n + o:t.index(b'\0', 0x20 * n + o)].decode()
    return [(nm(a), nm(c), off, csz, usz) for a, c, usz, off, csz in struct.iter_unpack('<IIQQQ', t[:0x20 * n])]

def pkz_jobs(files, parts=64):
    """Every entry of every PKZ in ranges of about equal stored length (all of Bayonetta 3's shaders are in root_000.pkz, but no file name is relied on). → ([(path, rel, entries), …], the PKZ paths)"""
    pk = [(p, r, pkz_entries(p)) for p, r in files if p.endswith('.pkz')]
    pk = [x for x in pk if x[2] is not None]
    jobs, step = [], sum(e[3] for _, _, es in pk for e in es) / parts
    for p, r, es in pk:
        cur, size = [], 0
        for e in es:
            cur.append(e); size += e[3]
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
    return jobs, {p for p, _, _ in pk}

def pkz_shaders(path, rel, ents, out):
    """A range of PKZ entries (pkz_jobs), each decoded and scanned like a loose file: Oodle Kraken through the game's own decoder in the Mac host (oodle)"""
    with open(path, 'rb') as f:
        for name, comp, off, csz, usz in ents:
            f.seek(off); c = f.read(csz)
            if comp == 'None': d = c
            elif comp == 'OodleKraken': d = oodle(c, usz)
            elif comp == 'ZStandard': d = zstd_mod().decompress(c)
            else: red(f'/{rel}/{name}: compression {comp!r} is not known')
            if len(d) != usz: red(f'/{rel}/{name}: {len(d)} bytes decoded, the entry says {usz}')
            scan_blob(d, f'/{rel}/{name}', out)

CPK = b'CPK '
def cpk_jobs(files, parts=64):
    """Every entry of every CRI CPK archive in ranges of about equal length (Bayonetta 2: six in the RomFS root, 9.5 GB, 6,091 files, 12 GB of them CRILAYLA-compressed; its shaders are BNSH
    files in shader.dat, shader2.dat and shadereff.dat, but no file name is relied on). → ([(path, rel, entries), …], the CPK paths)"""
    import cpk
    with_head = []
    for p, r in files:
        if p.lower().endswith('.cpk'):
            with open(p, 'rb') as f: h = f.read(4)
            if h != CPK: continue
            try: with_head.append((p, r, cpk.files(p)))
            except (ValueError, struct.error, KeyError, UnicodeDecodeError) as e:   # a layout cpk.py does not read: scanned as a loose file, as before CPKs were read (13 Sentinels and Catherine: Full Body were not checked)
                print(f'[RED] /{r}: the CPK cannot be read ({e}); scanned as one file', flush=True)
    jobs, step = [], sum(e[3] for _, _, es in with_head for e in es) / parts
    for p, r, es in with_head:
        cur, size = [], 0
        for e in es:
            cur.append(e); size += e[3]
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
    return jobs, {p for p, _, _ in with_head}

def cpk_shaders(path, rel, ents, out):
    """A range of CPK entries (cpk_jobs), each decoded (CRILAYLA or stored) and scanned like a loose file"""
    import cpk
    for e in ents:
        try: d = cpk.read(path, e)
        except (ValueError, IndexError) as x:   # a compression cpk.py does not decode: the entry is left out, and the split goes on
            print(f'[RED] /{rel}/{e[0]}: {x}; not scanned', flush=True); continue
        scan_blob(d, f'/{rel}/{e[0]}', out)

ZIP = b'PK\x03\x04'
def zip_jobs(files, parts=64):
    """Every member of every zip archive in ranges of about equal length (Portal: the whole game in game.zip, 41,416 members, 4.6 GB inflated, Deflate or stored; its GLSLC outputs are the
    221 nxcontent/nvsb/*.nvsb files, but no name is relied on). → ([(path, rel, (ZIP, members)), …], the zip paths). An archive zipfile cannot read is scanned as one file, as before"""
    import zipfile
    zs = []
    for p, r in files:
        with open(p, 'rb') as f: h = f.read(4)
        if h != ZIP: continue
        try:
            with open(p, 'rb') as source, zipfile.ZipFile(source) as z: zs.append((p, r, [(i.filename, i.file_size) for i in z.infolist() if not i.is_dir()]))
        except (zipfile.BadZipFile, OSError, ValueError) as e: print(f'[RED] /{r}: the zip cannot be read ({e}); scanned as one file', flush=True)
    jobs, step = [], sum(n for _, _, es in zs for _, n in es) / parts
    for p, r, es in zs:
        cur, size = [], 0
        for nm, n in es:
            cur.append(nm); size += n
            if size >= step: jobs.append((p, r, (ZIP, tuple(cur)))); cur, size = [], 0
        if cur: jobs.append((p, r, (ZIP, tuple(cur))))
    return jobs, {p for p, _, _ in zs}

def zip_shaders(path, rel, names, out):
    """A range of zip members (zip_jobs), each inflated and scanned like a loose file: scan_blob, then whole GLSLC outputs (Portal's .nvsb files: a 0x60-byte 'NVSB' header, then one
    GLSLC output of a vertex and a pixel shader)"""
    import zipfile
    with open(path, 'rb') as source, zipfile.ZipFile(source) as z:
        for nm in names:
            d = z.read(nm); n = len(out)
            scan_blob(d, f'/{rel}/{nm}', out)
            if len(out) == n and d[:4] != b'MSHD' and b'\x91\x68\x86\x19' in d: out += glslc_shaders(d, f'/{rel}/{nm}', need=False)

def pgdat_members(d, label):
    """A PlatinumGames DAT archive: 'DAT\\0', u32 file count, then the offsets of five tables: file offsets (u32 each), extensions (4 bytes each), names (u32 name length, then one name of that
    length each), file lengths (u32 each), a name hash map. Members may be DAT archives again. → [(name, data)], or None when the tables do not fit the data (another game's file that only starts
    with the same four bytes is then scanned as before)"""
    n, ot, et, nt, st = struct.unpack_from('<5I', d, 4)
    if not n: return []
    if max(ot + 4 * n, st + 4 * n, nt + 4) > len(d): return None
    offs, sizes, w = struct.unpack_from(f'<{n}I', d, ot), struct.unpack_from(f'<{n}I', d, st), struct.unpack_from('<I', d, nt)[0]
    if nt + 4 + w * n > len(d) or any(o + s > len(d) for o, s in zip(offs, sizes)): return None
    names = [d[nt + 4 + w * i:nt + 4 + w * (i + 1)].split(b'\0')[0].decode(errors='replace') for i in range(n)]
    return [(nm, d[o:o + s]) for nm, o, s in zip(names, offs, sizes)]

def pg_shader(d, label):
    """A PlatinumGames shader object (Bayonetta 3: 8,024 in root_000.pkz's shader*.dat and autoshadereff.dat): 'VSO\\0' / 'PSO\\0' / 'CSO\\0' / 'HSO\\0' / 'DSO\\0' (the stage), 4 version bytes
    (bytes 6 and 7: the GPU code version, 1.9 to 1.16, which the engine checks against the driver's, main+0xd0da20), u32 control length, u32 control offset (0x18), u32 code length, u32 code offset; nothing else. One stage each: the engine binds its programs per stage,
    so they are paired as single-stage programs (separable_pairs)"""
    st = PG_STAGES[d[:4]]
    nctl, octl, ncode, ocode = struct.unpack_from('<4I', d, 8)
    if octl + nctl > len(d) or ocode + ncode > len(d): red(f'{label}: shader object sections out of bounds')
    ctl, code = d[octl:octl + nctl], d[ocode:ocode + ncode]
    why = check(label, st, ctl, code)
    if not why and st != 'cs' and sph_stage(code) != st: why = f'the SPH stage {sph_stage(code)} does not match the object\'s magic'
    if why: red(f'{label} ({st}): {why}')
    return [(f'{label}#PGSO', st, ctl, code)]

def pg_sized_shader(d, label):
    """PlatinumGames' binding-table form: u32 prefix length (including itself), bindings, u32 code length, code, u32 control length, control. NieR:Automata's 1,371 objects in five DATs
    all have a 0x88-byte prefix and 0x880 control bytes; keys match the objects passed to NVN at boot. Recognize the section boundaries and NVN magics, without relying on a title or filename."""
    if len(d) < 12: return []
    prefix = struct.unpack_from('<I', d)[0]
    if prefix < 4 or prefix & 3 or prefix + 8 > len(d): return []
    ncode = struct.unpack_from('<I', d, prefix)[0]
    codeoff, ctlsize = prefix + 4, prefix + 4 + ncode
    if ncode < 0x80 or ctlsize + 4 > len(d) or d[codeoff:codeoff + 4] not in (GFX_MAGIC, CS_MAGIC): return []
    nctl = struct.unpack_from('<I', d, ctlsize)[0]
    ctloff = ctlsize + 4
    if nctl < 0x760 or ctloff + nctl != len(d) or d[ctloff:ctloff + 4] != CTL_MAGIC: return []
    ctl, code = d[ctloff:], d[codeoff:ctlsize]
    st = 'cs' if code[:4] == CS_MAGIC else sph_stage(code)
    if st is None: red(f'{label}: binding-table shader has an unknown SPH stage')
    why = check(label, st, ctl, code)
    if why: red(f'{label} ({st}): {why}')
    return [(f'{label}#PGSO', st, ctl, code)]

MTARC, MTSPK = b'ARC\0', b'SPK\0'
def mtarc_members(d, label):
    """Capcom's MT Framework archive (The Great Ace Attorney Chronicles: 447 '.arc' files under nativeNXx64): 'ARC\\0', u16 version 7, u16 entry count, then one 0x90-byte entry per file
    {char path[128] ('\\\\'-separated, no extension), u32 type hash, u32 stored length, u32 length in the low 29 bits (flags above), u32 offset}; the data is a zlib stream, or stored when the
    stored length equals the length. → (path, data) of each entry"""
    import zlib
    ver, n = struct.unpack_from('<HH', d, 4)
    if ver != 7: red(f'{label}: ARC version {ver}: only version 7 is known')
    for i in range(n):
        o = 8 + 0x90 * i
        path = d[o:o + 0x80].split(b'\0')[0].decode('latin-1')
        csz, usz, off = struct.unpack_from('<3I', d, o + 0x84); usz &= 0x1fffffff
        data = d[off:off + csz] if csz == usz else zlib.decompress(d[off:off + csz])
        if len(data) != usz: red(f'{label}#{path}: {len(data):#x} bytes, the entry says {usz:#x}')
        yield path.replace('\\', '/'), data

def mtspk_shaders(d, label):
    """MT Framework's shader package ('SPK\\0'; The Great Ace Attorney Chronicles keeps its only one, sc\\NX_64\\root, in sa/NX_64/root.arc): after the package's tables every shader is a
    code area followed directly by its control section, back to back to the end of the file (230 vertex shaders, then 230 pixel shaders). Each control section names its code length (+0x704),
    so the code area is the bytes right before it, and every block must start where the previous one ended. The tables that say which vertex and pixel shaders the engine combines are not
    read, so the shaders are paired as single-stage programs (separable_pairs)"""
    out, end, c = [], None, d.find(CTL_MAGIC)
    while c >= 0:
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        a = c - ncode
        if a < 0 or d[a:a + 4] != GFX_MAGIC or end is not None and a != end: red(f'{label} SPK control section @{c:#x}: its code area does not end right before it')
        ctl, code = d[c:c + nctl], d[a:c]
        st = sph_stage(code)
        why = check(label, st, ctl, code) if st else 'no stage in the SPH'
        if why: red(f'{label} SPK @{a:#x}: {why}')
        out.append((f'{label}#MTSPK@{a:#x}', st, ctl, code))
        end = c + nctl; c = d.find(CTL_MAGIC, end)
    if end != len(d): red(f'{label} SPK: the last shader ends at {end}, not at the end of the package ({len(d):#x})')
    return out

def mtarc_shaders(d, label, out):
    for path, m in mtarc_members(d, label):
        if m[:4] == MTSPK: out.extend(mtspk_shaders(m, f'{label}#{path}'))

SCNE, ZSTD_MAGIC = b'SCNE', bytes.fromhex('28b52ffd')
LZ4_FRAME_MAGIC = bytes.fromhex('04224d18')   # the frame of HAL's other .cmp container (the ubin/ map data)
def bea_members(d, label):
    """NintendoWare Bezel Engine archive ('.bea', magic 'SCNE', a NintendoWare binary file of version 0x10100; Big Brain Academy: Brain vs. Brain keeps nearly all of RomFS in 603 of them):
    +0x20 u64 entry count, +0x28 u64 offset of the entry pointer array (u64 each), each pointing at a 0x30-byte 'ASST' entry {+0x10 u16 kind, u16 alignment shift, u32 stored length,
    u32 length, +0x20 u64 data offset, +0x28 u64 name offset (u16 length + the path)}. Every entry of the game's 7,665 is one zstd frame (kind 2, and 258 for the one sound project),
    even the two whose stored length equals their length, so the frame magic decides. → (path, data) of each entry"""
    n, arr = struct.unpack_from('<QQ', d, 0x20)
    zstd = None
    for i in range(n):
        o = struct.unpack_from('<Q', d, arr + 8 * i)[0]
        if d[o:o + 4] != b'ASST': red(f'{label}: entry {i} @{o:#x} is not an ASST entry')
        csz, usz, off, nm = struct.unpack_from('<II4xQQ', d, o + 0x14)
        path = d[nm + 2:nm + 2 + struct.unpack_from('<H', d, nm)[0]].decode('utf-8', 'replace')
        data = d[off:off + csz]
        if data[:4] == ZSTD_MAGIC:
            zstd = zstd or zstd_mod(); data = zstd.decompress(data)
        if len(data) != usz: red(f'{label}/{path}: {len(data):#x} bytes, the entry says {usz:#x}')
        yield path, data

TFXO = b'.tfxo file\0'
def tfxo_shaders(d, label):
    """YEBIS (Silicon Studio's post-effect library: bloom, glare, depth of field, SSAO …) keeps its compiled effects in one '.tfxo file' (Pokémon Sword 1.3.2: a 39.5 MB zlib stream in main,
    3,155 shaders, see tfxo_streams): a text header (the file length, the shader count, 'hlsl', 'TexUtil_NVN_NX', the technique list 'tech_…,vs_…,ps_…'), then the shaders, each a control section
    (its own length at +0x20: 0x850 on SDK 7) directly followed by its code area (control +0x704 bytes). The effect sets each stage as a program of its own (separable_pairs)"""
    out = []
    for m in re.finditer(re.escape(CTL_MAGIC), d):
        a = m.start(); n = struct.unpack_from('<I', d, a + 0x20)[0]
        if not 0x760 <= n <= 0x880: continue
        ctl = d[a:a + n]; ncode = struct.unpack_from('<I', ctl, 0x704)[0]; code = d[a + n:a + n + ncode]
        st = 'cs' if code[:4] == CS_MAGIC else sph_stage(code) if code[:4] == GFX_MAGIC else None
        if not st: red(f'{label}: the control section at {a:#x} is not followed by a code area')
        why = check(label, st, ctl, code)
        if why: red(f'{label}: shader at {a:#x}: {why}')
        out.append((f'{label}#TFXO@{a:#x}', st, ctl, code))
    return out

def tfxo_streams(d, label):
    """YEBIS effect files linked into a module as zlib streams (Pokémon Sword: one, at main+0x1c98ed9 in 1.3.2, read through the engine's own inflate): a stream is taken when its first bytes
    inflate to the '.tfxo file' header"""
    import zlib
    out = []
    for m in ZLIB_HEAD.finditer(d):
        try: head = zlib.decompressobj().decompress(d[m.start():m.start() + 4096], len(TFXO))   # a dynamic block's code tables come first
        except zlib.error: continue
        if head != TFXO: continue
        z = zlib.decompressobj(); x = z.decompress(d[m.start():])
        out += tfxo_shaders(x, f'{label}@{m.start():#x}')
    return out

RFRM, SNVN = b'RFRM', b'SNVN'
def rfrm_chunk(d, o):   # a Retro Studios RFRM chunk at o: fourcc, big-endian u64 data length, u32 version, u32 version; its data follows at +0x18
    return d[o:o + 4], o + 0x18, o + 0x18 + struct.unpack_from('>Q', d, o + 4)[0]

def snvn_records(d, a, b, label):
    """The program records of an SNVN chunk's data d[a:b]: each a big-endian u32 code length, u32 control length, a byte (0), u32 stream length, then a zlib stream that inflates to
    exactly the two lengths: every stage's code area back to back, then their control sections in the same order (0x840 bytes each on SDK 4, control +0x704 = its code length).
    The tables before the records differ between files (MaterialArchive.arc's SNVN has one of 1,374 (offset, length) pairs, a material's has none), so the records are found by their
    own lengths: a zlib header whose preceding 13 bytes give a stream that ends at exactly that length and inflates to exactly code + control bytes. → [(offset, [(ctl, code), …])]"""
    import zlib
    out, i = [], a + 12
    while (i := d.find(b'\x78', i + 1, b)) >= 0:
        ncode, nctl, _, nz = struct.unpack_from('>IIBI', d, i - 13)
        if not (nctl and not nctl % 0x40 and nctl < 1 << 20 and ncode < 1 << 24 and 0 < nz <= b - i): continue
        z = zlib.decompressobj()
        try: y = z.decompress(d[i:i + nz])
        except zlib.error: continue
        if not z.eof or z.unused_data or len(y) != ncode + nctl: continue
        stages, k, c = [], 0, ncode
        while c < len(y):
            if y[c:c + 4] != CTL_MAGIC: red(f'{label} SNVN record @{i:#x}: no control section at {c:#x}')
            n, nk = struct.unpack_from('<I', y, c + 0x20)[0], struct.unpack_from('<I', y, c + 0x704)[0]
            stages.append((y[c:c + n], y[k:k + nk])); k += nk; c += n
        if k != ncode: red(f'{label} SNVN record @{i:#x}: the code areas end at {k:#x}, the control sections start at {ncode:#x}')
        out.append((i, stages)); i += nz - 1
    return out

def snvn_shaders(d, label):
    """Retro Studios' engine (Donkey Kong Country: Tropical Freeze): NVN programs in SNVN chunks, beside the D3D bytecode (DXBC) of the same materials in an SDX chunk.
    MaterialArchive.arc is one RFRM 'MTRL' form holding both (3,468 stages in 1,374 programs); the .pak archives hold more in their MTRL resources (rfrm_shaders). Each record is one
    program, so its stages pair with each other (var_pairs)"""
    out, p = [], d.find(SNVN)
    while p >= 0:
        _, a, b = rfrm_chunk(d, p)
        for i, stages in snvn_records(d, a, b, label):
            for ctl, code in stages:
                st = glslc_stage(ctl, code)
                why = check(label, st, ctl, code) if st else 'no stage in the code area'
                if why: red(f'{label} SNVN record @{i:#x}: {why}')
                out.append((f'{label}#SNVN@{i:#x}#var', st, ctl, code))
        p = d.find(SNVN, b)
    return out

def retro_lzss8(s, n):
    """Retro's LZSS on 8-bit units (CLZSSDecompressor::DecompressBuffer8, Tropical Freeze main+0xb4714): a flag byte read from its high bit; a set bit is a back reference
    of two bytes (length = high nibble + 3, distance = the other 12 bits), a clear one a literal byte. → the first n bytes, or None when the stream runs out or points before its start"""
    out, i, flags, left = bytearray(), 0, 0, 0
    try:
        while len(out) < n:
            if not left: flags, i, left = s[i], i + 1, 8
            if flags & 0x80:
                ln, dist = (s[i] >> 4) + 3, (s[i] & 0xf) << 8 | s[i + 1]; i += 2
                if not 0 < dist <= len(out): return None
                for _ in range(ln): out.append(out[-dist])
            else:
                out.append(s[i]); i += 1
            flags, left = flags << 1 & 0xff, left - 1
    except IndexError: return None
    return bytes(out[:n])

def retro_main_forms(d, label):
    """RFRM forms compressed into main with CBufferedCompression: a type byte and three zero bytes, then the data. Tropical Freeze's startup material archive
    (NMaterialStartup::get_startup_archive_compressed_buffer, main+0x1510cf8, 5,870 bytes; ImportStartupArchive inflates it at boot) is type 1, LZSS on 8-bit units (retro_lzss8),
    an RFRM 'MTRL' form like MaterialArchive.arc: four programs, among them the CGraphicsUtilCache quads that draw the title logo. Its SNVN copies of shared code areas carry
    other control sections (+0x7d0 differs) than MaterialArchive.arc's, so they are keys of their own. Found by the stream's first literals 'RFRM'; the form's own length (+4) gives the size"""
    out = []
    for m in re.finditer(b'RFRM', d):
        p = m.start()
        if p < 5 or d[p - 5] != 1 or d[p - 4:p - 1] != b'\0\0\0': continue
        head = retro_lzss8(d[p - 1:], 0x20)
        if not head or head[:4] != RFRM or head[0x14:0x18] != b'MTRL': continue
        x = retro_lzss8(d[p - 1:], 0x20 + struct.unpack_from('>Q', head, 4)[0])
        if x: out += snvn_shaders(x, f'{label}@{p - 5:#x}')
    return out

def rfrm_shaders(d, label):
    """A Retro Studios RFRM file: a 'PACK' archive (the .pak files) or a single form (MaterialArchive.arc, an 'MTRL' form). A pack's 'ADIR' chunk lists its resources: u32 count, then
    36-byte entries {fourcc type, 16-byte GUID, big-endian u64 offset, u64 length}. Only 'MTRL' resources hold shaders (795 in Tropical Freeze's packs): an RFRM form whose data after
    its 0x20-byte header is a zlib stream, holding an RFRM 'MTRL' form with an SNVN chunk"""
    import zlib
    if d[0x14:0x18] != b'PACK': return snvn_shaders(d, label)
    out, a = [], d.find(b'ADIR', 0x20)
    if a < 0: red(f'{label}: RFRM PACK without an ADIR chunk')
    for i in range(struct.unpack_from('>I', d, a + 0x18)[0]):
        e = a + 0x1c + 36 * i
        if d[e:e + 4] != b'MTRL': continue
        off, size = struct.unpack_from('>QQ', d, e + 20)
        r = d[off:off + size]
        if r[:4] != RFRM: red(f'{label}: MTRL resource {d[e + 4:e + 20].hex()} is not an RFRM form')
        out += snvn_shaders(zlib.decompress(r[0x20:]) if r[0x20:0x21] == b'\x78' else r, f'{label}/{d[e + 4:e + 20].hex()}')
    return out

GFPAK = b'GFLXPACK'

def gfpak_members(d, label):
    """Game Freak's GFPAK archive (Pokémon Sword: 7,699 '.gfpak' files under bin/archive, 7.9 GB): 'GFLXPACK', u32 version 0x1000, u32 0, u32 file count, u32 folder count,
    u64 offset of the file table, u64 offset of the file hashes (FNV-1a 64 of each path; no names are stored), u64 offset of the folder table pointers. The file table holds
    0x18 bytes per file {u16 level, u16 compression, u32 length, u32 stored length, u32 0, u64 data offset}: compression 0 stored, 1 zlib, 2 LZ4 (one block), 3 Oodle Kraken (pkNX).
    Every entry of Sword's is LZ4; Pokémon Legends: Arceus's are Oodle Kraken, decoded with the game's own decoder as Trinity's packs are (oodle; profile_guess.py build_sources
    has the NSP script set DM_OODLE_HOST for them). Inside: BNTX textures, BNSH shader archives (the models' and effects'), the models and animations. → (#index, data) of each entry"""
    if d[8:12] != b'\0\x10\0\0': red(f'{label}: GFPAK version {d[8:12].hex()}, not 0x1000')
    n, _, files = struct.unpack_from('<IIQ', d, 0x10)
    for i in range(n):
        ct, usz, csz, off = struct.unpack_from('<2xHII4xQ', d, files + 0x18 * i)
        raw = d[off:off + csz]
        if ct == 2:
            import lz4.block
            data = lz4.block.decompress(raw, uncompressed_size=usz)
        elif ct == 1: data = __import__('zlib').decompress(raw)
        elif ct == 0: data = raw
        elif ct == 3: data = oodle(raw, usz)
        else: red(f'{label}: entry {i} has compression {ct}, which this reader does not decode')
        if len(data) != usz: red(f'{label}#{i}: {len(data):#x} bytes, the entry says {usz:#x}')
        yield f'#{i}', data

NLG_DICT = bytes.fromhex('5824f3a9')
def nlg_dict(d, label):
    """Next Level Games' archive index (Luigi's Mansion 3: '<name>.dict' beside '<name>.data', every RomFS file but the Wwise banks): u32 magic 0xa9f32458, u8 4, u8 1, u8 compressed,
    u8 0, u32 the largest stored chunk, u8 chunk count, u8 record count, u8 name count, u8 0; then one 0x18-byte record per variant {u32 hash, u16 top-level entry count of its
    table, u16, u8 chunk of each of its 16 sections (0 = none)}, one 0x10-byte chunk per piece of data {u32 offset, u32 length, u32 stored length (one zlib stream), u16 alignment
    (log2 in the low 5 bits), u8 file, u8 GPU memory}, and the file names ('.data', '.debug', '.nxpc'). Record v's own entry table is chunk v: 12 bytes per entry {u32 kind (bit 31: a
    list of w1 entries from entry w2; bits 27..30 section; bits 22..26 log2 alignment), u32 length, u32 offset in the section}. The game reads the archive in main+0x1ba00 (Luigi's
    Mansion 3 1.4.0). → (compressed, records [(top, chunks)], chunks [(offset, length, stored, file)], file names)"""
    nchunk, nrec = (d[0xc], d[0xd]) if len(d) >= 0x10 else (0, 0)
    co = 0x10 + 0x18 * nrec
    # The layout above, checked before it is read: Mario Strikers: Battle League's (another revision, PR #282) stopped the split with an IndexError that named no file
    if len(d) < co + 0x10 * nchunk or nrec > nchunk or any(c >= nchunk for i in range(nrec) for c in d[0x18 + 0x18 * i:0x28 + 0x18 * i]):
        red(f'{label}: the index ({len(d):#x} bytes, header {d[4:0x10].hex(" ")}) is not in the layout of Luigi\'s Mansion 3\'s archives, the only one nlg_dict reads')
    comp, nname = d[6], d[0xe]
    recs = [(struct.unpack_from('<H', d, 0x14 + 0x18 * i)[0], list(d[0x18 + 0x18 * i:0x28 + 0x18 * i])) for i in range(nrec)]
    chunks = [struct.unpack_from('<3I2xB', d, co + 0x10 * i) for i in range(nchunk)]
    names = d[co + 0x10 * nchunk:].split(b'\0')[:nname]
    return comp, recs, chunks, [n.decode('latin-1') for n in names]

def nlg_walk(tab, start, count, out):
    """The plain entries of a variant's table in the game's order (lists inline, depth first; main+0x1cbd0)"""
    for i in range(start, start + count):
        w0, w1, w2 = struct.unpack_from('<3I', tab, 12 * i)
        if w0 >> 31: nlg_walk(tab, w2, w1, out)
        else: out.append(i)

def nlg_patch_section(tab, order, s, old, body, cut, end, size):
    """An update's section s, rebuilt as the game does after loading the old one (main+0x1c8d0 and main+0x1cbd0, Luigi's Mansion 3 1.4.0): the entries of section s in table order are packed
    from the start of the section at their alignment, each taking its data from the old section (offsets below its length) or from the update's body (the rest, counted from the old length),
    and the entry's offset becomes the new one. The game does it in place and keeps a copy of old[cut:end] for the data the packing overwrites before it is read; this does the same"""
    buf = bytearray(max(len(old), size)); buf[:len(old)] = old
    keep, cur = bytes(old[cut:end]) if end else b'', 0
    for i in order:
        w0, n, src = struct.unpack_from('<3I', tab, 12 * i)
        if (w0 >> 27) & 0xf != s: continue
        al = 1 << ((w0 >> 22) & 0x1f) if w0 & 0x7c00000 else 1
        cur = (cur + al - 1) & -al
        if src >= len(old): buf[cur:cur + n] = body[src - len(old):src - len(old) + n]
        elif end and cut <= src < end: buf[cur:cur + n] = keep[src - cut:src - cut + n]
        elif src != cur: buf[cur:cur + n] = bytes(buf[src:src + n])
        struct.pack_into('<I', tab, 12 * i + 8, cur); cur += n
    return bytes(buf[:size])

def nlg_shaders(path, label, out):
    """Shaders of a Next Level Games archive (nlg_dict). Luigi's Mansion 3 keeps all 5,149 of the base game (5,317 in update 1.4.0) in init: section 8 of each variant starts with the control
    sections back to back (0x850 bytes each), section 7 with their code areas in the same order (a compute code area without a header), and the engine makes one program per stage. An update
    ships '<name>.patch' beside the unchanged archive: u64 the variants it patches (bit = record), u32 offset, stored and inflated length of its body (one zlib stream: 16 u32 section
    offsets, 16 u16 alignments, then each section's new data), then per patched variant (in record order) 0xd0 bytes {u32 offset, stored length and entry count of its new table (one zlib
    stream), u16 top-level entry count, u16, u32 new length of each of 16 sections, 16 u32 end and 16 u32 start of the old data the packing overwrites before reading it} (main+0x1bb60).
    Every variant whose files are in RomFS is read (the 13 even records; the odd ones point into '.nxpc', which is not shipped), and the shaders found pair by order (checked one by one)"""
    import zlib
    d = open(path, 'rb').read(); stem = path[:-len('.dict')]
    comp, recs, chunks, names = nlg_dict(d, label)
    files = {i: open(stem + nm, 'rb').read() for i, nm in enumerate(names) if os.path.exists(stem + nm)}
    patch = open(stem + '.patch', 'rb').read() if os.path.exists(stem + '.patch') else None
    cache = {}
    def chunk(ci):
        if ci not in cache:
            off, n, stored, f = chunks[ci]
            raw = files[f][off:off + (stored if comp else n)]   # an archive stored as is keeps 0 there (Movies, flow)
            cache[ci] = zlib.decompressobj().decompress(raw) if comp else raw[:n]
            if len(cache[ci]) != n: red(f'{label}: chunk {ci} inflates to {len(cache[ci]):#x} bytes, the index says {n:#x}')
        return cache[ci]
    mask, body = 0, b''
    if patch:
        mask, (boff, bstored, bn) = struct.unpack_from('<Q', patch)[0], struct.unpack_from('<3I', patch, 8)
        body = zlib.decompress(patch[boff:boff + bstored])
        if len(body) != bn: red(f'{label}: the patch body inflates to {len(body):#x} bytes, its header says {bn:#x}')
        bsec = struct.unpack_from('<16I', body)
    for v, (top, secs) in enumerate(recs):
        if any(chunks[ci][3] not in files for ci in [v] + [c for c in secs if c]): continue   # a variant of a file the game does not ship
        if mask >> v & 1:
            r = struct.unpack_from('<4I48I', patch, 0x18 + 0xd0 * bin(mask & ((1 << v) - 1)).count('1'))
            tab = bytearray(zlib.decompress(patch[r[0]:r[0] + r[1]]))
            if len(tab) != 12 * r[2]: red(f'{label}: variant {v} patch table has {len(tab)} bytes for {r[2]} entries')
            order = []; nlg_walk(tab, 0, r[3] & 0xffff, order)
            sec = {}
            def section(s):
                if s not in sec:
                    old = chunk(secs[s]) if secs[s] else b''
                    sec[s] = nlg_patch_section(tab, order, s, old, body[bsec[s]:bsec[s + 1] if s < 15 else len(body)], r[36 + s], r[20 + s], r[4 + s])
                return sec[s]
            live = [s for s in range(16) if r[4 + s]]
        else:
            section = lambda s: chunk(secs[s]); live = [s for s in range(16) if secs[s]]
        for c in [s for s in live if section(s)[:4] == CTL_MAGIC]:
            ctls = section(c); n = struct.unpack_from('<I', ctls, 0x20)[0]
            for k in live:   # the section whose data pairs with every control section in turn: code areas back to back from its start
                if k == c: continue
                code, found, p, i = section(k), [], 0, 0
                while ctls[i * n:i * n + 4] == CTL_MAGIC:
                    ctl = ctls[i * n:(i + 1) * n]; ln = struct.unpack_from('<I', ctl, 0x704)[0]; cd = code[p:p + ln]
                    st = glslc_stage(ctl, cd)
                    if not st or check(label, st, ctl, cd): found = None; break
                    found.append((f'{label}#NLG{v}:{c}@{i * n:#x}', st, ctl, cd)); p += ln; i += 1
                if found: out.extend(found); break
            else: red(f'{label}: variant {v} section {c} holds control sections, but no section of the variant holds their code areas in order')

BHD5, DCX, BND3 = b'BHD5', b'DCX\0', b'BND3'
def bhd5_entries(path):
    """FromSoftware's dvdbnd archive (Dark Souls Remastered: dvdbnd0.bdt and dvdbnd1.bdt, 3.6 GB, starting 'BDF3'), indexed by the .bhd5 file beside it: 'BHD5', 0xff (little-endian), u32 1, u32 file size,
    u32 bucket count, u32 0, u64 bucket table offset; each 16-byte bucket {u32 count, u32, u64 offset} points at count 16-byte entries {u32 path hash, u32 size, u64 offset in the .bdt}.
    The index holds only path hashes, no names. → [(hash, offset, size)], or None when there is no index beside the file"""
    idx = path[:-4] + '.bhd5'
    if not path.endswith('.bdt') or not os.path.exists(idx): return None
    with open(idx, 'rb') as f: d = f.read()
    if d[:5] != BHD5 + b'\xff': red(f'{idx}: not a little-endian BHD5 index')
    nb, tab = struct.unpack_from('<I4xQ', d, 0x10)
    out = []
    for i in range(nb):
        n, _, at = struct.unpack_from('<IIQ', d, tab + 16 * i)
        out += [(h, off, size) for h, size, off in struct.iter_unpack('<IIQ', d[at:at + 16 * n])]
    return out

def bdt_jobs(files, parts=64):
    """The entries of every dvdbnd archive, in ranges of about equal stored size, one scan job each. → ([(path, rel, entries), …], the .bdt paths taken)"""
    jobs, taken = [], set()
    for p, r in files:
        ents = bhd5_entries(p)
        if ents is None: continue
        taken.add(p); ents.sort(key=lambda e: e[1]); step, cur, size = sum(e[2] for e in ents) / parts, [], 0
        for e in ents:
            cur.append(e); size += e[2]
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
    return jobs, taken

def undcx(d, label):
    """FromSoftware's DCX container (big-endian): 'DCX\\0', +0x18 'DCS\\0' u32 length, u32 stored length, +0x24 'DCP\\0' + the method, +0x44 'DCA\\0' u32 8, then the stream. Dark Souls Remastered
    uses only DFLT (a zlib stream) on all 5,184 of its DCX entries"""
    import zlib
    n, csz = struct.unpack_from('>II', d, 0x1c)
    if d[0x28:0x2c] != b'DFLT' or d[0x44:0x48] != b'DCA\0': red(f'{label}: DCX method {d[0x28:0x2c]}: only DFLT is known')
    out = zlib.decompress(d[0x4c:0x4c + csz])
    if len(out) != n: red(f'{label}: DCX holds {len(out):#x} bytes, its header says {n:#x}')
    return out

def bnd3_members(d, label):
    """FromSoftware's BND3 archive: 'BND3', 8 bytes of version, u8 format, u8 big-endian, u8 bit-big-endian, u8 0, i32 count, i32 header end, 8 bytes 0, then count 28-byte entries {u8 flags, 3 bytes 0,
    i32 length, u64 offset, i32 ID, u32 name offset, i32 unpacked length} (the formats Dark Souls Remastered uses, 0x5c and 0x7c: 64-bit offsets, IDs, names and the unpacked length). Names are
    Shift JIS paths ('N:\\FRPG\\Source\\Shader\\…') or base names. → (name, data) of each entry"""
    if d[12] not in (0x5c, 0x7c) or d[13]: red(f'{label}: BND3 format {d[12]:#x}, big-endian {d[13]}: only formats 0x5c and 0x7c, little-endian, are known')
    for i in range(struct.unpack_from('<i', d, 0x10)[0]):
        _, n, off, _, nm, usz = struct.unpack_from('<B3xiQiIi', d, 0x20 + 28 * i)
        if n != usz: red(f'{label}: BND3 entry {i} is compressed ({n:#x} of {usz:#x} bytes)')
        name = d[nm:d.index(b'\0', nm)].decode('shift-jis', 'replace').replace('\\', '/')
        yield name.split('/Source/', 1)[-1], d[off:off + n]

def bdt_shaders(path, rel, ents, out):
    """Dark Souls Remastered's shaders: GLSLC outputs, one per BND3 entry ('.vpo' vertex, '.fpo' pixel; 3,225 in six DCX-compressed BND3 archives in dvdbnd0.bdt, 3,088 of them the
    FLVER material shaders). The engine combines vertex and pixel shaders per draw, so they are paired as single-stage programs (separable_pairs)"""
    from romfs import mmap
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for h, off, n in ents:
            label = f'/{rel}#{h:08x}'; d = m[off:off + n]
            if d[:4] == DCX: d = undcx(d, label)
            if d[:4] != BND3 or b'\x91\x68\x86\x19' not in d: continue   # the game's other BND3 archives use other formats (0x78) and hold no shaders
            for nm, b in bnd3_members(d, label):
                if b[:4] == b'\x91\x68\x86\x19': out.extend(glslc_shaders(b, f'{label}/{nm}'))

class LzxError(Exception): pass

# MonoGame's compressed XNB (Stardew Valley: 3,535 of its 3,549 .xnb files): ContentManager.cs knows
# ContentCompressedLzx = 0x80 and ContentCompressedLz4 = 0x40 (XNA's LZX flag was 0x40, MonoGame's own writer
# uses these). The LZX is as XMemCompress writes it: a CHMLib-style LZX decoder (jedwing/CHMLib src/lzx.c, from
# cabextract) with a 2^16 window, inside XNB block framing (MonoGame's LzxDecoderStream): each block is two
# bytes, 0xff then the u16be output and input sizes, or the u16be input size with an 0x8000 output. The LZ4 is
# one raw LZ4 block (its Lz4DecoderStream)
LZX_EXTRA = (0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
             15, 15, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17)
LZX_BASE = (0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072,
            4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144, 393216,
            524288, 655360, 786432, 917504, 1048576, 1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152)

def _lzx_table(nsyms, nbits, length):
    """make_decode_table: a huffman lookup table out of canonical code lengths (David Tritscher's, as in cabextract)"""
    table = [0] * ((1 << nbits) + nsyms * 2)
    pos, bit_num, table_mask = 0, 1, 1 << nbits
    bit_mask = table_mask >> 1
    next_symbol = bit_mask
    while bit_num <= nbits:
        for sym in range(nsyms):
            if length[sym] == bit_num:
                leaf = pos
                pos += bit_mask
                if pos > table_mask: raise LzxError('decode table overrun')
                for i in range(leaf, pos): table[i] = sym
        bit_mask >>= 1; bit_num += 1
    if pos != table_mask:
        for sym in range(pos, table_mask): table[sym] = 0
        pos <<= 16; table_mask <<= 16; bit_mask = 1 << 15
        while bit_num <= 16:
            for sym in range(nsyms):
                if length[sym] == bit_num:
                    leaf = pos >> 16
                    for fill in range(bit_num - nbits):
                        if table[leaf] == 0:
                            table[next_symbol << 1] = 0
                            table[(next_symbol << 1) + 1] = 0
                            table[leaf] = next_symbol; next_symbol += 1
                        leaf = table[leaf] << 1
                        if (pos >> (15 - fill)) & 1: leaf += 1
                    table[leaf] = sym
                    pos += bit_mask
                    if pos > table_mask: raise LzxError('decode table overflow')
            bit_mask >>= 1; bit_num += 1
    if pos != table_mask and any(length[:nsyms]): raise LzxError('a bad or incomplete huffman table')
    return table

class LzxState:
    """The decoder state, which lives for the whole XNB: the ring window, the LRU offsets and the block in progress"""
    def __init__(self, window_bits=16):
        self.window_size = 1 << window_bits
        self.window = bytearray(self.window_size)
        self.posn = 0
        self.R0 = self.R1 = self.R2 = 1
        self.main_elements = 256 + ((window_bits << 1) << 3)   # position slots: window<<1
        self.header_read = False
        self.block_type = 0
        self.block_remaining = 0
        self.intel_filesize = 0
        self.intel_curpos = 0
        self.intel_started = False
        self.frames_read = 0
        self.main_len = bytearray(256 + 50 * 8 + 64)   # + LZX_LENTABLE_SAFETY: deltas are applied to them across blocks
        self.length_len = bytearray(249 + 1 + 64)
        self.main_tbl = self.length_tbl = self.al_tbl = None   # an LZX block can span XNB frames, so the tables persist with the state
        self.al_len = None
        self.d = b''
        self.ip = 0
        self.bb = 0
        self.bl = 0

    def _ensure(self, n):
        while self.bl < n:
            self.bb = (self.bb << 16) | (self.d[self.ip] | (self.d[self.ip + 1] << 8) if self.ip + 1 < len(self.d) else 0)   # a 16-bit overread past the block end is allowed, as in the C code
            self.bl += 16; self.ip += 2

    def _remove(self, n): self.bl -= n; self.bb &= (1 << self.bl) - 1

    def _read(self, n):
        self._ensure(n)
        v = self.bb >> (self.bl - n)
        self._remove(n)
        return v

    def _huffsym(self, table, nbits, maxsyms, lens):
        self._ensure(16)
        i = table[self.bb >> (self.bl - nbits)]
        if i >= maxsyms:
            k = 0
            while True:
                if self.bl - nbits - 1 - k < 0: raise LzxError('a huffman code longer than the bitstream')
                i = (i << 1) | ((self.bb >> (self.bl - nbits - 1 - k)) & 1)
                k += 1
                i = table[i]
                if i < maxsyms: break
        self._remove(lens[i])
        return i

    def _read_lens(self, lens, first, last):
        pre_len = [self._read(4) for _ in range(20)]
        pre = _lzx_table(20, 6, pre_len)
        x = first
        while x < last:
            z = self._huffsym(pre, 6, 20, pre_len)
            if z == 17:
                y = self._read(4) + 4
                while y: lens[x] = 0; x += 1; y -= 1
            elif z == 18:
                y = self._read(5) + 20
                while y: lens[x] = 0; x += 1; y -= 1
            elif z == 19:
                y = self._read(1) + 4
                z = self._huffsym(pre, 6, 20, pre_len)
                z = lens[x] - z
                if z < 0: z += 17
                while y: lens[x] = z; x += 1; y -= 1
            else:
                z = lens[x] - z
                if z < 0: z += 17
                lens[x] = z; x += 1

    def decompress(self, src, outlen):
        self.d, self.ip, self.bb, self.bl = src, 0, 0, 0
        togo = outlen
        if not self.header_read:
            if self._read(1):
                self.intel_filesize = (self._read(16) << 16) | self._read(16)
            self.header_read = True
        while togo > 0:
            if self.block_remaining == 0:
                if self.block_type == 3:   # after an uncompressed block the bitstream returns to a byte boundary
                    if self.block_length & 1: self.ip += 1
                    self.bb = 0; self.bl = 0
                self.block_type = self._read(3)
                self.block_remaining = self.block_length = (self._read(16) << 8) | self._read(8)
                al_table = pre = None
                if self.block_type == 2:
                    self.al_len = [self._read(3) for _ in range(8)]
                    self.al_tbl = _lzx_table(8, 7, self.al_len)
                if self.block_type in (1, 2):
                    self._read_lens(self.main_len, 0, 256)   # the lengths are deltas against the previous block's, so they persist
                    self._read_lens(self.main_len, 256, self.main_elements)
                    self.main_tbl = _lzx_table(256 + 50 * 8, 12, self.main_len)
                    if self.main_len[0xE8]: self.intel_started = True
                    self._read_lens(self.length_len, 0, 249)
                    self.length_tbl = _lzx_table(249 + 1, 12, self.length_len)
                elif self.block_type == 3:
                    self.intel_started = True
                    self._ensure(16)
                    if self.bl > 16: self.ip -= 2   # align the bitstream, as the C code does (the bits stay in the buffer)
                    self.R0 = int.from_bytes(self.d[self.ip:self.ip + 4], 'little'); self.ip += 4
                    self.R1 = int.from_bytes(self.d[self.ip:self.ip + 4], 'little'); self.ip += 4
                    self.R2 = int.from_bytes(self.d[self.ip:self.ip + 4], 'little'); self.ip += 4
                else: raise LzxError(f'block type {self.block_type}')
            if self.ip > len(self.d) and (self.ip > len(self.d) + 2 or self.bl < 16): raise LzxError('the block ended early')
            this_run = min(self.block_remaining, togo)
            togo -= this_run
            self.block_remaining -= this_run
            self.posn &= self.window_size - 1
            if self.posn + this_run > self.window_size: raise LzxError('a run straddles the window')
            posn = self.posn
            if self.block_type == 3:
                if self.ip + this_run > len(self.d): raise LzxError('the uncompressed block runs past the block')
                self.window[posn:posn + this_run] = self.d[self.ip:self.ip + this_run]
                self.ip += this_run; posn += this_run
            else:
                main, length, al_table, al_len = self.main_tbl, self.length_tbl, self.al_tbl, self.al_len
                while this_run > 0:
                    main_element = self._huffsym(main, 12, 256 + 50 * 8, self.main_len)
                    if main_element < 256:
                        self.window[posn] = main_element; posn += 1; this_run -= 1
                        continue
                    main_element -= 256
                    match_length = main_element & 7
                    if match_length == 7:
                        match_length += self._huffsym(length, 12, 249 + 1, self.length_len)
                    match_length += 2
                    match_offset = main_element >> 3
                    if match_offset > 2:
                        if self.block_type == 2:
                            extra = LZX_EXTRA[match_offset]
                            match_offset = LZX_BASE[match_offset] - 2
                            if extra > 3:
                                extra -= 3
                                match_offset += (self._read(extra) << 3) + self._huffsym(al_table, 7, 8, al_len)
                            elif extra == 3:
                                match_offset += self._huffsym(al_table, 7, 8, al_len)
                            elif extra > 0:
                                match_offset += self._read(extra)
                            else:
                                match_offset = 1
                        else:
                            if match_offset != 3:
                                match_offset = LZX_BASE[match_offset] - 2 + self._read(LZX_EXTRA[match_offset])
                            else:
                                match_offset = 1
                        self.R2, self.R1, self.R0 = self.R1, self.R0, match_offset
                    elif match_offset == 0: match_offset = self.R0
                    elif match_offset == 1: match_offset = self.R1; self.R1, self.R0 = self.R0, match_offset
                    else: match_offset = self.R2; self.R2, self.R0 = self.R0, match_offset
                    posn += match_length
                    if posn > self.window_size: raise LzxError('a match overruns the window')
                    this_run -= match_length
                    src = posn - match_length - match_offset
                    if src < 0: src += self.window_size
                    if match_offset >= match_length and src + match_length <= self.window_size:
                        self.window[posn - match_length:posn] = self.window[src:src + match_length]
                    else:
                        w = self.window; dst = posn - match_length
                        for i in range(match_length): w[dst + i] = w[(src + i) % self.window_size]
            self.posn = posn
        start = self.posn - outlen if self.posn else self.window_size - outlen
        out = bytes(self.window[start:self.posn or self.window_size])
        self.frames_read += 1
        if self.frames_read <= 32768 and self.intel_filesize != 0:   # the Intel E8 transform
            if outlen <= 6 or not self.intel_started:
                self.intel_curpos += outlen
            else:
                data = bytearray(out)
                curpos, filesize, pos, end = self.intel_curpos, self.intel_filesize, 0, outlen - 10
                self.intel_curpos = curpos + outlen
                while pos < end:
                    b = data[pos]; pos += 1
                    if b != 0xE8: curpos += 1; continue
                    abs_off = int.from_bytes(data[pos:pos + 4], 'little', signed=True)
                    if -curpos <= abs_off < filesize:
                        rel_off = abs_off - curpos if abs_off >= 0 else abs_off + filesize
                        data[pos:pos + 4] = (rel_off & 0xFFFFFFFF).to_bytes(4, 'little')
                    pos += 4
                    curpos += 5
                out = bytes(data)
        return out

def xnb_decompress(d, first_block=False):
    """The decompressed content of a compressed XNB (flags 0x80 LZX, 0x40 LZ4; the u32 at 6 is the file size,
    the u32 at 10 the decompressed size). first_block stops after the first LZX block: the reader names sit
    at the start of the content, so an effect file is recognized there without decompressing the rest"""
    dec_size = struct.unpack_from('<I', d, 10)[0]
    body = d[14:struct.unpack_from('<I', d, 6)[0]]
    if d[5] & 0x80:
        st = LzxState(16)
        out = bytearray(); p = 0
        while len(out) < dec_size:
            hi = body[p]; p += 1
            if hi == 0xFF:
                dst_size = (body[p] << 8) | body[p + 1]; src_size = (body[p + 2] << 8) | body[p + 3]; p += 4
            else:
                dst_size = 0x8000
                src_size = (hi << 8) | body[p]; p += 1
            if src_size == 0 or dst_size == 0: break
            out += st.decompress(body[p:p + src_size], dst_size)
            p += src_size
            if first_block: break
        return bytes(out)
    if d[5] & 0x40:
        import lz4.block
        return lz4.block.decompress(body, uncompressed_size=dec_size)
    return d[14:]

class _Rd:   # a little-endian reader over bytes, with .NET's BinaryReader strings (7-bit encoded length + UTF-8)
    def __init__(s, d, p): s.d, s.p = d, p
    def u8(s): s.p += 1; return s.d[s.p - 1]
    def un(s, f): v = struct.unpack_from(f, s.d, s.p); s.p += struct.calcsize(f); return v[0] if len(v) == 1 else v
    def raw(s, n): s.p += n; return s.d[s.p - n:s.p]
    def str(s):
        n = sh = 0
        while True:
            b = s.u8(); n |= (b & 0x7f) << sh; sh += 7
            if b < 0x80: return s.raw(n)

MGFX = b'MGFX'
def mgfx_effect(d, p, label):
    """A MonoGame effect at d[p:] (Celeste: MonoGame for the Switch, its C# compiled to C++ by Sickhead's BRUTE; 13 in Content/Effects/*.xnb, whose XNB 'S' files are read by EffectReader,
    and SpriteBatch's in main): 'MGFX', u8 version 8, u8 profile, u32 effect key; the constant buffers {name, i16 size, u8 count, {u8 parameter, u16 offset} each}; the shaders {u8 vertex (1) or
    pixel (0), u32 length, the bytecode, u8 sampler count, {u8 type, u8 texture slot, u8 sampler slot, u8 has state (then 20 bytes of state), name, u8 parameter} each, u8 count + constant buffer
    indices, u8 count + attributes {name, u8 usage, u8 index, i16 location}}; the bytecode is u32 control length, the control section, u32 code length, the code area. Then the parameters and the
    techniques and passes, read to the end so that a misread shows: a pass names its vertex and pixel shader by index, 255 for none. → [(label, stage, ctl, code)], or None when the header is not
    version 8's or its first shader is not an NVN one (four bytes 'MGFX' anywhere in a main)"""
    if d[p + 4:p + 5] != b'\x08': return None
    r = _Rd(d, p + 10)
    try:
        for _ in range(r.u8()): r.str(); r.raw(2); r.raw(3 * r.u8())
        if d[r.p + 10:r.p + 14] != CTL_MAGIC: return None   # shader count, then the first shader's stage, length and control length
    except IndexError: return None
    def no_annotations():
        if r.u8(): red(f'{label} MGFX @{p:#x}: annotations (MonoGame writes none)')
    out = []
    for i in range(r.u8()):
        vs, n = r.u8(), r.un('<I')
        if r.p + n > len(d):   # a shader whose length runs past the data: a stray 'MGFX' in other content, not an effect
            if label.endswith('.xnb'): red(f'{label} MGFX @{p:#x}: shader {i} claims {n:#x} bytes past the end of the file')
            return None
        bc = r.raw(n)
        nctl = struct.unpack_from('<I', bc, 0)[0]; ncode = struct.unpack_from('<I', bc, 4 + nctl)[0] if 8 + nctl <= n else -1
        if vs > 1 or 8 + nctl + ncode != n: red(f'{label} MGFX @{p:#x} shader {i}: {n:#x} bytes, not a control section and a code area')
        ctl, code = bc[4:4 + nctl], bc[8 + nctl:]
        st = sph_stage(code) if code[:4] == GFX_MAGIC else None
        why = check(label, st, ctl, code) if st == ('ps', 'vs')[vs] else f'the SPH stage is {st}, the effect says {("pixel", "vertex")[vs]}'
        if why: red(f'{label} MGFX @{p:#x} shader {i}: {why}')
        out.append((f'{label}#MGFX@{p:#x}#{i}', st, ctl, code))
        for _ in range(r.u8()):
            r.raw(3)
            if r.u8(): r.raw(20)   # address u / v / w, border colour, filter, max anisotropy, max mip level, mip LOD bias
            r.str(); r.u8()
        r.raw(r.u8())
        for _ in range(r.u8()): r.str(); r.raw(4)
    def params():   # {u8 class, u8 type, name, semantic, annotations, u8 rows, u8 columns, elements, struct members, the value when it has neither} each → how many
        n = r.u8()
        for _ in range(n):
            typ = r.raw(2)[1]; r.str(); r.str(); no_annotations()
            rows, cols = r.u8(), r.u8()
            if params() + params(): continue
            if typ in (1, 2, 3): r.raw(4 * rows * cols)   # bool, int32, single
            elif typ == 4: r.str()
        return n
    params()
    for _ in range(r.u8()):   # techniques
        r.str(); no_annotations()
        for _ in range(r.u8()):   # passes: name, annotations, vertex / pixel shader index, then the blend, depth-stencil and rasterizer states when present
            r.str(); no_annotations(); v, f = r.u8(), r.u8()
            if v != 255 and v >= len(out) or f != 255 and f >= len(out): red(f'{label} MGFX @{p:#x}: a pass names shader {v} / {f} of {len(out)}')
            for n in (18, 25, 12):
                if r.u8(): r.raw(n)
    if r.p > len(d) or label.endswith('.xnb') and r.p != len(d): red(f'{label} MGFX @{p:#x}: the effect ends at {r.p:#x}, the file at {len(d):#x}')
    return out

def mgfx_shaders(d, label):
    out = []
    for m in re.finditer(MGFX, d): out += mgfx_effect(d, m.start(), label) or []
    if label.endswith('.xnb') and b'EffectReader' in d[:0x200] and not out: red(f'{label}: an XNB effect without an MGFX version 8 effect of NVN shaders')
    return out

def rel_targets(img):
    """A module image's relative relocations: {slot: target}, both module offsets. RELA's R_AARCH64_RELATIVE entries (type 0x403) carry the target as their addend; DT_RELR (SDK 20, tag 36,
    size tag 35) packs slots whose own 8 bytes are the target: an even entry is a slot (the next slot follows it), an odd one a bitmap of the 63 slots after the last (as loader.c relr_apply)"""
    m = struct.unpack_from('<I', img, 4)[0]; o = m + struct.unpack_from('<i', img, m + 4)[0]; t = {}
    while o + 16 <= len(img):
        tag, v = struct.unpack_from('<qQ', img, o); o += 16
        if tag == 0: break
        t.setdefault(tag, v)
    out = {}
    for i in range(0, t.get(8, 0) if 7 in t else 0, 24):
        off, info, add = struct.unpack_from('<QQq', img, t[7] + i)
        if info & 0xffffffff == 0x403: out[off] = add
    nxt = 0
    for i in range(0, t.get(35, 0) if 36 in t else 0, 8):
        e = struct.unpack_from('<Q', img, t[36] + i)[0]
        if not e & 1: out[e] = struct.unpack_from('<Q', img, e)[0]; nxt = e + 8; continue
        for b in range(63):
            if e >> (b + 1) & 1: s = nxt + 8 * b; out[s] = struct.unpack_from('<Q', img, s)[0]
        nxt += 8 * 63
    return out

def adrp_targets(d, lo, hi):
    """Addresses in [lo, hi) that the code builds with adrp + add on the same register (the add within the next 8 instructions) → the offsets of those adds, all module offsets. Every word of
    the image is tried (a data word that decodes so only adds a candidate)"""
    w, out = memoryview(d)[:len(d) & ~3].cast('I'), collections.defaultdict(list)
    for i, x in enumerate(w):
        if x & 0x9f000000 != 0x90000000: continue
        rd = x & 31; page = (i * 4 & ~0xfff) + ((((x >> 5 & 0x7ffff) << 2 | x >> 29 & 3) << 44 >> 44) << 12)
        if not lo - 0x1000 <= page < hi: continue
        for j, y in enumerate(w[i + 1:i + 9]):
            if y & 0xffc00000 == 0x91000000 and y >> 5 & 31 == rd:
                t = page + (y >> 10 & 0xfff)
                if lo <= t < hi: out[t].append((i + 1 + j) * 4)
                break
    return out

def phyre_shaders(d, label):
    """Phyre's NVN code immediately precedes its control section, unaligned in RYHP effects; loose blobs also prefix their two lengths (FFX/X-2)."""
    out = []
    for m in re.finditer(re.escape(CTL_MAGIC), d):
        c = m.start()
        if c + 0x708 > len(d): continue
        cn = struct.unpack_from('<I', d, c + 0x20)[0]
        kn = struct.unpack_from('<I', d, c + 0x704)[0]
        k = c - kn
        if k < 0 or kn < 0x80 or cn < 0x760 or c + cn > len(d) or d[k:k + 4] != GFX_MAGIC: continue
        ctl, code = d[c:c + cn], d[k:c]
        st = sph_stage(code)
        if st and not check(label, st, ctl, code): out.append((f'{label}#PHYRE@{k:#x}', st, ctl, code))
    return out


def raw_shaders(d, label, known=()):
    """Control sections in main that are in no GLSLC output and no BNSH file: a shader compiled offline whose control section and code area the code hands to nvnShaderData itself (Advance Wars 1+2:
    Re-Boot Camp, Unity 2020.2 with Bink 2: the movie player's 32: 2 vertex, 22 pixel and 8 compute shaders; without them its movies drew white). Most are 0x20-byte records in data {code area
    pointer, control section pointer, u32 code length, u32 control length} (two tables, main+0x49eae50 and +0x49fb5c0), found as two relocated slots, the second pointing at the control section and
    the first at a code area of the length its control +0x704 gives. The rest the code uses by address (main+0x1546b4c, +0x1547efc): adrp + add to the control section and to a code area it copies
    from, which lies right after the control section's own length (control +0x20, at 16-byte alignment) or right before it. Only a code area the code builds the address of is taken: both neighbours
    can look right, so the one whose address the code builds nearest to the control section's (within 1 KB) is taken: main+0x4a2c670 is a compute shader whose code is the 0x100 bytes before it (adds 0x20 apart,
    main+0x1548014 / +0x1548034); after it lies another vertex shader's code area, whose first 0x100 bytes pass the header and length checks, and that area is the vertex shader main+0x4a2d290 takes from
    before itself (main+0x1548358 / +0x1548374, 0x324 bytes from the first). Compute code areas have no header (SDK 10), so the stage is glslc_stage's, and a headerless one is checked by its length only, never
    across a control section. known: the control sections the other readers took from this main (Celeste's MonoGame effect in main holds some outside any GLSLC output or BNSH file), left out.
    Of 25 mains on hand only that one has such control sections, so the tables of every other game stay as they were"""
    skip = []
    for m in re.finditer(b'\x91\x68\x86\x19', d):
        size = struct.unpack_from('<I', d, m.start() + 0x44)[0]
        if 0x760 < size <= len(d) - m.start(): skip.append((m.start(), m.start() + size))
    for m in re.finditer(b'BNSH\0\0\0\0', d): skip.append((m.start(), m.start() + struct.unpack_from('<I', d, m.start() + 0x1c)[0]))
    ctls = [c for m in re.finditer(re.escape(CTL_MAGIC), d) if not (c := m.start()) & 3 and not any(a <= c < b for a, b in skip) and d[c:c + struct.unpack_from('<I', d, c + 0x20)[0]] not in known]
    if not ctls: return []
    rel, code_of, cset = rel_targets(d), {}, set(ctls)
    for s, c in rel.items():
        if c in cset and s - 8 in rel and c not in code_of and struct.unpack_from('<I', d, s + 8)[0] == struct.unpack_from('<I', d, c + 0x704)[0]: code_of[c] = rel[s - 8]
    built = adrp_targets(d, min(ctls) - 0x100000, max(ctls) + 0x100000) if any(c not in code_of for c in ctls) else {}
    out = []
    for c in ctls:
        nctl, n = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, why = d[c:c + nctl], 'no code area: no data record points at one, and the code builds the address of none right after or right before it'
        near = lambda k: min(abs(a - b) for a in built[k] for b in built[c])   # how close the code builds this code area's address to the control section's
        for k in ([code_of[c]] if c in code_of else sorted((k for k in ((c + nctl + 15) & ~15, c - n) if k in built and c in built and near(k) < 0x400), key=near)):
            code = d[k:k + n]; st = glslc_stage(ctl, code) if k >= 0 and CTL_MAGIC not in code else None
            if st and not check(label, st, ctl, code): out.append((f'{label}#RAW@{c:#x}', st, ctl, code)); why = None; break
        if why: print(f'[RED] {label} control section @{c:#x}: {why}; left out', flush=True)
    return out

ONEPACK = b'ONEPACK\0'
def fb_field(d, t, i):
    """FlatBuffers: absolute offset of field i of the table at t, None when the field is absent"""
    vt = t - struct.unpack_from('<i', d, t)[0]
    if 4 + 2 * i >= struct.unpack_from('<H', d, vt)[0]: return None
    off = struct.unpack_from('<H', d, vt + 4 + 2 * i)[0]
    return t + off if off else None

def fb_vec(d, t, i):
    """FlatBuffers: (start, count) of the vector in field i"""
    p = fb_field(d, t, i)
    if p is None: return 0, 0
    p += struct.unpack_from('<I', d, p)[0]
    return p + 4, struct.unpack_from('<I', d, p)[0]

def fb_refs(d, t, i):   # the tables / strings a vector of offsets points at
    st, n = fb_vec(d, t, i)
    return [st + 4 * k + struct.unpack_from('<I', d, st + 4 * k)[0] for k in range(n)]

def fb_root(d, at=0): return at + struct.unpack_from('<I', d, at)[0]

def trpfs_jobs(files, parts=64):
    """Game Freak's Trinity engine (Pokémon Legends: Z-A): almost everything is in arc/data.trpfs, packs of files, with arc/data.trpfd as their index, both FlatBuffers. The descriptor's root table:
    0 file hashes [u64], 1 pack names [string], 2 file infos [{0 pack index u64}] (in file hash order), 3 pack infos [{0 pack size u64, 1 file count u64}]. The file system: 'ONEPACK\\0', u64 offset of its
    root table {0 pack name hashes [u64], 1 pack offsets [u64]}, each pack a table of its own at its offset. A pack is named through its first file's hash (its own name hash is not a plain FNV-1a of the
    descriptor's name). → ([(path, rel, ((pack offset, pack name), …)), …] in about equal sizes, the paths taken)"""
    from romfs import mmap
    jobs, taken = [], set()
    for p, r in files:
        if not p.endswith('.trpfs') or not os.path.exists(p[:-1] + 'd'): continue
        with open(p, 'rb') as f:
            if f.read(8) != ONEPACK: continue
        fd = open(p[:-1] + 'd', 'rb').read(); fr = fb_root(fd)
        hashes = struct.unpack_from(f'<{fb_vec(fd, fr, 0)[1]}Q', fd, fb_vec(fd, fr, 0)[0])
        names = [fd[s + 4:s + 4 + struct.unpack_from('<I', fd, s)[0]].decode() for s in fb_refs(fd, fr, 1)]
        pack_of = {h: struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] if fb_field(fd, t, 0) is not None else 0 for h, t in zip(hashes, fb_refs(fd, fr, 2))}
        sizes = [struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] for t in fb_refs(fd, fr, 3)]
        with open(p, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
            sr = fb_root(d, struct.unpack_from('<Q', d, 8)[0]); st, n = fb_vec(d, sr, 1)
            packs = []
            for off in struct.unpack_from(f'<{n}Q', d, st):
                pr = fb_root(d, off); h0 = fb_vec(d, pr, 0)
                if not h0[1] or struct.unpack_from('<Q', d, h0[0])[0] not in pack_of: red(f'{r}: the pack at {off:#x} has no file the descriptor lists')
                i = pack_of[struct.unpack_from('<Q', d, h0[0])[0]]; packs.append((off, names[i], sizes[i]))
        cur, size, step = [], 0, sum(s for _, _, s in packs) / parts
        for off, name, s in sorted(packs):
            cur.append((off, name)); size += s
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
        taken |= {p, p[:-1] + 'd'}
    return jobs, taken

def smash_arc_jobs(files, parts=128):
    """Super Smash Bros. Ultimate's data.arc (17.6 GB, smash_arc.py): the game's current files, in ranges of about equal stored size. → ([(path, rel, entries), …], the paths taken)"""
    import smash_arc
    jobs, taken = [], set()
    for p, r in files:
        if not p.endswith('.arc'): continue   # only .arc files are opened (MT Framework's ARC archives share the extension, not the magic)
        with open(p, 'rb') as f:
            if f.read(8) != smash_arc.MAGIC: continue
        ents = smash_arc.files(p, regions=True); taken.add(p)   # every region's files: some effects exist only in other regions'
        cur, size, step = [], 0, sum(e[2] for e in ents) / parts
        for e in ents:
            cur.append(e); size += e[2]
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
    return jobs, taken

def smash_arc_shaders(path, rel, ents, out):
    """Each file of a range scanned as a file of its own: GLSLC outputs in the SSBH shader databases ('HBSS' … 'RDHS', the .nushdb files: 41 in 13.0.5), BNSH in the effects ('EFFN') and the UI
    layout archives (SARC). Labels name the file by its format (an SSBH resource by its type, 'SHDR') and its path's hash40: /data.arc/SHDR#<hash40>"""
    from romfs import mmap
    import smash_arc
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for e in ents:
            if not e[3]: continue
            b = smash_arc.read(d, e)
            kind = b[0x13:0x0f:-1] if b[:4] == b'HBSS' else b[:4]
            label = f'/{rel}/{kind.decode("latin1") if kind.isalnum() else "data"}#{e[0]:010x}'
            n = len(out); scan_blob(b, label, out)
            if len(out) == n and b'\x91\x68\x86\x19' in b: out.extend(glslc_shaders(b, label, need=False))

_OODLE = None
def oodle(comp, n):
    """The game's own decoder: OodleLZ_Decompress (Trinity's packs are Oodle Mermaid, Pokémon Legends: Arceus's GFPAK entries Oodle Kraken, and Oodle is linked into main), or LZ5_decompress_safe (DRAGON QUEST XI S's legacy Unreal paks):
    one Mac host per worker process in its tool mode (dm/host/hle_oodle.c, DM_OODLE_SERVE), fed over a pipe. DM_OODLE_HOST = the host, DM_OODLE_MODULES = the rewritten modules it loads
    (the NSP script sets both for a game with Trinity packs, Oodle PKZ or GFPAK entries, or legacy Unreal paks, profile_guess.py shader_sources)"""
    global _OODLE
    if _OODLE is None:
        import tempfile
        host, mods = os.environ.get('DM_OODLE_HOST'), os.environ.get('DM_OODLE_MODULES')
        if not host or not mods: red('Oodle- or LZ5-compressed game files: set DM_OODLE_HOST (the Mac host, make -C dm/host GAME=<id> dmhost) and DM_OODLE_MODULES (the rewritten modules) to decode them with the game\'s own decoder')
        err = tempfile.TemporaryFile()
        _OODLE = (subprocess.Popen([host, mods], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, env=dict(os.environ, DM_OODLE_SERVE='1')), err)
    p, err = _OODLE
    try:
        p.stdin.write(struct.pack('<QQ', len(comp), n)); p.stdin.write(comp); p.stdin.flush()
        got = struct.unpack('<Q', p.stdout.read(8))[0]
    except (BrokenPipeError, struct.error): got = None
    if got != n:
        err.seek(0); red(f'the game\'s decoder in the Mac host failed on a {len(comp)} → {n} byte stream; ---- host output ----\n' + err.read().decode(errors='replace')[-3000:])
    return p.stdout.read(n)

def trinity_shaders(path, rel, packs, out):
    """A range of Trinity packs (trpfs_jobs). Pack table: 0 file hashes [u64], 1 files [{1 compression i8 (3 = Oodle; stored otherwise), 2 level u8, 3 length u64, 4 data [u8]}]; each file is scanned like a loose one"""
    from romfs import mmap
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for off, name in packs:
            pr = fb_root(d, off); hs = fb_vec(d, pr, 0)
            for k, t in enumerate(fb_refs(d, pr, 1)):
                c, ln = fb_field(d, t, 1), fb_field(d, t, 3)
                comp, n = (struct.unpack_from('<b', d, c)[0] if c is not None else 0), (struct.unpack_from('<Q', d, ln)[0] if ln is not None else 0)
                st, sz = fb_vec(d, t, 4)
                if comp == 3: data = oodle(d[st:st + sz], n)
                elif sz == n: data = d[st:st + sz]
                else: red(f'/{rel}#{name}: file {k} has compression {comp} ({sz} → {n} bytes), which is not known')
                scan_blob(data, f'/{rel}#{name}#{struct.unpack_from("<Q", d, hs[0] + 8 * k)[0]:016x}', out)

UE_LEGACY = 'UE legacy pak'
def ue_legacy_jobs(files, parts=64):
    """Unreal paks whose shaders are inline in the cooked packages (uepak.inline_shaders): legacy paks (version 4, DRAGON QUEST XI S: pakchunk0-Switch.pak and pakchunk1-Switch.pak, 13.8 GB,
    and the update's *_0_P.pak) and version 11 paks without shader code archives (OCTOPATH TRAVELER II: Majesty-Switch.pak, 3.3 GB). The engine mounts them all as one view, and a patch pak
    (name ending in _P.pak) wins over the others for the same path. Only the newest copy of each path is scanned. → ([(pak path, rel, (UE_LEGACY, names)), …] in about equal raw sizes, the pak paths taken)"""
    def inline(p):
        try: return uepak.inline_shaders(p)
        except (OSError, ValueError, struct.error): return False
    paks = [(p, r) for p, r in files if p.endswith('.pak') and inline(p)]
    if not paks: return [], set()
    view = {}
    for p, r in sorted(paks, key=lambda x: (x[1].endswith('_P.pak'), x[1])):   # patch paks last, so they replace; one view per pak folder (one per version given)
        pk = uepak.Pak(p)
        for name in pk.files: view[(os.path.dirname(p), os.path.normpath(pk.mount + name))] = (p, r, name, pk.entry(name)['raw'])
    jobs, cur, size, step = [], collections.defaultdict(list), 0, sum(v[3] for v in view.values()) / parts
    for p, r, name, raw in sorted(view.values(), key=lambda x: (x[1], x[2])):
        cur[(p, r)].append(name); size += raw
        if size >= step: jobs += [(p_, r_, (UE_LEGACY, tuple(ns))) for (p_, r_), ns in cur.items()]; cur, size = collections.defaultdict(list), 0
    jobs += [(p_, r_, (UE_LEGACY, tuple(ns))) for (p_, r_), ns in cur.items()]
    return jobs, {p for p, _ in paks}

_PAKS = {}
def ue_legacy_shaders(path, rel, names, out):
    """Files of an Unreal pak with inline shaders (ue_legacy_jobs), each decoded block by block (Zlib and LZ4 in uepak.py; others with the game's own decoder, DRAGON QUEST XI S: LZ5) and scanned
    for the shader records the engine keeps inline"""
    pk = _PAKS.get(path) or _PAKS.setdefault(path, uepak.Pak(path))
    for name in names:
        d = pk.read(name, decode=oodle)
        label = re.sub(r"^(\.\./)+", "", os.path.normpath(pk.mount + name))
        ue_inline_shaders(d, f'/{rel}#{label}', out)

ZLIB_HEAD = re.compile(b'\x78[\x01\x5e\x9c\xda]')
def ue_inline_shaders(d, label, out):
    """Shaders kept inline in cooked Unreal packages (UE 4.18, DRAGON QUEST XI S: each material's shader map in its .uexp, 38 to 87 shaders in the ones looked at, and the engine's global shaders in
    Engine/GlobalShaderCache-GLSL_SWITCH.bin, 945): each shader's code is a byte array (i32 length, then the bytes) holding one zlib stream, which inflates to an LSLG record as in the shader code
    archives (uepak.lslg). Found by the zlib header and the length in front of it: a stream that inflates whole from exactly that many bytes and starts with LSLG (every such stream in the files
    looked at). A record stored without compression (LSLG right after its length) is taken too.
    Unreal Engine 4.27 (OCTOPATH TRAVELER II: no shader code archives, each material's shaders in its .uexp, the global ones in Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin) keeps them as the
    entries of FShaderMapResourceCode: the code (i32 length, then an LZ4 block, the engine's shader compression since 4.26), the raw size (i32) and the frequency (u8). Found by the LZ4 block's
    first literals, which are the record's LSLG: the token (and its literal length bytes) right in front, the length before them, and a block that decodes to exactly the raw size after it.
    The engine combines vertex and pixel shaders per draw (separable_pairs)"""
    import zlib, lz4.block
    def take(s, at):
        rec = uepak.lslg(s)
        if not rec: red(f'{label}: the shader record at {at:#x} is not an LSLG record of the known layout')
        tag, ctl, code = rec; st = UE_TAG.get(tag)
        if not st: red(f'{label}: the shader record at {at:#x} has stage tag {tag}')
        why = check(label, st, ctl, code)
        if why: red(f'{label}: shader at {at:#x} ({st}): {why}')
        out.append((f'{label}#UE{at:x}', st, ctl, code))
    for m in ZLIB_HEAD.finditer(d):
        i = m.start(); n = int.from_bytes(d[i - 4:i], 'little') if i >= 4 else 0
        if not 2 <= n <= len(d) - i: continue
        z = zlib.decompressobj()
        try: s = z.decompress(d[i:i + n])
        except zlib.error: continue
        if z.eof and not z.unused_data and s[:4] == b'LSLG': take(s, i)
    for m in re.finditer(b'LSLG', d):
        i = m.start(); n = int.from_bytes(d[i - 4:i], 'little') if i >= 4 else 0
        if 2 <= n <= len(d) - i and uepak.lslg(d[i:i + n]): take(d[i:i + n], i); continue
        for j in range(i - 1, max(i - 6, 4) - 1, -1):   # an LZ4 block whose first literals start here: token at j, then (literal length 15 and up) its length bytes
            t, ext = d[j] >> 4, d[j + 1:i]
            if (t < 15 and ext) or (t == 15 and not (ext and ext[-1] < 0xff and all(x == 0xff for x in ext[:-1]))): continue
            n = int.from_bytes(d[j - 4:j], 'little')
            if not 8 <= n <= len(d) - j - 5: continue
            raw = int.from_bytes(d[j + n:j + n + 4], 'little')
            try: s = lz4.block.decompress(d[j:j + n], uncompressed_size=raw)
            except Exception: continue
            if len(s) == raw and s[:4] == b'LSLG': take(s, j); break

def arh_entries(d):
    """Monolith ARH1 index → (path, ARD offset, stored size). The XOR trie
    reconstructs full paths; the metadata table supplies the actual file bounds.
    Format: https://github.com/roccodev/ard-tools/blob/master/docs/arh1.md
    Xenoblade 3 2.2.1: 76,445 entries in bf3.arh, 9.05 GB in bf3.ard."""
    if len(d) < 48 or d[:4] != b'arh1': return []
    _, count, so, sn, no, nn, fo, fn, key = struct.unpack_from('<9I', d, 4)
    if nn != count * 8 or any(o > len(d) or n > len(d) - o for o, n in ((so, sn), (no, nn), (fo, fn * 24))): red('ARH1: index table out of bounds')
    key = (key ^ 0xf3f35353).to_bytes(4, 'little')
    # The format encrypts whole u32 words; a final partial word stays plain.
    dec = lambda o, n: bytes(v ^ key[i & 3] for i, v in enumerate(d[o:o + (n & ~3)])) + d[o + (n & ~3):o + n]
    strings = dec(so, sn); nodes = list(struct.iter_unpack('<ii', dec(no, nn)))
    names = {}
    for i, (nxt, prev) in enumerate(nodes):
        if nxt >= 0 or prev < 0: continue
        off = -nxt
        if off >= len(strings) or (end := strings.find(b'\0', off)) < 0 or end + 5 > len(strings): red('ARH1: invalid path suffix')
        fid = struct.unpack_from('<I', strings, end + 1)[0]; prefix = []; cur = i; seen = set()
        while cur:
            if cur in seen or not 0 <= cur < len(nodes): red('ARH1: invalid path trie')
            seen.add(cur); parent = nodes[cur][1]
            if not 0 <= parent < len(nodes): red('ARH1: invalid path parent')
            ch = cur ^ nodes[parent][0]
            if not 0 <= ch < 128: red('ARH1: invalid path character')
            prefix.append(ch); cur = parent
        names[fid] = (bytes(reversed(prefix)) + strings[off:end]).decode('ascii')
    out = []
    for i in range(fn):
        off, size, raw, flags, fid = struct.unpack_from('<Q4I', d, fo + i * 24)
        # HasXbc1Header (bit 1): stored size excludes the 48-byte wrapper.
        if size: out.append((names.get(fid, f'#{fid}'), off, size + (48 if flags & 2 else 0)))
    return out


def arh_jobs(files, parts=64):
    jobs, used = [], set()
    for path, rel in files:
        if not path.endswith('.arh'): continue
        with open(path, 'rb') as f: d = f.read()
        if d[:4] != b'arh1': continue
        ard = path[:-4] + '.ard'
        if not os.path.isfile(ard): red(f'{rel}: ARH1 index has no ARD data file')
        entries = arh_entries(d); length = os.path.getsize(ard)
        if any(o > length or n > length - o for _, o, n in entries): red(f'{rel}: ARD entry out of bounds')
        step = max(1, (len(entries) + parts - 1) // parts)
        jobs += [(ard, rel[:-4] + '.ard', (b'ARH1', entries[i:i + step])) for i in range(0, len(entries), step)]
        used.update((path, ard))
    return jobs, used


def xbc_data(d, off):
    """An XBC1 stream: 48-byte header, stored / zlib / zstd payload. Return
    (decoded bytes, end), or None for bytes that merely resemble a header."""
    if off + 48 > len(d) or d[off:off + 4] != b'xbc1': return None
    kind, raw, size = struct.unpack_from('<3I', d, off + 4)
    if kind not in (0, 1, 3) or not size or off + 48 + size > len(d): return None
    data = d[off + 48:off + 48 + size]
    if kind == 1: data = zlib.decompress(data)
    elif kind == 3: data = zstd_mod().decompress(data)
    if len(data) != raw: red(f'XBC1 @{off:#x}: expected {raw} bytes, decoded {len(data)}')
    return data, off + 48 + size


def monolith_cmp_data(d, off):
    """Firehawk effect archives: cmp\\0zlib, stored/raw sizes, zlib payload.
    These independently compressed shader resources sit inside XBC1 entries."""
    if off + 16 > len(d) or d[off:off + 8] != b'cmp\0zlib': return None
    size, raw = struct.unpack_from('<2I', d, off + 8)
    if not size or size > len(d) - off - 16: return None
    z = zlib.decompressobj(); data = z.decompress(d[off + 16:off + 16 + size]) + z.flush()
    if not z.eof or z.unused_data or len(data) != raw: red(f'cmp zlib @{off:#x}: payload does not match stored/raw sizes')
    return data, off + 16 + size


def hcps_shaders(d, base, label):
    """HCPS shader programs (Monolith's wishp, also embedded in models/map
    streams). SLCT gives the control/code ranges and variant descriptors; each
    NVSD's final stage record gives their sizes. Pair stages within each variant.
    Layout cross-checked against xc3_lib/src/spch.rs and the game's own bytes."""
    def span(off, n):
        if off < base or off > len(d) or n > len(d) - off: red(f'{label} HCPS @{base:#x}: section out of bounds')
        return d[off:off + n]
    span(base, 72)
    table, count = struct.unpack_from('<2I', d, base + 8)
    slct_off, slct_size, code_off, code_size, ctl_off, ctl_size = struct.unpack_from('<6I', d, base + 24)
    span(base + table, count * 8)
    slcts, codes, ctls = (span(base + o, n) for o, n in ((slct_off, slct_size), (code_off, code_size), (ctl_off, ctl_size)))
    if not code_size and not ctl_size: return []   # source-only GLSL programs (DLC model streams), no NVN binaries to extract
    out = []
    for i in range(count):
        s = struct.unpack_from('<I', d, base + table + 8 * i)[0]
        if s + 72 > len(slcts) or slcts[s:s + 4] != b'SLCT': red(f'{label} HCPS: invalid SLCT {i}')
        num, ptr = struct.unpack_from('<2I', slcts, s + 16)
        c, cn, k, kn = struct.unpack_from('<4I', slcts, s + 40)
        ce, ke = c + cn, k + kn
        if s + ptr + num * 8 > len(slcts) or ce > len(ctls) or ke > len(codes): red(f'{label} HCPS SLCT {i}: variant range out of bounds')
        for j in range(num):
            po, pn = struct.unpack_from('<2I', slcts, s + ptr + j * 8); p = s + po
            if pn < 32 or p + pn > len(slcts): red(f'{label} HCPS: invalid NVSD')
            vo, vn = struct.unpack_from('<2I', slcts, p + 24)
            if not vn or vo + vn * 20 > pn: red(f'{label} HCPS: invalid NVSD stage records')
            _, vs, ps, vc, pc = struct.unpack_from('<5I', slcts, p + vo + (vn - 1) * 20)
            pend, kend = c + vc + pc, k + vs + ps
            if pend > ce or kend > ke: red(f'{label} HCPS: program exceeds SLCT range')
            # A vertex bundle can include a geometry stage (shd_lgt.wishp:
            # 768-byte vertex + 256-byte geometry, two 0x880 controls). NVSD's
            # vertex size covers both; each NVN control gives its own code size.
            while c < pend:
                if c + 0x708 > pend: red(f'{label} HCPS: truncated control')
                nc = struct.unpack_from('<I', ctls, c + 0x20)[0]
                nk = struct.unpack_from('<I', ctls, c + 0x704)[0]
                if nc < 0x760 or c + nc > pend or not nk or k + nk > kend: red(f'{label} HCPS: shader exceeds program range')
                ctl, code = ctls[c:c + nc], codes[k:k + nk]; c += (nc + 7) & ~7; k += nk
                st = glslc_stage(ctl, code)
                why = check(label, st, ctl, code) if st else 'unknown stage'
                if why: red(f'{label} HCPS SLCT {i} variant {j}: {why}')
                out.append((f'{label}#HCPS@{base:#x}#var{i}.{j}', st, ctl, code))
            if k != kend: red(f'{label} HCPS: unused program code bytes')
        if c != ce or k != ke: red(f'{label} HCPS SLCT {i}: unused control/code bytes')
    return out


def slct_shaders(d, base, label):
    """A standalone SLCT has its control/code offsets relative to itself.
    Monolith embeds this form in main, without an HCPS header. Present those
    same relative sections to the HCPS reader; no code or control bytes change.
    Its text-bearing control sections are individually aligned to eight bytes."""
    if base + 72 > len(d): return []
    c, cn, k, kn = struct.unpack_from('<4I', d, base + 40)
    size = max(c + cn, k + kn)
    if min(c, k) < 72 or size > len(d) - base: return []
    header = bytearray(80); header[:4] = b'HCPS'
    struct.pack_into('<2I', header, 8, 72, 1)
    struct.pack_into('<6I', header, 24, 80, size, 80, size, 80, size)
    return hcps_shaders(bytes(header) + d[base:base + size], 0, f'{label}#SLCT@{base:#x}')


def monolith_shaders(d, label, out):
    """Scan decoded archive entries and their XBC1 streams without relying on
    extensions: model streams and map data embed HCPS alongside other assets."""
    pos = 0; compressed = []
    # Separate literal scans use Python's fast substring search. Alternation's
    # character-set search is costly across the archive's decoded texture data.
    markers = sorted((m.start(), magic == b'xbc1', len(magic)) for magic in (b'xbc1', b'cmp\0zlib') for m in re.finditer(magic, d))
    for off, xbc, size in markers:
        if off < pos: continue
        result = xbc_data(d, off) if xbc else monolith_cmp_data(d, off)
        pos = off + size if result is None else result[1]
        if result is not None:
            compressed.append((off, pos))
            monolith_shaders(result[0], f'{label}#{"XBC1" if xbc else "cmp-zlib"}@{off:#x}', out)
    ci = 0
    for m in re.finditer(b'HCPS', d):
        base = m.start()
        while ci < len(compressed) and compressed[ci][1] <= base: ci += 1
        if ci < len(compressed) and compressed[ci][0] <= base: continue   # a zstd literal is not a decoded container
        if base + 8 <= len(d) and struct.unpack_from('<I', d, base + 4)[0] in (1000, 1010): out.extend(hcps_shaders(d, base, label))
    # Effect asset packs keep complete GLSLC outputs alongside HCPS programs.
    # As above, compressed literals are visited only after decoding their stream.
    out.extend(glslc_shaders(d, label, need=False, skip=compressed))


def arh_shaders(path, rel, entries, out):
    with open(path, 'rb') as f:
        for name, off, size in entries:
            f.seek(off); data = f.read(size)
            monolith_shaders(data, f'/{rel}{name}', out)


def yaz0(s):
    """Yaz0 decompression (Super Mario 3D World + Bowser's Fury: almost all of RomFS is .szs = SARC wrapped in Yaz0). Pure Python runs at about 20–45 MB/s per core, under a minute for the whole 4.7 GB RomFS in parallel, and needs no extra dependency.
    Group header bits: 1 = one literal byte; 0 = back-reference, 2 bytes {length high nibble, 12-bit distance} (distance + 1); length 0 reads one more byte + 0x12, else + 2. Overlapping copies are expanded by their period"""
    n = int.from_bytes(s[4:8], 'big'); out = bytearray(); si = 16
    while len(out) < n:
        h = s[si]; si += 1
        for _ in range(8):
            if h & 0x80:
                out.append(s[si]); si += 1
            else:
                b1, b2 = s[si], s[si + 1]; si += 2
                dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                if ln: ln += 2
                else: ln = s[si] + 0x12; si += 1
                st = len(out) - dist
                out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
            if len(out) >= n: break
            h <<= 1
    return bytes(out[:n])

def sarc_members(d):
    hl, bom, size, doff = struct.unpack_from('<HHII', d, 4)
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        nm = d[names + (attr & 0xffffff) * 4:].split(b'\0')[0].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'
        yield nm, d[doff + s:doff + e]

NV_STAGES = ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']   # NVN stage order
COMMON, TREES = None, {}   # Unity common strings and borrowed type trees (given when the pool starts)
ZDICTS, _ZD = {}, {}       # zstd dictionaries by ID (given when the pool starts) and their digested form in this process

def _pool_init(common, trees, zdicts={}):
    global COMMON, TREES, ZDICTS
    COMMON, TREES, ZDICTS = common, trees, zdicts

def zs_dicts(romfs):
    """Tears of the Kingdom compresses its RomFS files with zstd dictionaries (the frame header names one, ID 1 for the shader packages). They ship as Pack/ZsDic.pack.zs:
    a SARC (itself plain zstd) of *.zsdic files, each a zstd dictionary (magic 0xEC30A437, then its ID). → {ID: dictionary}; {} without that file (Mario's .zs are plain zstd)"""
    p = os.path.join(romfs, 'Pack/ZsDic.pack.zs')
    if not os.path.exists(p): return {}
    d = zstd_mod().decompress(open(p, 'rb').read())
    return {struct.unpack_from('<I', m, 4)[0]: m for _, m in sarc_members(d) if m[:4] == bytes.fromhex('37a430ec')}

def zs_dict_id(d):
    """The zstd dictionary a frame names (0: none)"""
    fhd = d[4] if len(d) > 5 and d[:4] == ZSTD_MAGIC else 0
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)   # dictionary ID size; it follows the window descriptor, which single-segment frames omit
    return int.from_bytes(d[o:o + n], 'little')

def unzs_dict(d, did, where):
    """zstd frames that name dictionary did, decompressed with it from ZDICTS"""
    zstd = zstd_mod()
    if did not in ZDICTS: red(f'{where}: a frame needs zstd dictionary {did}, which Pack/ZsDic.pack.zs does not have')
    if did not in _ZD: _ZD[did] = zstd.ZstdDict(ZDICTS[did])
    return zstd.decompress(d, zstd_dict=_ZD[did])

def unzs(path):
    """A zstd file decompressed: frames without a dictionary through the zstd command as before (fed the bytes: inside a RomFS image the path is not a file the command can open),
    frames that name one with that dictionary from ZDICTS"""
    d = open(path, 'rb').read()
    did = zs_dict_id(d)
    if not did: return subprocess.run(['zstd', '-d', '-c', '-q'], input=d, capture_output=True, check=True).stdout
    return unzs_dict(d, did, path)

def frame_size(f):
    """The uncompressed size a zstd or LZ4 frame header records, None when it records none"""
    if f[:4] == LZ4_FRAME_MAGIC:   # FLG, BD, then the 8-byte content size when FLG bit 3 is set
        return int.from_bytes(f[6:14], 'little') if len(f) > 14 and f[4] & 8 else None
    fhd = f[4] if len(f) > 5 else 0
    single, n = fhd >> 5 & 1, (fhd >> 5 & 1, 2, 4, 8)[fhd >> 6]   # a single-segment frame always records it (1 byte at least), others only with a size flag
    if not n: return None
    o = 6 - single + (0, 1, 2, 4)[fhd & 3]   # after the window descriptor and the dictionary ID
    return int.from_bytes(f[o:o + n], 'little') + (256 if n == 2 else 0)

def unsized(d, where):
    """A size-prefixed frame (HAL's .cmp containers, Kirby and the Forgotten Land): a 4-byte little-endian uncompressed size, then a zstd frame (with a dictionary as for a zstd file)
    or an LZ4 frame (its ubin/ map data) → the frame decompressed. Bytes whose frame header records another size are not such a container and are returned as they are (Tears of the
    Kingdom's 1,202 Cave/…/C.crbin.<hash>/*.quad files: the word before the frame is that hash). A frame that records none must decompress to exactly the prefix's size; one that does not,
    or does not decompress, is reported and returned as it is. So is a dictionary ZDICTS does not have, rather than stopping the split as for a zstd file: the ID may be any bytes after a magic at +4"""
    n, f = struct.unpack_from('<I', d)[0], d[4:]
    if frame_size(f) not in (None, n): return d
    did = zs_dict_id(f)
    try:
        if f[:4] == LZ4_FRAME_MAGIC:
            import lz4.frame
            dec = lz4.frame.decompress(f)
        elif did and did not in ZDICTS: dec = None
        else: dec = unzs_dict(f, did, where) if did else zstd_mod().decompress(f)
    except Exception as e: why = str(e) or type(e).__name__
    else:
        if dec is None: why = f'the frame needs zstd dictionary {did}, which Pack/ZsDic.pack.zs does not have'
        elif len(dec) == n: return dec
        else: why = f'the size prefix says {n} bytes, the frame holds {len(dec)}'
    print(f'[RED] {where}: a size-prefixed frame that did not open ({why}): the file is read as it is', flush=True)   # a container that stays closed holds its shaders unread
    return d

def unity_program(code, where, stage=None):
    """Unity's Switch program code → [(stage, control, code)]; stage = the subprogram's stage from m_ParsedForm, which only the pre-2019.3 layout needs"""
    if len(code) >= 0x14 and code[0x10:0x14] == CTL_MAGIC:   # before 2019.3: 0x10 header + control section + code area, one stage
        if stage is None: red(f'{where}: pre-2019.3 subprogram whose stage is not in m_ParsedForm')
        nctl, ncode = struct.unpack_from('<2i', code, 8); a = 0x10; end = a + nctl + ncode
        ctl, cd = code[a:a + nctl], code[a + nctl:end]
        why = check(where, stage, ctl, cd) if end <= len(code) else 'block out of bounds'
        if why: red(f'{where} {stage}: {why}')
        if end != len(code): red(f'{where}: program code is {len(code):#x} bytes, but control section + code area end at {end:#x}')
        return [(stage, ctl, cd)]
    h = struct.unpack_from('<24i', code, 0); out = []; end = 0x60
    for i, st in enumerate(NV_STAGES):
        off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
        if off == -1: continue
        a = 0x60 + off; ctl, cd = code[a:a + nctl], code[a + nctl:a + nctl + ncode]
        why = check(where, st, ctl, cd) if a + nctl + ncode <= len(code) else 'block out of bounds'
        if why: red(f'{where} {st}: {why}')
        out.append((st, ctl, cd)); end = max(end, a + nctl + ncode)
    if end != len(code): red(f'{where}: program code is {len(code):#x} bytes, but the stage blocks end at {end:#x}')
    return out

def unity_stages(o):
    """Shader object → {subprogram index: stage}, from the passes of m_ParsedForm (progVertex / progFragment / ... m_SubPrograms[].m_BlobIndex)"""
    m = {}
    for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []):
        for p in sub.get('m_Passes', []):
            for field, st in (('progVertex', 'vs'), ('progFragment', 'ps'), ('progGeometry', 'gs'), ('progHull', 'hs'), ('progDomain', 'ds')):
                for sp in (p.get(field) or {}).get('m_SubPrograms', []): m.setdefault(sp.get('m_BlobIndex'), st)
    return m

def unity_code_blobs(o):
    """Shader object → the subprogram indices that hold program code, or None when every entry does (before Unity 2022). Unity 2022 also keeps the parameters of each program in the blob
    as entries of their own (m_ParameterBlobIndices of each program), and the player's programs are m_PlayerSubPrograms[tier][].m_BlobIndex (Ace Attorney Investigations Collection, Unity 2022.3).
    Both fields came together: UnityPy's type trees for 2020.3.18 (Engage) and 2021.3 have neither, so older games read every entry as before"""
    progs = [p.get(f) or {} for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []) for p in sub.get('m_Passes', [])
             for f in ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain')]
    if not any('m_ParameterBlobIndices' in p for p in progs): return None
    return {sp['m_BlobIndex'] for p in progs for tier in p.get('m_PlayerSubPrograms', []) for sp in tier}

def unity_compute(x, base, trees):
    """ComputeShader objects (class 72) of a serialized file → [(where, 'cs', control, code)]: each kernel variant's code is a program code with one stage, compute (Monster Train 2,
    Unity 2022.3: the post-processing stack's 12 compute shaders, its colour grading LUT baker among them, in Data/sharedassets2.assets). Raises NoTree in the first pass, so the file is scanned
    again with the borrowed trees and unityfs.py's layouts; in the second pass a class still without a tree is reported and its kernels left out, as before compute shaders were read.
    Unity 2022.3 lists a kernel's variants in uniqueVariants; Unity 2020.3 keys them by keyword set in variantMap (AI: THE SOMNIUM FILES - nirvanA Initiative's bundles, whose type trees say so).
    Unity 2019.4 has no variant wrapper: each named kernel holds its code directly (Pokemon Brilliant Diamond's GPU skinning)."""
    import unityfs
    out = []
    try:
        for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_COMPUTE_SHADER}, trees):
            for v in o['variants']:
                for k in v['kernels']:
                    variants = k['uniqueVariants'] if 'uniqueVariants' in k else [p['second'] for p in k['variantMap']] if 'variantMap' in k else [k]
                    for i, u in enumerate(variants):
                        where = f'{base}#{o["m_Name"]}#{k["name"]}.{i}'
                        out.extend((where, st, ctl, cd) for st, ctl, cd in unity_program(u['code'], where, 'cs'))
    except unityfs.NoTree as e:
        if not TREES: raise
        print(f'[RED] {base}: ComputeShader objects that no bundle lends a type tree to and unityfs.py has no layout for, their kernels are left out ({e})', flush=True)
        return []
    # The NSP script may derive shader-only tables when a new reader adds compute kernels to a registered build.
    # Emit the actual reader's key/source evidence; guessing from asset names could admit unrelated table changes.
    for where, _, ctl, code in out:
        print('unity compute source: ' + json.dumps([skey(ctl, code), where]), flush=True)
    return out

def unity_pass_pairs(o, programs):
    """Legacy one-stage programs → vertex / pixel-input-cover pairs within each parsed pass and platform.
    Unity selects the stages separately; their blob indices differ even with the same keywords (Obra Dinn's OneBit/Rope: vertex 0, pixel 8).
    Cover every pixel input of the pass, including variants with stage-specific keywords and stripped-variant fallbacks, without assuming equal keyword lists
    or forming the full Cartesian product. These supplemental pairs feed output padding through pso_pairs.tsv, leaving existing evidence-pair tables unchanged."""
    pairs = set()
    for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []):
        for p in sub.get('m_Passes', []):
            for plat in {plat for plat, _ in programs}:
                stages = {}
                for field, stage in (('progVertex', 'vs'), ('progFragment', 'ps')):
                    stages[stage] = {}
                    for sp in (p.get(field) or {}).get('m_SubPrograms', []):
                        program = programs.get((plat, sp.get('m_BlobIndex')))
                        if program and program[0] == stage:
                            _, ctl, code = program
                            stages[stage][skey(ctl, code)] = code
                cover = pixel_input_cover(stages['ps'])
                pairs.update((v, f) for v in stages['vs'] for f in cover)
    return pairs

def unity_shaders(d, label, out, trees):
    """Shaders and compute shaders in a UnityFS bundle or serialized file → out and legacy pass pairs; raises NoTree when no type tree is available"""
    import unityfs
    if COMMON is None: red(f'{label}: a Unity asset, but main has no Unity common string table')
    pairs = set()
    sers = unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [('', d)]
    for node, x in sers:
        if not unityfs.is_serialized(x): continue
        for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_SHADER}, trees):
            name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
            stages = unity_stages(o)
            legacy = {}
            for plat, k, sp in unityfs.shader_programs(o, unity_code_blobs(o)):
                where = f'{label}{":" + node if node else ""}#{name}#var{plat}.{k}'
                code = unityfs.subprogram(sp)[4]
                programs = unity_program(code, where, stages.get(k))
                out.extend((where, st, ctl, cd) for st, ctl, cd in programs)
                if len(code) >= 0x14 and code[0x10:0x14] == CTL_MAGIC: legacy[plat, k] = programs[0]
            pairs |= unity_pass_pairs(o, legacy)
        out.extend(unity_compute(x, f'{label}{":" + node if node else ""}', trees))
        out.extend(unity_text_glslc(x, f'{label}{":" + node if node else ""}'))
    return pairs

def unity_text_glslc(x, base):
    """Whole GLSLC outputs that a game keeps in a TextAsset of a serialized file → [(where, stage, control, code)]. FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition (Unity 2018.4.2f1)
    draws its maps and effects with programs of its own, outside Unity's Shader objects: the TextAsset shader.switch.vfxsb in Data/resources.assets (11 MB, header word 0x20160817) holds
    1,160 GLSLC outputs of a vertex and a pixel stage each (1,207 distinct keys), which the game hands to NVN itself; without them its ground tiles and effects were not drawn.
    The bytes are only decoded when the file holds the GLSLC magic, and glslc_shaders keeps only outputs that pass its checks, so files without such TextAssets give what they gave before"""
    import unityfs
    if b'\x91\x68\x86\x19' not in x: return []
    out = []
    for pid, name, b in unityfs.text_assets(x, COMMON):
        if b'\x91\x68\x86\x19' in b: out += glslc_shaders(b, f'{base}#{name}', need=False)
    return out

def scan_blob(d, label, out):
    if d[:4] in (b'HCPS', b'xbc1', b'DRSM') or d[:8] == b'cmp\0zlib':
        monolith_shaders(d, label, out)
        return
    for m in re.finditer(b'SLCT\x03\x10\0\0', d): out.extend(slct_shaders(d, m.start(), label))
    if d[:4] == b'Yaz0': d = yaz0(d)
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d): scan_blob(m, f'{label}/{nm}', out)
        return
    if d[:4] == b'BAHS': out.extend(bahs_shaders(d, label)); return
    if d[:len(TFXO)] == TFXO: out.extend(tfxo_shaders(d, label)); return
    if d[:4] == b'MSHD': out.extend(mshd_shaders(d, label)); return
    if d[:4] == PGDAT and len(d) >= 0x1c and (mem := pgdat_members(d, label)) is not None:   # PlatinumGames
        for nm, m in mem: scan_blob(m, f'{label}/{nm}', out)
        return
    if d[:4] in PG_STAGES and len(d) >= 0x18 and d[(o := struct.unpack_from('<I', d, 0xc)[0]):o + 4] == CTL_MAGIC: out.extend(pg_shader(d, label)); return
    if shaders := pg_sized_shader(d, label): out.extend(shaders); return
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc': out.extend(bnsh_shaders(d, m.start(), label))
    if MGFX in d: out.extend(mgfx_shaders(d, label))
    if NEEDLE in d: out.extend(needle_shaders(d, label))

def group(label):
    """source group (second half of the package file name)"""
    if label.startswith('main'): return 'main'
    for k, g in GROUPS:   # the profile's shader_groups, first match wins
        if k in label: return g
    return 'other'

def ps_inputs(code):
    """Generic attributes a pixel shader reads: SPH words 6–13 are the input map, 8 bits for each of the 32 attributes (Ryujinx ShaderHeader.ImapTypes), non-zero = read; the translated MSL inputs are exactly user(locn<attribute>)"""
    return {i for i in range(32) if struct.unpack_from('<I', code, 0x30 + 4 * (6 + (i >> 2)))[0] >> ((i & 3) * 8) & 0xff}

def pixel_input_cover(shaders):
    """{pixel key: code} → a deterministic greedy cover of their generic inputs; ties take the smaller key."""
    ins = {k: ps_inputs(code) for k, code in shaders.items()}
    left, cover = set().union(*ins.values()) if ins else set(), []
    while left:
        k = min(ins, key=lambda k: (-len(ins[k] & left), k)); cover.append(k); left -= ins[k]
    return cover

SINGLE_STAGE = ('#GLSLC@', '#UE', '#MTSPK@', '#MGFX@', '#NLG', '#PGSO', '#RAW@', '#TFXO@', '#NEEDLE@', '#SNOWDROP@', '#PHYRE@')   # sources whose every shader is a program of one stage: GLSLC outputs, Unreal's shader records, MT Framework's shader package (BNSH variations, Unity programs, MSHD and SDF pair theirs);
# MonoGame's effects, whose passes may name no vertex shader and keep the one bound (Celeste: 10 of its 13 effects draw through SpriteBatch's vertex shader, which is in main); Next Level Games'
# archives (Luigi's Mansion 3 makes 5,317 programs of one stage each at boot); PlatinumGames' shader objects (Bayonetta 3: one stage each, pg_shader); control sections loose in main
# (Advance Wars 1+2: Re-Boot Camp's Bink shaders, raw_shaders), which no file pairs; and YEBIS's effect file (tfxo_shaders)
def separable_pairs(keys):
    """Single-stage programs (13 Sentinels: each GLSLC output becomes a program with one stage, and BindProgram's stage bits combine vertex and pixel programs per draw; Unreal combines its shaders per draw
    from the pipeline state) have their real pairs only at draw time. vs_pad uses only the union of the locations the paired pixel shaders read, so every single-stage vertex shader is paired with
    a "cover": the fewest pixel shaders whose read locations together equal those of all pixel shaders (greedy, ties go to the smaller key); padded that way, a vertex shader links with any pixel shader.
    All pairs would be 376 × 375 rows; the cover needs a few rows × 376. No single-stage vertex shader, no cover"""
    single = [v for v in keys if keys[v][1] == 'vs' and any(s in keys[v][0] for s in SINGLE_STAGE) and '#var' not in keys[v][0]]   # '#var': a GLSLC output of several stages (glslc_program)
    if not single: return set(), []
    cover = pixel_input_cover({k: v[3] for k, v in keys.items() if v[1] == 'ps'})
    return {(v, p) for v in single for p in cover}, cover

SMASH_ARC = struct.pack('<Q', 0xABCDEF9876543210)   # smash_arc.MAGIC (scan_file reads only the first bytes, without importing the reader)
UE_TAG = {'SV': 'vs', 'SH': 'hs', 'SD': 'ds', 'SG': 'gs', 'SP': 'ps', 'SC': 'cs'}
def main_imports(img):   # the names main imports (its dynamic symbols)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools')); import nso_syms
    return {s for s, defined, _ in nso_syms.dynsyms(img) if not defined}

def glslc_module(exefs):   # a module of the profile's that defines glslcCompile, beside main in its ExeFS (profile_guess.py build_sources); None: main's import is never bound
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools')); import nso_syms
    for m in GAME.words('modules', 'main'):
        p = os.path.join(exefs, m)
        if m != 'main' and os.path.exists(p) and any(s == 'glslcCompile' and defined for s, defined, _ in nso_syms.dynsyms(nso_image(p))): return m
    return None

def ue_shaders(d, label):
    """An Unreal Engine shader code archive with every shader decoded (Octopath Traveler 0: the two .ushaderbytecode files of its pak, whose shaders are Oodle-compressed; the NSP script decodes them
    on the Mac with the game's own Oodle, uepak.py): each shader is an LSLG record holding one stage's code area and control section (uepak.lslg); the stage is the record's tag (SV vertex,
    SP pixel, SC compute), checked against the archive entry's frequency. The engine combines vertex and pixel shaders per draw, so they are paired as single-stage programs (separable_pairs)"""
    a, out = uepak.archive(d), []
    for i, (off, size, raw, fr) in enumerate(a['shaders']):
        if size != raw: red(f'{label}: shader {i} is still compressed (an archive the NSP script has not decoded)')
        rec = uepak.lslg(d[a['code'] + off:a['code'] + off + size])
        if not rec: red(f'{label}: shader {i} is not an LSLG record')
        tag, ctl, code = rec; st = UE_TAG.get(tag)
        if not st or uepak.FREQ.get(fr) != st: red(f'{label}: shader {i} has tag {tag} but frequency {fr}')
        why = check(label, st, ctl, code)
        if why: red(f'{label}: shader {i} ({st}): {why}')
        out.append((f'{label}#UE{i}', st, ctl, code))
    return out

def var_pairs(found):
    """(vertex key, pixel key) within the same BNSH variation; (vertex key, pixel key, 'prog') within the same BAHS program variation (bahs_variations).
    The tables stay separate; dmsh_build.py pads against both, as the pipeline prebuild uses both."""
    var = collections.defaultdict(dict)
    for where, st, ctl, code in found:
        g = where if '#var' in where else BAHS_VAR.get(where)
        if g: var[g][st] = skey(ctl, code)
    return {(v['vs'], v['ps']) + (('prog',) if '#BAHS#prog' in g else ()) for g, v in var.items() if 'vs' in v and 'ps' in v}

def scan_file(args):
    """→ (relative path, {key: …}, pairs, extra); extra = this file's Unity type trees, 'defer' (no type tree, scanned again in the second pass), or ('unreadable', why).
    A file no reader can read (a format revision the reader does not know: its red line, or the exception it raised) is left out, and the split goes on: one such
    archive used to end the whole split with nothing written (PR #282: one of Mario Strikers: Battle League's 879 Next Level Games archives). main reports them"""
    try: return scan_file_in(args)
    except (Exception, SystemExit) as e:   # red() is a SystemExit
        why = str(e).removeprefix('[RED] ') if isinstance(e, SystemExit) else f'{type(e).__name__}: {e}'
        if not why.lstrip('/').startswith(args[1]): why = f'{args[1]}: {why}'   # most red lines name the file already
        print(f'[RED] {why} (its shaders are left out)', flush=True)
        return args[1], {}, set(), ('unreadable', why)

def scan_file_in(args):
    path, rel = args[:2]
    stem, ext = os.path.splitext(path)
    if ext in ('.data', '.patch', '.debug', '.nxpc') and os.path.exists(stem + '.dict'): return rel, {}, set(), {}   # read through its Next Level Games index (nlg_shaders)
    with open(path, 'rb') as f: head = f.read(16)
    found = []; trees = {}
    if len(args) > 2 and args[2][0] == b'ARH1':
        arh_shaders(path, rel, args[2][1], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == NLG_DICT:
        nlg_shaders(path, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:8] in (IDRK, PDRK):
        idrk_shaders(path, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:8] == ONEPACK:   # a range of Trinity packs (trpfs_jobs)
        trinity_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == ZIP and args[2][0] == ZIP:   # members of a zip archive (zip_jobs)
        zip_shaders(path, rel, args[2][1], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and args[2][0] == UE_LEGACY:   # files of a legacy Unreal pak (ue_legacy_jobs)
        ue_legacy_shaders(path, rel, args[2][1], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == PKZL:   # entries of a PlatinumGames PKZ (pkz_jobs)
        pkz_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == CPK:   # entries of a CRI CPK (cpk_jobs)
        cpk_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == KPKA:   # entries of an RE Engine PAK (kpka_jobs)
        kpka_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:8] == SMASH_ARC:   # files of Super Smash Bros. Ultimate's data.arc (smash_arc_jobs)
        smash_arc_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == b'BDF3':   # entries of a FromSoftware dvdbnd archive (bdt_jobs)
        bdt_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2:   # a range of LINKDATA entries (linkdata_jobs)
        linkdata_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == SPKG or head[:4] == b'\0\0\1\0' and path.endswith('.gz'):   # Three Houses' loose shader packages and chunked zlib files (kt_shaders)
        kt_shaders(open(path, 'rb').read(), '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == RFRM:   # a Retro Studios form or pack (rfrm_shaders)
        found += rfrm_shaders(open(path, 'rb').read(), '/' + rel)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == b'BERG':   # Snowdrop's SDF data slices: shader objects in individual compressed records
        with open(path, 'rb') as f: found += snowdrop_shaders(f.read(), '/' + rel)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == MTARC:   # an MT Framework archive (mtarc_members): its shader package
        mtarc_shaders(open(path, 'rb').read(), '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == SCNE:   # a Bezel Engine archive (bea_members): each entry scanned as a file of its own (the shaders are BNSH in its .bfsha entries)
        for nm, m in bea_members(open(path, 'rb').read(), '/' + rel): scan_blob(m, f'/{rel}/{nm}', found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:8] == GFPAK:   # a Game Freak archive (gfpak_members): each entry scanned as a file of its own (the shaders are BNSH entries)
        for nm, m in gfpak_members(open(path, 'rb').read(), '/' + rel): scan_blob(m, f'/{rel}{nm}', found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == b'SRYK':
        import vbf
        with open(path, 'rb') as f:
            for entry in vbf.entries(path):
                # Compiled effects and loose GPU programs; texture and sound members carry no shader programs.
                if '.fx' in entry[0] or '/shaders/' in entry[0] or entry[0].endswith(('.vs.bin', '.ps.bin', '.fs.bin')):
                    found += phyre_shaders(vbf.read(f, entry), f'/{rel}/{entry[0]}')
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == ZSTD_MAGIC: d = unzs(path)   # by the magic: Paper Mario: The Thousand-Year Door names its zstd files .zst (8,726, its shaders among them)
    else:
        with open(path, 'rb') as f: d = f.read()
    # Monolith also ships these containers loose, without an ARH/ARD archive
    # (Xenoblade DE). The generic prefilter below otherwise drops even HCPS.
    # DRSM model streams contain several embedded XBC1 resources.
    if d[:4] in (b'HCPS', b'xbc1', b'DRSM') or d[:8] == b'cmp\0zlib':
        scan_blob(d, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if d[:4] == b'RYHP' or d[8:12] == GFX_MAGIC:
        found += phyre_shaders(d, '/' + rel)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] != ZSTD_MAGIC and head[4:8] in (ZSTD_MAGIC, LZ4_FRAME_MAGIC): d = unsized(d, '/' + rel)   # HAL's .cmp containers (Kirby and the Forgotten Land: 7,201 of them, 5,121 zstd frames with 19,364 BNSH inside and 2,080 LZ4 frames). Read raw, the split found 121 shaders and the game asked for 900 it did not have ("[RED] shader ... is not in the shader map")
    if d[:4] == b'Yaz0': d = yaz0(d)
    if d[:3] == b'XNB' and d[4] == 5 and d[5] & 0xC0 and len(d) > 14:   # a compressed XNB (Stardew Valley: 3,535 LZ4 files, 8 LZX): only effects (EffectReader) carry MGFX shaders, and for LZX the reader names sit in the first block, so it decides whether the rest is worth decoding
        if d[5] & 0x80:
            head_d = xnb_decompress(d, first_block=True)
            if b'EffectReader' not in head_d: return rel, {}, set(), {}
            d = head_d if len(head_d) >= struct.unpack_from('<I', d, 10)[0] else xnb_decompress(d)
        else:
            d = xnb_decompress(d)
    if d[:8] == b'UnityFS\0' or (d[8:11] == b'\0\0\0' and __import__('unityfs').is_serialized(d)):
        import unityfs
        try: pairs = unity_shaders(d, '/' + rel, found, trees if not TREES else dict(TREES))
        except unityfs.NoTree: return rel, {}, set(), 'defer'
        return rel, uniq_keys(found), var_pairs(found) | {(a, b, 'prog') for a, b in pairs}, trees
    elif b'BNSH\0\0\0\0' not in d and b'BAHS' not in d and d[:4] != b'MSHD' and not (d[:4] == b'SARC' and b'Yaz0' in d) and b'\x91\x68\x86\x19' not in d and MGFX not in d and NEEDLE not in d: return rel, {}, set(), {}   # a SARC may hold Yaz0 members
    else:
        scan_blob(d, '/' + rel, found)
        # whole GLSLC outputs in a file no other reader takes (FINAL FANTASY TACTICS - The Ivalice Chronicles: its engine's technique files, resources_nx/…/shader/*.tec, 261 outputs beside
        # main's 49). Metroid Dread's MSHD files hold GLSLC outputs too, which mshd_shaders already reads; no other profile's loose files hold any
        if not found and d[:4] != b'MSHD' and b'\x91\x68\x86\x19' in d: found += glslc_shaders(d, '/' + rel, need=False)
    return rel, uniq_keys(found), var_pairs(found), trees

def uniq_keys(found):
    uniq = {}
    for where, st, ctl, code in found:
        key = skey(ctl, code)
        if key in uniq and not same_shader(uniq[key][2:4], (ctl, code)): red(f'key {key} is the same but the control section or code area differs ({where})')
        uniq.setdefault(key, (where, st, ctl, code))
    return uniq

def nso_image(path):   # an NSO's image (dm/tools/nso.py); nso.Error (a ValueError) on a damaged file
    import nso
    with open(path, 'rb') as f: return bytes(nso.load(f.read(), path).image)

def scan_batch(jobs):   # scan_file over a batch of jobs, in a pool worker
    return [scan_file(j) for j in jobs]


def pooled(ex, jobs, total, start=0, chunk=1):
    """scan_file over jobs in the pool, the results in job order, with a 'progress: <done>/<total>' line at most twice a second (the NSP script shows it as
    the stage's count). The count is of the jobs finished in any order: counted in order it would stall behind one slow archive range while the others finish"""
    batches = [jobs[i:i + chunk] for i in range(0, len(jobs), chunk)]
    futures = {ex.submit(scan_batch, b): len(b) for b in batches}
    done, pending, shown = start, set(futures), 0.0
    while pending:
        finished, pending = cf.wait(pending, timeout=0.5)
        done += sum(futures[f] for f in finished)
        if not pending or time.monotonic() - shown >= 0.5: print(f'progress: {done}/{total}', flush=True); shown = time.monotonic()
    return [r for f in futures for r in f.result()]


def main():
    romfs_l, main_l, pkg, mapout, factsout = sys.argv[1:6]
    pairsout = sys.argv[6] if len(sys.argv) > 6 else None
    psopairsout = sys.argv[7] if len(sys.argv) > 7 else None   # pso_pairs.tsv: supplemental BAHS program variations and legacy Unity pass-input covers
    dlc_v = sys.argv[8].split(',') if len(sys.argv) > 8 and sys.argv[8] else []   # extra content has no ExeFS; each root is the extracted DLC index
    # Several versions (base game + update): the RomFS and main arguments are comma-separated and paired one to one; the output is their union (keys are content hashes, shaders shared by versions appear once).
    # The first version's source column is unchanged; later versions are prefixed with their workspace directory name (e.g. v327680): /v327680/Shader/…, main@v327680. They sort after the first version, so existing rows keep their package indices.
    romfs_v, main_v = romfs_l.split(','), main_l.split(',')
    if len(romfs_v) != len(main_v): red(f'{len(romfs_v)} RomFS and {len(main_v)} main: they must pair one to one')
    files, results, common = [], [], None
    for i, (romfs, mainnso) in enumerate(zip(romfs_v, main_v)):
        tag = os.path.basename(os.path.dirname(os.path.abspath(romfs))) if i else ''
        files += sorted((os.path.join(dp, f), (tag + '/' if tag else '') + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
        img = nso_image(mainnso)
        if not i:
            import unityfs
            try: common = unityfs.common_strings(img)
            except ValueError: common = None   # not a Unity game
        found = []; scan_blob(img, 'main' + ('@' + tag if tag else ''), found)
        found += glslc_shaders(img, 'main' + ('@' + tag if tag else ''), need=False)
        found += tfxo_streams(img, 'main' + ('@' + tag if tag else ''))   # YEBIS's effect file, a zlib stream in main (Pokémon Sword)
        found += retro_main_forms(img, 'main' + ('@' + tag if tag else ''))   # Retro's startup material archive, LZSS in main (Donkey Kong Country: Tropical Freeze)
        found += raw_shaders(img, 'main' + ('@' + tag if tag else ''), {c for _, _, c, _ in found})
        # GLSLC outputs the game compiles at run time, captured on the Mac (hle_glslc.c): <version dir>/glslc_capture, or DM_GLSLC_CAPTURE. A game needs them when main holds GLSL sources and
        # calls glslcCompile in a module it loads (Fire Emblem: Three Houses; profile_guess.py shader_sources). Captain Toad: Treasure Tracker imports it with agl's GLSL sources, but no module defines it
        cap = os.environ.get('DM_GLSLC_CAPTURE') or os.path.join(os.path.dirname(os.path.abspath(romfs)), 'glslc_capture')
        names = sorted(f for f in os.listdir(cap) if f.endswith('.glslc')) if os.path.isdir(cap) else []
        if not names and glsl_sources(img) and 'glslcCompile' in main_imports(img) and glslc_module(os.path.dirname(os.path.abspath(mainnso))):
            print(f'[RED] main holds GLSL sources and imports glslcCompile, but there are no captured GLSLC outputs in {cap}: the shaders the game compiles at run time will have no translation', flush=True)
        for nm in names: found += glslc_shaders(open(os.path.join(cap, nm), 'rb').read(), f'/glslc_capture/{nm}')
        # Unreal Engine shader code archives decoded on the Mac (uepak.py): <version dir>/ue_shaders, or DM_UE_SHADERS; needed when the RomFS has Unreal paks
        ued = os.environ.get('DM_UE_SHADERS') or os.path.join(os.path.dirname(os.path.abspath(romfs)), 'ue_shaders')
        names = sorted(f for f in os.listdir(ued) if f.endswith('.ushaderbytecode')) if os.path.isdir(ued) else []
        if not names and uepak.unreal_paks(romfs, inline=False): red(f'the RomFS has Unreal paks, but there are no decoded shader code archives in {ued} (the NSP script writes them)')
        for nm in names: found += ue_shaders(open(os.path.join(ued, nm), 'rb').read(), f'/ue_shaders/{nm}')
        results.append(('main', {skey(c, k): (w, s, c, k) for w, s, c, k in found}, var_pairs(found), {}))
    for romfs in dlc_v:
        name = os.path.basename(os.path.normpath(romfs))
        tag = 'aoc/' + (name.removesuffix('.img') if is_image(romfs) else name)
        files += sorted((os.path.join(dp, f), tag + '/' + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
    zdicts = {k: v for r in romfs_v + dlc_v for k, v in zs_dicts(r).items()}   # Tears of the Kingdom's zstd dictionaries (zs_dicts)
    keys = {}
    # First pass: every file; Unity bundles carry type trees, and the Shader trees are collected on the way. Second pass: Unity serialized files without type trees are scanned again with the borrowed trees
    J = int(os.environ.get('J', '8'))
    big = [j for p, r in files for j in (linkdata_jobs(p, r) or [])]   # Three Houses' 6.9 GB DATA1.bin: one job per range of entries, not one read of the whole file
    pak_jobs, paks = kpka_jobs(files)   # Monster Hunter Rise's PAKs: the newest copy of each path, in ranges of entries
    tr_jobs, trp = trpfs_jobs(files)   # Pokémon Legends: Z-A's Trinity packs (data.trpfs and its index data.trpfd), decoded by the game's own Oodle in the Mac host
    ue_jobs, uep = ue_legacy_jobs(files)   # Unreal paks with inline shaders: DRAGON QUEST XI S's legacy ones (the game's own LZ5 in the Mac host), OCTOPATH TRAVELER II's (Zlib)
    bd_jobs, bdts = bdt_jobs(files)   # Dark Souls Remastered's dvdbnd archives: the entries of each .bdt by its .bhd5 index, in ranges
    arc_jobs, arcs = smash_arc_jobs(files)   # Super Smash Bros. Ultimate's data.arc: its current files, in ranges
    pz_jobs, pkzs = pkz_jobs(files)   # Bayonetta 3's PKZ archives, Oodle Kraken decoded by the game's own decoder in the Mac host
    ck_jobs, cpks = cpk_jobs(files)   # Bayonetta 2's CRI CPK archives (CRILAYLA entries)
    zp_jobs, zips = zip_jobs(files)   # Portal's game.zip (Deflate members)
    ah_jobs, arhs = arh_jobs(files)   # Monolith's ARH1/ARD archives and embedded XBC1/HCPS programs
    big += pak_jobs + tr_jobs + ue_jobs + bd_jobs + arc_jobs + pz_jobs + ck_jobs + zp_jobs + ah_jobs
    files = [(p, r) for p, r in files if not linkdata_jobs(p, r) and p not in paks and p not in trp and p not in uep and p not in bdts and p not in arcs and p not in pkzs and p not in cpks and p not in zips and p not in arhs]
    with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, {}, zdicts)) as ex:
        n = len(big) + len(files)
        results += pooled(ex, big, n) + pooled(ex, files, n, len(big), chunk=16)
    trees, defer = {}, []
    for rel, uniq, pr, extra in results:
        if extra == 'defer': defer.append(rel)
        elif isinstance(extra, dict): trees.update(extra)
    if defer:
        # a hash no bundle supplied takes the layout unityfs.py has for it (KNOWN_TREES); only here, so files a bundle lends a tree to keep their place in the tables (Ace Attorney's built-ins)
        import unityfs
        for k in unityfs.KNOWN_TREES: trees.setdefault(k, unityfs.known_tree(*k))
        byrel = {r: p for p, r in files}
        with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, trees, zdicts)) as ex: again = list(ex.map(scan_file, [(byrel[r], r) for r in defer]))
        for rel, uniq, pr, extra in again:
            if extra == 'defer': red(f'{rel}: Unity serialized file without type trees, and no bundle has a Shader type tree of the same hash to borrow')
        results = [r for r in results if r[3] != 'defer'] + again
    bad = [extra[1] for *_, extra in results if isinstance(extra, tuple)]
    if bad: print(f'[RED] {len(bad)} file(s) could not be read, their shaders are left out (first: {bad[0]})', flush=True)   # build_ipa_from_nsp.py reads this line
    per_src = collections.Counter(); pairs = set()
    for rel, uniq, pr, _ in results:
        pairs |= pr
        for key, v in uniq.items():
            if key in keys:
                if not same_shader(keys[key][2:4], v[2:4]): red(f'key {key} is the same but the control section or code area differs ({v[0]} / {keys[key][0]})')
                continue
            keys[key] = v; per_src[group(v[0])] += 1
    prog = {(a, b) for a, b, *t in pairs if t} - pairs; pairs = {x for x in pairs if len(x) == 2}
    sp, cover = separable_pairs(keys); pairs |= sp
    if cover: print(f'single-stage programs: {len({v for v, _ in sp}):,} vertex shaders are paired with a cover of {len(cover)} pixel shader(s) ({" ".join(cover)})')
    os.makedirs(pkg, exist_ok=True)
    for f in os.listdir(pkg):
        if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
    recs = collections.defaultdict(list); rows = []; facts = {}; stat = collections.Counter()
    for key in sorted(keys, key=lambda k: (group(keys[k][0]), keys[k][0], k)):   # the same input gives byte-identical output
        where, st, ctl, code = keys[key]
        name = f'{st}_{group(where)}.bin'
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
        if rec.find(CTL_MAGIC) != 0x38 + len(code): red(f'key {key}: the control magic appears in the code area, dmsh_build would split the record wrongly')
        if rec.count(b'BTCDBTCD') != 1: red(f'key {key}: the record separator appears inside the record')
        rows.append((name, len(recs[name]), STAGE_NAME[st], key, where)); recs[name].append(bytes(rec)); stat[st] += 1
        if st == 'cs':
            x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
            if not x * y * z: red(f'compute shader {key}: control +0x748 thread group is 0 ({where})')
            facts[key] = {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}
    for name, rr in recs.items():
        with open(os.path.join(pkg, name), 'wb') as f: f.write(b''.join(rr))
    with open(mapout, 'w') as f:
        kinds = ' / '.join(k for k, hit in (('BNSH / BAHS', any('#BNSH@' in r[4] or '#BAHS@' in r[4] for r in rows)), ('Unity Shader', any('#var' in r[4] and '#BNSH@' not in r[4] and '#MSHD#' not in r[4] and '#SDF#' not in r[4] and '#GLSLC@' not in r[4] and '#SNVN@' not in r[4] for r in rows)),
                                            ('Retro Studios SNVN', any('#SNVN@' in r[4] for r in rows)),
                                            ('MSHD', any('#MSHD#' in r[4] for r in rows)), ('GLSLC', any('#GLSLC@' in r[4] for r in rows)), ('RE Engine SDF', any('#SDF#' in r[4] for r in rows)),
                                            ('MT Framework SPK', any('#MTSPK@' in r[4] for r in rows)), ('MonoGame MGFX', any('#MGFX@' in r[4] for r in rows)),
                                            ('PlatinumGames shader objects', any('#PGSO' in r[4] for r in rows)),
                                            ('Snowdrop shader objects', any('#SNOWDROP@' in r[4] for r in rows)),
                                            ('Unreal Engine shader code archives', any('#UE' in r[4] for r in rows))) if hit)
        f.write(f'# kind: evidence  {NOTE} ({kinds} in RomFS + main) → control section + code area keys (dm/gfx/tools/shader_split.py)\n')
        f.write('# column 5 records where each key first appears; no PC shader pairing supplied\n')
        f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    # Hand-written facts the machine code cannot give (the profile's shader_facts_manual.tsv, same row format) are merged over the generated ones, so the table stays reproducible
    manual = GAME.table('shader_facts_manual.tsv')
    if os.path.exists(manual):
        for ln in open(manual):
            if ln.strip() and not ln.startswith('#'): k, v = ln.rstrip('\n').split('\t', 1); facts[k] = {**facts.get(k, {}), **json.loads(v)}
    with open(factsout, 'w') as f:
        f.write('# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. No PC shader pairing supplied: only the thread group / shared memory of compute shaders (taken from the control section); other queries get the translator defaults\n')
        for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
    if pairsout:
        with open(pairsout, 'w') as f:
            f.write('# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(pairs): f.write(f'{a}\t{b}\n')
    if psopairsout:
        with open(psopairsout, 'w') as f:
            f.write('# kind: evidence  supplemental vertex / pixel shader pairs (BAHS program variations and legacy Unity pass-input covers) for vertex output padding and the pipeline prebuild (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(prog - pairs): f.write(f'{a}\t{b}\n')
    print(f'shaders: {len(rows)} (' + ', '.join(f'{STAGE_NAME[s]} {n}' for s, n in sorted(stat.items())) + '); sources ' + ', '.join(f'{g} {n}' for g, n in per_src.most_common()) + f'; compute shader answers {len(facts)} rows' + (f'; variation pairs {len(pairs)}' if pairsout else ''))

if __name__ == '__main__':
    import nso
    try: main()
    except nso.Error as e: red(str(e))   # a damaged code module

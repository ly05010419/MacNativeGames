#!/usr/bin/env python3
# Provisional game profiles: the profile keys a game's own files decide, for a game without a hand-made profile in dm/games (scripts/build_ipa_from_nsp.py writes them).
# facts_of(ExeFS, RomFS) reads the code modules (dynamic symbols, sizes, SDK strings) and the RomFS file headers (a romfs_manifest.tsv from nx_extract --manifest, or a RomFS folder);
# guess(facts) turns the facts into profile keys, each with the reason written above it in game.tsv. Every rule was checked against the hand-made profiles; --check runs that comparison again.
# mem_used needs the rewritten modules (mem_used below, after the rewrite). The alias / heap split of a game with virtual address memory that sets a heap needs its heap request:
# the NSP script's heap check boots it on the Mac until that request (heap_signals below), a few seconds. The keys no short run decides (render_cond, tex_flush_upload,
# tex_dirty_upload, core_preempt, gpu_timestamps, feedback_snap, alpha_from_red, vconv_range, present_interval) keep their defaults for a developer: a minute's boot without input
# cannot tell the games that need them (Tears of the Kingdom) from those that only use what they answer (Wonder's conditional clears, Breath of the Wild's flushed textures).
# Usage: profile_guess.py <ExeFS dir> <romfs_manifest.tsv | RomFS dir>   the facts and the guessed keys
#        profile_guess.py --check [<NSP root>]                        every profile in dm/games against its extraction under <NSP root> (default /Volumes/T7/nsp): where the guess differs
import collections, os, re, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools')); import nso_syms   # dynamic symbols, the host's hooks

MAGICS = [(b'UnityFS\0', 'UnityFS'), (b'UnityWeb', 'UnityWeb'), (b'UnityRaw', 'UnityRaw'), (b'BNSH', 'BNSH'), (b'BAHS', 'BAHS'), (b'SARC', 'SARC'), (b'Yaz0', 'Yaz0'),
          (bytes.fromhex('28b52ffd'), 'zstd'), (b'FRES', 'bfres'), (b'BNTX', 'BNTX'), (b'BWAV', 'BWAV'), (b'FSAR', 'BFSAR'), (b'CPK ', 'CRI CPK'), (b'CRID', 'CRI USM'),
          (b'@UTF', 'CRI UTF'), (b'AKPK', 'Wwise pck'), (b'BKHD', 'Wwise bnk'), (b'FSB5', 'FMOD'), (b'\x89PNG', 'PNG'), (b'RIFF', 'RIFF'), (b'OggS', 'Ogg'), (b'PK\x03\x04', 'zip'), (b'NSO0', 'NSO'),
          (bytes.fromhex('03022307'), 'SPIR-V'), (b'ONEPACK\0', 'Trinity pack'), (b'ARC\0', 'MT Framework ARC'), (b'SCNE', 'Bezel Engine archive'), (b'XNB', 'XNB'), (b'pkzl', 'PlatinumGames PKZ')]
PACKED = {'UnityFS', 'UnityWeb', 'Yaz0', 'zstd', 'BWAV', 'CRI CPK', 'CRI USM', 'Wwise pck', 'FMOD', 'PNG', 'Ogg', 'zip', 'Trinity pack', 'MT Framework ARC', 'Bezel Engine archive', 'PlatinumGames PKZ'}   # file headers of compressed / media formats (zip_store)
# IL2CPP by the metadata file every IL2CPP player opens: Unity 2022's main (Ace Attorney Investigations Collection) has no il2cpp_ API names, Engage's 2020.3 has both
MARKERS = [(b'nvnBootstrapLoader', 'NVN'), (b'vkCreateInstance', 'Vulkan'), (b'eglGetProcAddress', 'EGL / OpenGL'), (b'global-metadata.dat', 'Unity IL2CPP'), (b'UnityEngine', 'Unity'),
           (b'sead::', 'Nintendo sead'), (b'agl::', 'Nintendo agl'), (b'gsys::', 'Nintendo gsys'), (b'Unreal', 'Unreal'), (b'UE4', 'Unreal 4'), (b'Havok', 'Havok'), (b'CRIWARE', 'CRIWARE'), (b'Wwise', 'Wwise'),
           (b'MonoGame.Framework', 'MonoGame'), (b'FMOD mixer', 'FMOD')]
GLSLC_MAGIC = b'\x91\x68\x86\x19'   # GLSLC compiler outputs (shader_split.py glslc_shaders)
GL_SRC = rb'\0#version [0-9]'      # a GLSL source string as dm/gfx/tools/gl_shaders.py takes it from main (Balatro's main has none, only glslang's messages that start with '#version:')
GL_FILES = ('.mvgl', '.vert', '.frag')   # the RomFS files gl_shaders.py reads GLSL from
APP_POOL = 0xCD500000               # the application memory pool of a 4 GB Switch (3285 MiB), what every profile but Engage's reports
MiB = 1 << 20


def nso(path):
    """An NSO module: its three segments decompressed (the image without bss), build ID, segment sizes and bss size"""
    import lz4.block
    d = Path(path).read_bytes(); flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img), d[0x40:0x50].hex(), [sz for _, _, sz in segs], struct.unpack_from('<I', d, 0x3c)[0]


def kind(m):   # a short name for what a module is: its NintendoSDK component, GLSLC, or the nn:: namespace most of its exports are in
    if m['comps']: return sorted(m['comps'])[0]
    if any(s.startswith('glslc') for s in m['exports']): return 'GLSLC'
    areas = collections.Counter(nso_syms.area(s) for s in m['exports'])
    return areas.most_common(1)[0][0] if areas else '?'


def romfs_files(romfs):   # (path, size, first 32 bytes) of every RomFS file: from nx_extract's manifest, or read from a RomFS folder
    romfs = Path(romfs)
    if romfs.is_file():
        for line in romfs.read_text(errors='replace').splitlines():
            if line.startswith('#'): continue
            path, size, head = line.split('\t'); yield path, int(size), bytes.fromhex(head)
        return
    for d, _, names in os.walk(romfs):
        for n in names:
            p = Path(d) / n
            with open(p, 'rb') as f: head = f.read(32)
            yield '/' + str(p.relative_to(romfs)), p.stat().st_size, head


def npdm(path):   # (system resource size, program ID) from main.npdm; a non-zero system resource means virtual address memory (Engage 16 MB); (0, None) without the file
    if not Path(path).exists(): return 0, None
    d = Path(path).read_bytes(); a = d.find(b'ACI0')
    return (struct.unpack_from('<I', d, 0x14)[0] if d[:4] == b'META' else 0), (struct.unpack_from('<Q', d, a + 0x10)[0] if a >= 0 else None)


def facts_of(exefs, romfs):
    """What the profile rules (and the survey report) need from a game's code modules and RomFS"""
    exefs = Path(exefs)
    F = {'mods': {}, 'markers': {}, 'sdkver': set(), 'comps': set(), 'unity': set(), 'glslc': 0, 'glsl_src': 0, 'gl_names': 0, 'gl_src': 0, 'sysres': 0, 'program': None}
    for p in sorted(exefs.iterdir()):
        with open(p, 'rb') as f:
            if f.read(4) != b'NSO0': continue
        img, bid, szs, bss = nso(p)
        syms = nso_syms.dynsyms(img)
        end = ((((len(img) + bss + 0x3FFF) & ~0x3FFF) + 0x4000 + 0xFFFF) & ~0xFFFF)   # where patch_nso.py would append the patch area (NEAR_PATCH_OFF)
        m = F['mods'][p.name] = {'bid': bid, 'sizes': szs, 'bss': bss, 'far': end > 0x7000000,   # far: the patch area goes before the base (patch_nso.py FAR), the bss counts as used memory (hle_svc.c big_bss_bytes)
                                 'imports': {s for s, defined, _ in syms if not defined}, 'exports': {s for s, defined, bind in syms if defined and bind in (1, 2)},
                                 'comps': {c.decode().rsplit('-', 1)[0] for c in re.findall(rb'NintendoSDK_(?!nnSdk)\w+-[0-9_]+', img)}}
        F['sdkver'] |= {v.decode() for v in re.findall(rb'(?i)NintendoSDK_nnSdk-([0-9_]+)', img)}   # SDK 12 spells it NintendoSdk_, SDK 16 NintendoSDK_
        F['comps'] |= {c.decode() for c in re.findall(rb'SDK MW\+Nintendo\+([\w]+-[0-9_]+)', img)}
        for pat, name in MARKERS:
            if pat in img: F['markers'].setdefault(name, []).append(p.name)
        if p.name == 'main':
            F['unity'] = {v.decode() for v in re.findall(rb'20[12][0-9]\.[0-9]+\.[0-9]+f[0-9]', img)}
            F['glslc'], F['glsl_src'], F['gl_names'] = img.count(GLSLC_MAGIC), img.count(b'#version '), img.count(b'glShaderSource')
            F['gl_src'] = len(re.findall(GL_SRC, img))
            F['main_markers'] = {name for pat, name in MARKERS if pat in img}
        m['kind'] = {'main': 'the game', 'sdk': 'nnSdk', 'rtld': 'rtld'}.get(p.name) or kind(m)
    F['sysres'], F['program'] = npdm(exefs / 'main.npdm')
    major = {v.split('_')[0] for v in F['sdkver']}
    F['sdk'] = min((int(v) for v in major if v.isdigit()), default=0)

    # RomFS: counts by top level (two levels: Unity keeps everything under Data/), extension and header magic
    R = F['romfs'] = {'n': 0, 'bytes': 0, 'tops': collections.Counter(), 'topb': collections.Counter(), 'exts': collections.Counter(), 'extb': collections.Counter(), 'extc': collections.Counter(),
                      'mags': collections.Counter(), 'magb': collections.Counter(), 'dirs': collections.Counter(), 'biggest': (0, ''), 'root': set()}
    for path, size, h in romfs_files(romfs):
        R['n'] += 1; R['bytes'] += size
        parts = path.strip('/').split('/')
        top = '/'.join(parts[:2]) if len(parts) > 2 else parts[0] if len(parts) > 1 else '(root)'
        R['tops'][top] += 1; R['topb'][top] += size
        ext = os.path.splitext(path)[1].lower()
        R['exts'][ext or '(none)'] += 1; R['extb'][ext] += size
        if len(parts) > 1: R['dirs'][parts[0]] += 1
        else: R['root'].add(parts[0])
        R['biggest'] = max(R['biggest'], (size, path.strip('/')))
        mag = next((n for m, n in MAGICS if h.startswith(m)), None)
        if mag: R['mags'][mag] += 1; R['magb'][mag] += size
        if mag in PACKED: R['extc'][ext] += 1
    return F


# ---- Rules ----
def host_hooks():   # the host's own functions for imports, leaving out the HLE_FORCE wrappers that still call the module's function
    return nso_syms.host_hooks(str(HERE.parent / 'host'), forced=False)


def guess(F, hooks=None):
    """{key: (value, reason)} for the keys the files decide, in profile order; keys left out keep game.py's defaults"""
    G, mods, R = {}, F['mods'], F['romfs']
    imp = mods['main']['imports']
    hooks = host_hooks() if hooks is None else hooks
    preset = engine(F)

    # Code modules: a subsdk main takes nothing from (nothing the host does not supply itself) is not loaded: NVIDIA's OpenGL / Vulkan drivers and the Vulkan loader under a Vulkan game
    # (the host's Vulkan layer answers main's vk* calls, and a loaded driver would take them first), GLSLC and nn::pcm under Super Mario Galaxy. The movie modules stay.
    # NVIDIA's OpenGL and Vulkan drivers are never loaded: they program the GPU through NvRm, which the host does not have. Absolum (FNA3D on NVN) imports EGL for SDL3's GL support, never used
    order = lambda m: (m != 'main', m == 'sdk', int(m[6:]) if m.startswith('subsdk') and m[6:].isdigit() else 0)
    keep, drop = [], []
    for name in sorted((m for m in mods if m != 'rtld'), key=order):
        if name in ('main', 'sdk') or ((imp & mods[name]['exports']) - hooks and mods[name]['kind'] not in GPU_DRIVERS): keep.append(name)
        else: drop.append(f'{name} ({mods[name]["kind"]})')
    G['modules'] = (' '.join(keep), f'not loaded, main takes nothing from them the host does not supply: {", ".join(drop)}' if drop else 'every module main imports from')


    # Memory: the guest block is 256 MB in front + alias region + stack region + heap region (hle_svc.c mem_init)
    giant = sum(m['bss'] for n, m in mods.items() if n in keep and m['far'])
    heap_api = HEAP_API in imp
    if F['sysres']:   # virtual address memory: Engage's 5 GB block. With a heap, the heap check moves the alias / heap split once it sees the heap the game asks for (Wonder, Tears of the Kingdom);
        # without one the game maps everything in the alias region itself (Hollow Knight: Silksong's Unity 6 through AllocateAddressRegion / AllocateMemoryPages, 1.5 GB in its first room;
        # Absolum's and Pokémon Legends: Z-A's StandardAllocator), which a 256 MB alias region cannot hold
        mem = (0x140000000, 0x118000000, APP_POOL, f'the NPDM declares {F["sysres"] // MiB} MB of system resource (virtual address memory) and main ' +
               ('sets a heap: a 5 GB block with a large alias region; the heap check sizes the heap region from the heap the game asks for' if heap_api else
                'never sets a heap: a 5 GB block with a 4.375 GB alias region, where the game maps its memory itself'))
    else:
        heap = max(0x10000000, -(-(APP_POOL - 64 * MiB - giant) // 0x10000000) * 0x10000000) if heap_api else 0x10000000
        mem = (0x30000000 + heap, 0x10000000, APP_POOL,
               ('main sets a heap of at most the pool − 64 MB' + (f' − {giant // MiB} MB of bss (counted as used memory)' if giant else '') + f': a {heap // MiB} MB heap region')
               if heap_api else 'main never sets a heap (its memory is its own bss): a 256 MB heap region')
    G['mem_reserve'] = (f'{mem[0]:#x}', mem[3]); G['mem_alias'] = (f'{mem[1]:#x}', ''); G['mem_stack'] = ('0x10000000', ''); G['app_pool'] = (f'{mem[2]:#X}'.replace('0X', '0x'), '')
    G['sys_resource'] = (f'{F["sysres"]:#x}', f'the NPDM declares {F["sysres"] // MiB} MB of system resource' + ('' if F['sysres'] else
                         ': IsVirtualAddressMemoryEnabled is false, as on Horizon (Big Brain Academy: Brain vs. Brain sets a heap only then)'))

    if F.get('main_markers', set()) & {'Unreal', 'Unreal 4'} and R['exts'].get('.pak') and 'nvnBootstrapLoader' in imp:
        G['shader_groups'] = ('ShaderArchive-Global-=global ShaderArchive-=game', 'the decoded shader code archives: the engine\'s global shaders and the game\'s material shaders')
        G['mem_reserve'] = ('0x1028000000', 'Unreal: nninitStartup reserves a 62 GB address region and its FMallocBinned3 takes 37 GB of it, so a 64 GB alias region for the bring-up on the Mac. '
                            'An iPhone needs the allocator cut for the build and a capped region (game_args), as Octopath Traveler 0\'s profile (docs/agents/03)')
        G['mem_alias'] = ('0x1008000000', '')
    G.update(preset)
    return G


HEAP_API = '_ZN2nn2os17SetMemoryHeapSizeEm'
GPU_DRIVERS = {'NintendoSDK_OpenGL', 'NintendoSDK_Vulkan'}


# ---- Shader sources: decided from the game files at every build, not kept in the profile (scripts/build_ipa_from_nsp.py; shader_split.py reads what those steps write) ----
def shader_sources(imports, glsl_src, glslc_mods, spirv, unreal_paks, trinity_packs=0, legacy_paks=0, gl_names=0, gl_src=0, gl_files=0, oodle_pkz=0):
    """{name: (value, reason)}: graphics (vulkan when main imports Vulkan functions and not nvnBootstrapLoader; gl when it imports eglCreateWindowSurface and names glShaderSource, so it compiles
    GLSL through OpenGL (gl_names: how often main has that name; Absolum imports the same EGL functions for SDL3 but renders with NVN and names no GL function); else nvn; vkGetInstanceProcAddr
    alone is SDL's Vulkan loader, which Balatro's LÖVE links and never uses), vk_capture (a Vulkan game without SPIR-V files generates its shaders at run time), gl_capture (an OpenGL game with no
    GLSL source where gl_shaders.py looks, gl_src strings in main and gl_files files in the RomFS: it puts its shaders together at run time, LÖVE from its Lua), glslc_capture (main holds GLSL sources and calls glslcCompile in a module the game loads: it compiles some shaders at run time), ue_decode (Unreal paks keep the shaders Oodle-compressed
    in their shader code archives, which only the game's own Oodle decodes), oodle_packs (Trinity packs, or oodle_pkz PlatinumGames PKZ archives with Oodle Kraken entries:
    Oodle-compressed file by file, which the shader split decodes with the game's own decoder in the Mac host's tool mode), ue_legacy (legacy Unreal paks, version 4: the shaders are inline in the cooked packages, which the shader split reads through the game's own decoder the same way).
    unreal_paks counts the version 11 paks, legacy_paks the older ones. Checked against every profile: Hades and Super Mario Galaxy Vulkan, Galaxy alone with no SPIR-V file, Three Houses alone compiling
    shaders (13 Sentinels holds 106 GLSL sources but does not import glslcCompile; Captain Toad: Treasure Tracker imports it with agl's GLSL sources, but no module defines it), Octopath Traveler 0
    alone with version 11 Unreal paks, DRAGON QUEST XI S alone with legacy ones, Pokémon Legends: Z-A alone with Trinity packs, Digimon Story Cyber Sleuth and Balatro alone with OpenGL (of the 23 games
    extracted; Hades and both Galaxies import 99 or 100 Vulkan functions, Balatro only vkGetInstanceProcAddr), Balatro alone composing its GLSL"""
    vk = sum(1 for s in imports if re.match(r'vk[A-Z]', s) and s != 'vkGetInstanceProcAddr'); vulkan = bool(vk) and 'nvnBootstrapLoader' not in imports
    gl = not vulkan and 'eglCreateWindowSurface' in imports and bool(gl_names)
    return {'graphics': ('vulkan', f'main imports {vk} Vulkan functions and not nvnBootstrapLoader: the host\'s Vulkan layer (dm/gfx/vk/hle_vk.m)') if vulkan else
                        ('gl', 'main imports EGL and names glShaderSource: it renders with OpenGL and compiles GLSL, the host\'s GL layer (dm/gfx/gl/hle_gl.m)') if gl else ('nvn', 'main imports nvnBootstrapLoader'),
            'vk_capture': (vulkan and not spirv, 'no SPIR-V file in the RomFS: the game generates its shaders at run time (Super Mario Galaxy), captured by booting it on the Mac'),
            'gl_capture': (gl and not gl_src and not gl_files, 'no GLSL source in the RomFS or main: the game puts its shaders together at run time (LÖVE: Balatro), captured by booting it on the Mac'),
            'glslc_capture': (not vulkan and not gl and bool(glsl_src) and 'glslcCompile' in imports and bool(glslc_mods),
                              f'main holds {glsl_src} GLSL sources and calls glslcCompile in {", ".join(glslc_mods) or "no module it loads"}: shaders compiled at run time (Fire Emblem: Three Houses), captured by booting it on the Mac'),
            'ue_decode': (not vulkan and not gl and bool(unreal_paks), f'{unreal_paks} Unreal pak(s): the shaders are Oodle-compressed in their shader code archives (Octopath Traveler 0), decoded by booting the game on the Mac'),
            'oodle_packs': (not vulkan and not gl and bool(trinity_packs or oodle_pkz), (f'{trinity_packs} Trinity pack file(s) (ONEPACK, arc/data.trpfs: Pokémon Legends: Z-A)' if trinity_packs else
                            f'{oodle_pkz} PlatinumGames PKZ archive(s) with Oodle Kraken entries (Bayonetta 3)') + ', Oodle-compressed: the shader split decodes them with the game\'s own decoder in the Mac host (dm/host/hle_oodle.c DM_OODLE_SERVE)'),
            'ue_legacy': (not vulkan and not gl and bool(legacy_paks), f'{legacy_paks} legacy Unreal pak(s) (version 4, UE 4.18: DRAGON QUEST XI S): the shaders are inline in the cooked packages, compressed in the pak; the shader split decodes them with the game\'s own decoder in the Mac host (dm/host/hle_oodle.c DM_OODLE_SERVE)')}


def build_sources(exefs, romfs, modules):
    """shader_sources from an extraction (its ExeFS and RomFS folders) and the modules the profile loads"""
    exefs = Path(exefs); main = nso(exefs / 'main')[0]
    imports = {s for s, defined, _ in nso_syms.dynsyms(main) if not defined}; glsl_src = main.count(b'#version '); gl_names = main.count(b'glShaderSource'); gl_src = len(re.findall(GL_SRC, main))
    glslc_mods = [m for m in modules if m != 'main' and glsl_src and 'glslcCompile' in imports and (exefs / m).exists()
                  and any(s == 'glslcCompile' and defined for s, defined, _ in nso_syms.dynsyms(nso(exefs / m)[0]))]
    spirv = 0
    if any(re.match(r'vk[A-Z]', s) for s in imports):   # only a Vulkan game's RomFS is read
        spirv = sum(1 for _, _, head in romfs_files(romfs) if head.startswith(bytes.fromhex('03022307')))
    sys.path.insert(0, str(HERE.parent / 'gfx/tools')); import uepak
    trinity = 0
    for d, _, names in os.walk(romfs):
        for n in names:
            if n.endswith('.trpfs'):
                with open(os.path.join(d, n), 'rb') as f: trinity += f.read(8) == b'ONEPACK\0'
    gl_files = sum(1 for _, _, names in os.walk(romfs) for n in names if n.lower().endswith(GL_FILES)) if 'eglCreateWindowSurface' in imports else 0
    oodle_pkz = 0   # PlatinumGames PKZ archives whose name table (after the 0x20-byte entries) names Oodle Kraken: their entries are decoded with the game's own decoder
    for d, _, names in os.walk(romfs):
        for n in names:
            if n.endswith('.pkz'):
                with open(os.path.join(d, n), 'rb') as f:
                    h = f.read(0x20)
                    if h[:4] == b'pkzl':
                        cnt, eo, nl = struct.unpack_from('<III', h, 0x10); f.seek(eo + 0x20 * cnt); oodle_pkz += b'OodleKraken\0' in f.read(nl)
    return shader_sources(imports, glsl_src, glslc_mods, spirv, len(uepak.unreal_paks(str(romfs), legacy=False)), trinity, len(uepak.unreal_paks(str(romfs), legacy=True)), gl_names, gl_src, gl_files, oodle_pkz)


def va_heap(sysres, imports):   # virtual address memory (an NPDM system resource) and a heap: the alias / heap split is a starting point the heap check corrects (Wonder, Tears of the Kingdom; Engage's pool is its own measurement)
    return bool(sysres) and HEAP_API in imports


def heap_check_needed(exefs):   # va_heap from an extraction's ExeFS (the NSP script may reuse a provisional profile without surveying again)
    img = nso(Path(exefs) / 'main')[0]
    return va_heap(npdm(Path(exefs) / 'main.npdm')[0], {s for s, defined, _ in nso_syms.dynsyms(img) if not defined})


def engine(F):   # keys that follow from the engine rather than from a general signal
    if 'hagi.config' in F['romfs']['root']:   # NERD's Hagi Wii layer: the pointer is the touch screen in handheld mode (docked it is the Joy-Con gyro, which the host does not have)
        return {'op_mode': ('handheld', 'Hagi (hagi.config): the Wii pointer is the touch screen in handheld mode'), 'pad_layout': ('full', 'Hagi: a 3D game pad (D-pad, right stick)')}
    return {}


def mem_used(patched, modules):
    """mem_used from the rewritten modules (scripts/build_ipa_from_nsp.py, after the rewrite): SetHeapSize checks heap + image against the pool (hle_svc.c), the game asks for pool − used − bss,
    so used memory must cover the image without its bss. None when the default 64 MB does (Age of Calamity: 66.6 MB of modules → 72 MB)"""
    total = big = 0
    for m in modules:
        hdr = dict(re.findall(r'^# (\w+)\t(\S+)', (Path(patched) / f'{m}.fixups.tsv').read_text(errors='replace'), re.M))
        if int(hdr.get('PATCH_OFF', '0'), 16) < 0:   # patch area before the base: the image is IMAGE_SIZE, the part beyond the file is bss
            size = int(hdr['IMAGE_SIZE'], 16); big += size - int(hdr['FILE_IMAGE_LEN'], 16)
        else: size = ((Path(patched) / f'{m}.patched.bin').stat().st_size + 0x3FFF) & ~0x3FFF
        total += size
    need = ((total + 0x1FFFFF) & ~0x1FFFFF) - big
    return need + 4 * MiB if need > 64 * MiB else None


# ---- The heap check (scripts/build_ipa_from_nsp.py heap_check): host log lines → profile keys ----
def heap_signals(log, g):
    """{key: (value, reason)} from a heap-check run's host log (hle_svc.c SetHeapSize); g is the profile the run used"""
    out = {}
    req = [int(x, 16) for x in re.findall(r'SetHeapSize OOM: req (0x[0-9a-f]+)', log)]
    if req:   # the heap region is too small: give it the request (rounded up to 128 MB) and the alias region the rest of the block (Tears of the Kingdom: 0x86000000 → its 0x98000000)
        reserve, stack = int(g.get('mem_reserve'), 0), int(g.get('mem_stack'), 0)
        heap = -(-max(req) // 0x8000000) * 0x8000000
        alias = reserve - 0x10000000 - stack - heap
        if alias >= 0x40000000: out['mem_alias'] = (f'{alias:#x}', f'the game asked for a {max(req) / 2**30:.2f} GB heap: a {heap / 2**30:.3f} GB heap region')
        else: out['mem_reserve'] = (f'{0x10000000 + 0x40000000 + stack + heap:#x}', f'the game asked for a {max(req) / 2**30:.2f} GB heap')
    top = [float(x) for x in re.findall(r'\[mem\] alias region mapped up to \+([0-9.]+) GB', log)]
    alias = int(g.get('mem_alias'), 0) / 2**30
    if not req and '[mem] heap ' not in log and int(g.get('sys_resource', '0'), 0) and top and max(top) > alias - 0.25 and int(g.get('app_pool'), 0) > 0x40000000:
        # no heap, and the alias region nearly full within seconds: nninitStartup handed pool − used to StandardAllocator as one address region and the game reserves regions of its own
        # beside it (Unity 2020 / 2022: Ace Attorney Investigations Collection, Brain Memory Training Academy, Patrick's Parabox, Monster Train 2, AI: THE SOMNIUM FILES - nirvanA Initiative,
        # whose heap check reached +4.26 GB of 4.38 in 5 s with the 3.2 GB pool); Unity 2018.3 maps only in StandardAllocator's region (Raging Loop keeps the hardware pool)
        out['app_pool'] = ('0x40000000', f'no heap set and the alias region used up to +{max(top):.2f} GB of {alias:.2f} GB in the heap check: a 1 GB pool, so StandardAllocator\'s region leaves the game room for its own')
    m = re.search(r'\[mem\] SetHeapSize (0x[0-9a-f]+) \+ modules (0x[0-9a-f]+) > app pool', log)
    if m and '[mem] heap ' not in log[m.end():]:   # the pool check failed and no SetHeapSize succeeded after it (Mario retries 2 MB smaller until one does; Age of Calamity gives up)
        need = (int(m.group(2), 16) + 0x1FFFFF) & ~0x1FFFFF
        if need > int(g.get('mem_used', '0x4000000'), 0): out['mem_used'] = (f'{need + 4 * MiB:#x}', f'the heap check failed for good: {need / MiB:.0f} MB of modules, more than the used memory reported')
    return out


# ---- Checking the rules against the hand-made profiles ----
DEFAULTS = {'op_mode': 'console', 'pad_layout': 'simple'}
NO_SIGNAL = {'pad_layout', 'op_mode'}   # no static signal outside the engine presets; the player switches the pad layout in game, DM_OPMODE in env.txt sets the mode


def find_extraction(root, g):   # (ExeFS, RomFS manifest or folder) for a profile: its survey, else a work folder's extraction of the packed version
    for d in sorted(Path(root).iterdir()):
        s = d / f'{g.id}_survey/extract'
        if (s / 'ExeFS').is_dir() and (s / 'romfs_manifest.tsv').exists(): return s / 'ExeFS', s / 'romfs_manifest.tsv'
    for data in (f'{g.id}-work/data', f'{g.id}_build/extract'):   # the by-hand work layout (docs/development.md), then the NSP script's (<NSP folder>/<id>_build/extract/<version>): either holds what --check needs
        for d in sorted(Path(root).iterdir()):
            base = d / data
            for v in [g.get('package', '')] + sorted(x.name for x in base.glob('v*')):
                w = base / v
                if (w / 'ExeFS').is_dir() and (w / 'RomFS').is_dir(): return w / 'ExeFS', w / 'RomFS'
    return None


def check(root):
    sys.path.insert(0, str(HERE)); import game
    hooks, bad = host_hooks(), 0
    for g in game.all_games():
        if g.dir.parent != HERE: continue
        ex = find_extraction(root, g)
        if not ex: print(f'{g.id} {g.get("name")}: no extraction under {root}'); continue
        F = facts_of(*ex); G = guess(F, hooks)
        diff, unknown, later = [], [], []
        for k in list(DEFAULTS) + ['modules', 'mem_reserve', 'mem_alias', 'mem_stack', 'app_pool']:
            want, got = g.get(k, DEFAULTS.get(k, '')), G.get(k, (DEFAULTS.get(k, ''),))[0]
            if k.startswith('mem_') or k == 'app_pool': want, got = int(want, 0), int(got, 0)
            if want == got: continue
            if k in ('mem_alias', 'app_pool') and F['sysres']: later.append((k, got, want))
            elif k in NO_SIGNAL and k not in G: unknown.append((k, got, want))
            else: diff.append((k, got, want))
        fmt = lambda v: f'{v:#x}' if isinstance(v, int) else v
        print(f'{g.id} {g.get("name")}: ' + ('ok' if not diff else 'DIFF ' + '; '.join(f'{k} {fmt(got)} (profile {fmt(want)})' for k, got, want in diff))
              + (' | heap check / measured: ' + ', '.join(f'{k} {fmt(got)} (profile {fmt(want)})' for k, got, want in later) if later else '')
              + (' | no signal: ' + ', '.join(f'{k} {fmt(want)}' for k, _, want in unknown) if unknown else ''))
        bad += bool(diff)
    return bad


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--check']:
        sys.exit(1 if check(a[1] if len(a) > 1 else '/Volumes/T7/nsp') else 0)
    if len(a) != 2: sys.exit(__doc__ or 'usage: profile_guess.py <ExeFS dir> <romfs_manifest.tsv | RomFS dir> | profile_guess.py --check [<NSP root>]')
    F = facts_of(*a)
    for n, m in F['mods'].items():
        print(f'{n}: {m["kind"]}, build {m["bid"]}, bss {m["bss"] / MiB:.1f} MB{" (far)" if m["far"] else ""}, {len(m["imports"])} imports, {len(m["exports"])} exports')
    print(f'SDK {F["sdk"]}, system resource {F["sysres"] // MiB} MB, GLSLC outputs in main {F["glslc"]}, GLSL sources {F["glsl_src"]}, SPIR-V files {F["romfs"]["mags"]["SPIR-V"]}')
    for k, (v, why) in guess(F).items(): print(f'{k}\t{v}' + (f'\t# {why}' if why else ''))

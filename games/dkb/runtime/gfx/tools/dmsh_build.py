#!/usr/bin/env python3
# Route B offline build: original Switch shaders → SPIR-V (dmsh_xlate, the Ryujinx translator) → spirv-cross MSL → metallib + replay reflection JSON.
# No translation at runtime (iOS has no JIT): build output goes into the shader cache directory, file name = Switch control segment key (dm_ctl_sha16, same as column 4 of shader_map.tsv) + "_b".
# Source of query answers: the answer table in the game profile, dm/games/<id>/shader_facts.tsv (dmsh_facts.py) - texture types, vertex attribute / fragment output component types, compute thread group size and shared memory.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys = all); build output is actually written to <output cache dir>/b-<chain version>/ (same for IOSOUT), for the chain version see dmsh_chain.py.
# A full build (no keys) also stamps both directories with the game and the digest of its profile inputs (inputs.tsv) and writes the profile's c1_full.tsv and cb_extent.tsv (c1_guard.py, cb_extent.py)
# from its Mac outputs. One output directory per game: the chain version is the same for every game   Environment: DM_GAME=game profile (required, dm/games/<id>), XLATE=path to the translation tool (default the in-repo dmsh_xlate_bin), DMSH_CAS=content-addressed cache dir (default <output cache dir>/cas), DMSH_PREFILL=Mac library root[=iOS library root] (separated by :, their b-*/ go into the MSL-keyed compile cache first; sibling b-*/ always do), J=concurrency
# Build output: <key>_b.metallib, <key>_b.json (replay reflection, including drv_reads, the runtime read offsets into driver segments c0 / c2), <key>_b.metal (MSL)
# Environment IOSOUT=dir: the same MSL also goes through dm/ios/tools/ios_metal.compile to produce an iphoneos metallib + reflection (chain_metal records the iOS compile convention)
# Final summary: success / failure counts and the distribution of reasons; distribution of driver segment read offsets (those outside the range the replay layer can fill are listed separately)
import inspect, os, re, sys, json, mmap, struct, hashlib, shutil, subprocess, tempfile, collections, threading, concurrent.futures as cf
import dmsh_geometry, dmsh_atomic, dmsh_wide, dmsh_abi as abi   # abi: the driver segment layout and the binding kinds the host shares (dm/gfx/nvn/dmsh_abi.h)
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)
HOME = os.path.expanduser('~')
dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:]); cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain, dmsh_fetch, dmsh_facts, dmsh_mtlcache, dmsh_vin, dmsh_pull, dmsh_alpha, dmsh_logic, dmsh_mesh, dmsh_ampere, tool_lock   # translation chain version: verify the tools against the lock file first; build output goes into b-<V>/
_bad = tool_lock.check_tools()
if _bad: sys.exit('\n'.join('[RED] translation chain tools do not match the lock file: ' + b for b in _bad) + '\n(if the tools changed, run tool_lock.py --update-lock and commit the lock file; if the translator just was not rebuilt, rebuild it)')
CHAIN = dmsh_chain.version(); out = os.path.join(out, 'b-' + CHAIN)
GAME = dmsh_chain.load_game()   # the current game profile (DM_GAME): the tables come from here
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'[RED] {dmsh_chain.HEADER} does not hold the current translation chain version {CHAIN}: run dmsh_chain.py --header first and commit it (otherwise the host cannot find these libraries)')
INPUTS = dmsh_chain.inputs(GAME)   # the digest of the profile's inputs, which a full build stamps on its libraries (b-<V>/inputs.tsv)
XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
SPIRV_CROSS = dmsh_chain.SPIRV_CROSS   # the in-repo build the lock records
FACTS = dmsh_facts.load()
if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile; an empty table = every query answered with defaults)')
IOSOUT = os.environ.get('IOSOUT')   # destination for requested iOS libraries (iphoneos metallib + reflection)
# Which metallibs this run builds: DMSH_TARGETS=mac,ios (default: both when IOSOUT is set, else mac). The Mac
# sources and reflection are written even when 'mac' is left out (the profile's c1_full / cb_extent tables are
# made from them), so an iOS-only build skips only the macOS compile, about half of the Metal work.
TARGETS = {t.strip() for t in (os.environ.get('DMSH_TARGETS') or ('mac,ios' if IOSOUT else 'mac')).split(',') if t.strip()}
if not TARGETS or not TARGETS <= {'mac', 'ios'}: sys.exit(f'[RED] DMSH_TARGETS takes mac and/or ios: {",".join(sorted(TARGETS)) or "empty"}')
if 'ios' in TARGETS and not IOSOUT: sys.exit('[RED] DMSH_TARGETS asks for iOS libraries without IOSOUT')
if 'ios' not in TARGETS: IOSOUT = None   # every side library, cache, cleanup and stamp follows the same selection
if IOSOUT:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal
    IOSOUT = os.path.join(IOSOUT, 'b-' + CHAIN); os.makedirs(IOSOUT, exist_ok=True)
for _d in [out] + ([IOSOUT] if IOSOUT else []):   # one chain version serves every game, so b-<V>/ alone does not keep two games' libraries apart
    _g = dmsh_chain.read_stamp(_d).get('game')
    if _g and _g != GAME.id: sys.exit(f'[RED] {_d} holds the shader libraries of game {_g}, not {GAME.id}: give each game its own output directory')
MAP = str(GAME.table('shader_map.tsv'))
BF_WANT_FILE = str(GAME.table('bf_want.tsv'))
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
DUAL_WANT_FILE = str(GAME.table('dual_want.tsv'))   # pixel shaders a pipeline blends with their output 1 as the second source (see fs_dual), produced by the host (DM_NVN_DUAL_WANT)
DUAL_WANT = {line.split()[0] for line in open(DUAL_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(DUAL_WANT_FILE) else set()
XC = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
# spirv-cross writes the translator's FMin / FMax / FClamp as fast::min / max / clamp, whose result for a NaN operand is the GPU's choice; Maxwell's FMNMX returns the other operand
# and its saturation gives 0, as Metal's precise:: functions (IEEE fmin / fmax) do on every GPU. The M4 Pro's fast ones happen to agree, an A20 Pro's do not: Super Mario Galaxy 2's
# GX lighting takes max(0, NaN) from light directions left at zero, and the half of Mario its fill light faces came out white on the iPhone (GitHub issue 72; vk_shaders.py PRECISE)
def precise(msl): return re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', msl)

# Ampere HMMA.16816 with an f16 accumulator (agl's upscaler networks) reaches the MSL as a call of dmsh_hmma16816_f16(lane, block, A0..A3, B0, B1, C0, C1, out D1) → D0, with
# the subgroup's 256-word block of the scratch shared memory as the last argument (InstEmitAmpereTensor.AHmma, TensorPass, HelperFunctionManager.GenerateHmma16816F16). The
# translator's body computes D lane by lane from the block; here it becomes simdgroup_matrix code over the same block: A row-major (halves 0..255, 16 a row), B transposed
# (256..383, row n, 16 halves of k, loaded transposed), C / D row-major (384..511, 8 a row). Per HMMA on an M4 Pro: 1.08 ns a SIMD-group against 5.4 for the shuffle emulation
HMMA_BODY = '''{
    threadgroup uint* s = &%(scr)s[uint(%(blk)s)];
    uint g = uint(%(lane)s) >> 2, t = uint(%(lane)s) & 3;
    s[g * 8 + t] = uint(%(a0)s); s[(g + 8) * 8 + t] = uint(%(a1)s); s[g * 8 + 4 + t] = uint(%(a2)s); s[(g + 8) * 8 + 4 + t] = uint(%(a3)s);
    s[128 + g * 8 + t] = uint(%(b0)s); s[128 + g * 8 + 4 + t] = uint(%(b1)s);
    s[192 + g * 4 + t] = uint(%(acc0)s); s[192 + (g + 8) * 4 + t] = uint(%(acc1)s);
    simdgroup_barrier(mem_flags::mem_threadgroup);
    threadgroup half* h = (threadgroup half*)s;
    simdgroup_half8x8 a00, a01, a10, a11, b0, b1, c0, c1;
    simdgroup_load(a00, h, 16, ulong2(0, 0)); simdgroup_load(a01, h, 16, ulong2(8, 0)); simdgroup_load(a10, h, 16, ulong2(0, 8)); simdgroup_load(a11, h, 16, ulong2(8, 8));
    simdgroup_load(b0, h + 256, 16, ulong2(0, 0), true); simdgroup_load(b1, h + 256, 16, ulong2(8, 0), true);
    simdgroup_load(c0, h + 384, 8, ulong2(0, 0)); simdgroup_load(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_multiply_accumulate(c0, a00, b0, c0); simdgroup_multiply_accumulate(c0, a01, b1, c0);
    simdgroup_multiply_accumulate(c1, a10, b0, c1); simdgroup_multiply_accumulate(c1, a11, b1, c1);
    simdgroup_store(c0, h + 384, 8, ulong2(0, 0)); simdgroup_store(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint d0 = s[192 + g * 4 + t], d1 = s[192 + (g + 8) * 4 + t];
    simdgroup_barrier(mem_flags::mem_threadgroup);
    %(d1)s = as_type<%(dt)s>(d1);
    return as_type<%(rt)s>(d0);
}'''
# A store only the very first invocation of a dispatch makes (a compute shader's preamble: `if (-local.x == workgroup.x << log2(local size)) buffer[k] = value`). Hoian_Proc in Splatoon Raiders resets its append counter
# that way, with the other invocations adding to the counter at their end, hundreds of instructions later. On the Switch the first workgroup starts ahead of them. Apple's GPUs start workgroups in no order: a workgroup that had appended before
# the reset lost its plants to the next one's, a different few each frame, and they flickered. So the store leaves the shader and the host makes it, in order, before the dispatch (the reflection's first_store, nvn_mtl.m). Only this exact
# condition counts: it holds for one invocation of a dispatch with a flat local size. Returns the shader and [(storage binding's MSL name, word, value)]
FIRST_STORE = re.compile(r'float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*int (\w+) = as_type<int>\(\1\) << (\d+);\s*'
                         r'int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \5 \+ 1;\s*bool (\w+) = \6 != \3;\s*if \(!\7\)\s*\{\s*((?:\(\*\w+\.\w+\)\._m0\[\d+\] = \d+u;\s*)+)\}')
def first_store(msl, local):
    m = FIRST_STORE.search(msl)
    if not m or not local or local[1:] != [1, 1] or 1 << int(m.group(4)) != local[0]: return msl, []
    stores = [(n, int(w), int(v)) for n, w, v in re.findall(r'\(\*\w+\.(\w+)\)\._m0\[(\d+)\] = (\d+)u;', m.group(8))]
    if len(stores) > 4: return msl, []
    body = msl[m.start():m.end()]
    return msl[:m.start()] + body[:body.rindex('if (!')] + msl[m.end():], stores
def hmma_msl(msl):
    m = re.search(r'\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{', msl)
    if not m: return msl
    params, depth, cur = [], 0, ''   # commas inside template arguments (spvUnsafeArray<uint, N>) do not separate parameters
    for ch in m.group(2) + ',':
        depth += {'<': 1, '>': -1}.get(ch, 0)
        if ch == ',' and depth == 0: params.append(cur.strip()); cur = ''
        else: cur += ch
    scr = [x for x in params if x.startswith('threadgroup ')]
    plain = [x for x in params if not x.startswith('threadgroup ')]
    if len(scr) != 1 or len(plain) != 11: raise ValueError('dmsh_hmma16816_f16: unexpected parameters ' + m.group(2)[:200])
    def group1(pattern, text, how=re.search):   # the first group of a match the helper's signature must have
        found = how(pattern, text)
        if not found: raise ValueError('dmsh_hmma16816_f16: unexpected parameter ' + text[:100])
        return found.group(1)
    name = lambda x: group1(r'(\w+)$', x)
    names = [name(x) for x in plain]
    dt = group1(r'thread (\w+)&', plain[10], re.match)
    v = dict(zip(['lane', 'blk', 'a0', 'a1', 'a2', 'a3', 'b0', 'b1', 'acc0', 'acc1', 'd1'], names), scr=name(scr[0]), dt=dt, rt=m.group(1))
    start = m.end() - 1; depth = 0
    for i in range(start, len(msl)):
        depth += {'{': 1, '}': -1}.get(msl[i], 0)
        if depth == 0: break
    else: raise ValueError('dmsh_hmma16816_f16: body not closed')
    return msl[:start] + HMMA_BODY % v + msl[i + 1:]
# Border colours: Maxwell's sampler takes any RGBA border colour, Metal's only transparent black, opaque black or opaque white, and the host's sampler gets the nearest of them
# (nvn_border.h). A filtered sample is linear in the border colour, r = Σ w·texel + w_border·B, so two samples through samplers that differ only in their border colour give the rest:
# with Metal colours Bn (the sampler) and Bw (its twin), r = s_n + (s_w - s_n)·k, k = (B - Bn) / (Bw - Bn) per channel. Each sampler member a shader samples gets a twin member
# <member>_w and a bit in the block the shader reads (abi.KIND_BORDER: a mask word, then k per bit); the host sets the bit only for a sampler whose colour Metal lacks and whose wrap
# mode reaches the border, so every other sample costs a uniform branch and the twin sample runs only there. Tears of the Kingdom's grass bend map (ff9def5b3c418851) samples the
# previous map with clamp-to-border and border 0.5 (upright): read as transparent black, every row that scrolled in flattened the grass. Samples through sampler arrays, compares
# and gathers keep the nearest Metal colour
BORDER_HELPERS = f'''
struct dm_border_t {{ uint mask; float4 k[{abi.BORDER_SAMPLERS}]; }};
static inline __attribute__((always_inline)) float4 dm_bmix(float4 r, float4 w, float4 k) {{ return r + (w - r) * k; }}
static inline __attribute__((always_inline)) float dm_bmix(float r, float w, float4 k) {{ return r + (w - r) * k.x; }}
template<typename V, typename T, typename... A> static inline __attribute__((always_inline)) V dm_bs(V r, constant dm_border_t* b, uint i, T t, sampler w, A... a)
{{
    if (b->mask >> i & 1u) r = dm_bmix(r, t.sample(w, a...), b->k[i]);
    return r;
}}
'''
def border_emu(msl):
    """The samples of the argument buffer's textures through its sampler members, T.sample(S, args) → dm_bs(T.sample(S, args), block, bit, T, S_w, args) (see BORDER_HELPERS);
    the source unchanged when it samples through none. Only samples of float textures: an integer texture is not filtered, and its border colour is Maxwell's integer one, which
    the host does not correct (it keeps Metal's nearest)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    if not body: return msl
    # members with attributes after their id too (see reflect): the twins' ids go after every one of them
    members = [(m.group(1).strip(), m.group(2), int(m.group(3))) for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1), re.M)]
    samplers = [n for ty, n, _ in members if ty == 'sampler']
    ftex = {n for ty, n, _ in members if re.fullmatch(r'(texture|depth)\w*<float(,.*)?>', ty)}
    call = re.compile(r'spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+), ')
    used = {m.group(2) for m in call.finditer(msl) if m.group(1) in ftex}
    bit = {n: i for i, n in enumerate([n for n in samplers if n in used][:abi.BORDER_SAMPLERS])}
    if not bit: return msl
    def width(ty):   # argument buffer entries a member takes
        a = re.fullmatch(r'array<.*,\s*(\d+)>', ty); return int(a.group(1)) if a else 1
    nid = max(aid + width(ty) for ty, _, aid in members)   # the new members go after every entry spirv-cross gave out
    add = ''.join(f'    sampler {n}{abi.BORDER_TWIN} [[id({nid + i})]];\n' for n, i in bit.items()) + f'    constant dm_border_t* {abi.BORDER_MEMBER} [[id({nid + len(bit)})]];\n'
    end = body.end(1)
    msl = msl[:end] + ('' if msl[end - 1] == '\n' else '\n') + add + msl[end:]
    for m in reversed([m for m in call.finditer(msl) if m.group(2) in bit and m.group(1) in ftex]):   # the last first: a sample inside another's arguments is rewritten before the outer one copies them
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(msl[i], 0); i += 1
        args = msl[m.end():i - 1]
        twin = f'spvDescriptorSet0.{m.group(2)}{abi.BORDER_TWIN}'
        msl = msl[:m.start()] + f'dm_bs({msl[m.start():i]}, spvDescriptorSet0.{abi.BORDER_MEMBER}, {bit[m.group(2)]}u, spvDescriptorSet0.{m.group(1)}, {twin}, {args})' + msl[i:]
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n' + BORDER_HELPERS, 1)
CAS =os.environ.get('DMSH_CAS', os.path.join(cache_root, 'cas'))
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
# -fpreserve-invariance: vertex positions are marked [[invariant]], but without this flag Metal does not promise that two different shaders compute the same expression to the same value; on Maxwell the same multiply-adds always agree.
# Super Mario Odyssey draws a depth pre-pass with alRenderDepthOnly, then the G-buffer with another vertex shader and depth test less-or-equal: a hair off and the test failed, leaving those pixels with old content (Cap Kingdom's ground in 32x32 blocks)
# -O1: the front end's -O2 (CorrelatedValuePropagation, then SimplifyCFG) folds the translated shaders' repeated tests of one value into switches whose cases jump past their own join.
# Apple's GPU backend then exceeds its control-flow ID budget ("Max Unstruct ID 758 ... exceeds supported limit", a note, not an error) and falls back to a slower lowering:
# Odyssey's alRenderMaterialUber took 25 minutes and 17.3 MB of GPU code, 31 s and 1.3 MB at -O1. Ordinary shaders come out within a few percent either way (the backend optimizes again)
MAC_METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance']
MAC_RECORD = ' '.join(MAC_METAL) + '; xcrun -sdk macosx metallib'
# The outer shader and fetch-variant caches restore compiled libraries directly,
# before the MSL compile cache can check their compiler. Keep override builds separate there too.
METAL_FP = dmsh_mtlcache.compiler_id('macosx') + ('\0iphoneos\0' + dmsh_mtlcache.compiler_id('iphoneos') if IOSOUT else '')
def mac_compile(msl, air, lib):   # same interface as ios_metal.compile
    subprocess.run([*MAC_METAL, msl, '-o', air], check=True)
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True)
# The NVN driver's constant segments have one layout in every game, which dm/gfx/nvn/dmsh_abi.h holds for the host and this build (through dmsh_abi.py); nvn_mtl.m fills them the same way at run time.
#   c2 C2_TEX + 8 r: handles bound with nvnCommandBufferBindTexture (combined texture and sampler), slot r. The driver has 32 such slots, but a game may bind higher ones, whose words lie in the
#     image table (Hades II binds slot 34, and no image), so a texture instruction's handle anywhere below the separate texture table names a combined slot. In the image
#     table that slot reads the image when one is bound there (nvn_mtl.m tex_slot_h): a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D),
#     whose type the translator takes from the shader's surface instructions on that handle (dmsh_xlate ImageDims)
#   c2 C2_IMG + 8 r: images (nvnCommandBufferBindImage), slot r < C2_IMG_SLOTS
#   c2 C2_SEPTEX + 8 r: separate textures (nvnCommandBufferBindSeparateTexture), reported as slot SEPTEX_SLOT0 + r, where the host binds them (hle_nvn.c, NVN_SEPTEX_SLOT0)
#   c2 C2_SAMP + 8 r: separate samplers (nvnCommandBufferBindSeparateSampler), slot r < C2_SEP_SLOTS
#   c0 base + 0x10 r: storage buffer r (64-bit address, 32-bit size), base C0_SSBO_<stage> (the hull / domain stages' bases were never seen)
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP
SSBO = {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'gs': abi.C0_SSBO_GS, 'ps': abi.C0_SSBO_PS}

def record_index(path):   # → (an open descriptor, the offsets of the package's records and its end): record k is bytes offsets[k]..offsets[k + 1]
    # Scanned through a read-only mapping that is closed again, so no package stays in this process. Pokopia's ps_rdb.bin (4.25 GB) read and cut into its 105,118
    # records left the process at 8.7 GB, and each fork (every spirv-cross and Metal compile) then took 75 ms instead of 3 with the GIL held: one shader worker at a time (GitHub issue #1036)
    fd = os.open(path, os.O_RDONLY); n = os.fstat(fd).st_size
    if not n: return fd, [0]
    with mmap.mmap(fd, n, access=mmap.ACCESS_READ) as m: return fd, [x.start() for x in re.finditer(b'BTCDBTCD', m)] + [n]

def clone(src, dst):   # APFS copy-on-write, else a plain copy; the clonefile call, not a cp: a fork of this large process costs about 6 ms
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    dmsh_mtlcache.clone(src, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():   # this file's code form (dmsh_chain.code_form): no comments, docstrings or formatting, and the rule functions' bodies blanked
    return dmsh_chain.code_form(open(__file__, 'r').read(), {f.__name__ for f, _ in RULES})

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run([SPIRV_CROSS, '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0ios\0' + (ios_metal.RECORD if IOSOUT else '') + '\0metal\0' + METAL_FP)

# The profile keys that change how a shader is built (nvn_*, shader_*), as dmsh_chain.inputs takes them: part of every shader's cache key
PROFILE_ROWS = dmsh_chain.profile_rows(GAME)
def shader_key(key, name, rec):
    """The per-shader cache key. It covers the profile's build keys and the shader's pairs; a vertex shader with pairs also takes its pixel shaders' keys (pair_keyed), since vs_pad reads their
    MSL. Before, these were only in the chain version, so a build after changing only them hit the old entries and filed them under the new version (with an empty DMSH_CAS as the workaround)"""
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else 'missing'.encode()
    map_row = MAP_ROWS.get(key, 'missing'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name) + b'\0profile\0' + PROFILE_ROWS + b'\0pairs\0' + ','.join(sorted(PAIRS.get(key, ()))).encode() +
               b'\0dual\0' + (b'1' if key in DUAL_WANT else b'0'))   # fs_dual emits another entry point for listed shaders; the cache must track that choice
    return hashlib.sha256(payload).hexdigest()
def pair_keyed(digest, pixel_digests):   # a vertex shader's key with its pixel shaders' keys (vs_pad)
    return hashlib.sha256((digest + '\0' + ','.join(pixel_digests)).encode()).hexdigest()

def cache_meta_path(key): return os.path.join(CAS, 'index', key + '.json')
def cache_path(digest): return os.path.join(CAS, 'shader', digest)
CAS_USED = '_b_cas.txt'   # in the Mac library directory b-<V>/: the cache_path digests of its last full build, one per line

def cache_outputs(key, digest, cached=False):
    root = cache_path(digest); files = {}
    for label, directory in (('mac', out), ('ios', IOSOUT)):
        if not directory: continue
        # fin(), paired vertex padding and the profile tables also need these on an iOS-only cache hit.
        required = [key + '_b.json'] + ([key + '_b.metal'] if label == 'mac' else [])
        if label in TARGETS: required.append(key + '_b.metallib')
        base = os.path.join(root, label) if cached else directory
        names = required + [key + s + ext for s in ('_bg0', '_bg1', '_bg2', '_bg3', '_bg4', '_bgv', '_bl0', '_bl1', '_bl3') for ext in ('.metallib', '.json', '.metal')
                            if (label == 'mac' or ext != '.metal') and (ext != '.metallib' or label in TARGETS)]
        found = [n for n in names if os.path.exists(os.path.join(base, n))]
        if not all(n in found for n in required): return None
        files[label] = found
    return root, files

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, files = found
    for label, names in files.items():
        directory = out if label == 'mac' else IOSOUT
        for name in names:
            src = os.path.join(root, label, name); dst = os.path.join(directory, name)
            if name.endswith('.json'):
                tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
                with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', open(src, 'rb').read(), count=1))
                os.replace(tmp, dst)
            else: clone(src, dst)
    return True

def cache_lookup(key, digest, rule_fp, code_fp):
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if restore_cache(key, digest): PRE_STATS['hit'] += 1; return True, 'hit'
        old = json.load(open(cache_meta_path(key))) if os.path.exists(cache_meta_path(key)) else None
        if old and old.get('code_fp') != code_fp: reason = 'code fingerprint changed: all rebuilt'
        elif old and old.get('rule_fp') != rule_fp: reason = 'rule changed'
        else: reason = 'new key'
        PRE_STATS['miss'] += 1; PRE_STATS[reason] += 1; return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    # Paired pixel MSL is an input to vs_pad; a diagnostic build without it must not cache an incomplete vertex shader under a complete dependency key.
    if any(not os.path.isfile(os.path.join(out, ps + '_b.metal')) for ps in PAIRS.get(key, ())): return
    found = cache_outputs(key, digest)
    if not found: return
    root, files = found
    for label, names in files.items():
        d = os.path.join(root, label); os.makedirs(d, exist_ok=True)
        for name in names: clone(os.path.join(out if label == 'mac' else IOSOUT, name), os.path.join(d, name))
        for name in names:
            if name.endswith('.json'):
                src = os.path.join(out if label == 'mac' else IOSOUT, name)
                value = json.load(open(src)); value.pop('fetch', None); value.pop('vin', None); value.pop('pull', None); value.pop('alpha', None); value.pop('logic', None)   # the side libraries are made again from the restored MSL (fetch_finish, vin_finish, pull_finish, alpha_finish, logic_finish)
                atomic_json(os.path.join(d, name), value)
    atomic_json(cache_meta_path(key), {'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp})

BF_STATS = collections.Counter()

def bf_digest(msl, key):
    h = hashlib.sha256()
    h.update(msl.encode())
    h.update(b'\0mac\0' + MAC_RECORD.encode())
    h.update(b'\0ios\0' + (ios_metal.RECORD if IOSOUT else '').encode())
    h.update(b'\0metal\0' + METAL_FP.encode())
    h.update(b'\0bf-want\0' + (b'1' if key in BF_WANT else b'0'))
    return h.hexdigest()

def bf_cache_root(digest): return os.path.join(CAS, 'variant', digest)

def bf_cache_restore(key, digest, fmeta, fmsl):
    root = bf_cache_root(digest)
    names = ['metal'] + (['mac.metallib'] if 'mac' in TARGETS else []) + (['ios.metallib'] if IOSOUT else [])
    if not all(os.path.exists(os.path.join(root, n)) for n in names): return False
    clone(os.path.join(root, 'metal'), fmsl)
    if 'mac' in TARGETS: clone(os.path.join(root, 'mac.metallib'), os.path.join(out, key + '_bf.metallib'))
    if IOSOUT: clone(os.path.join(root, 'ios.metallib'), os.path.join(IOSOUT, key + '_bf.metallib'))
    BF_STATS['hit'] += 1
    return True

def bf_cache_publish(digest, fmsl, maclib, ioslib, fmeta):
    root = bf_cache_root(digest); os.makedirs(root, exist_ok=True)
    clone(fmsl, os.path.join(root, 'metal'))
    if maclib: clone(maclib, os.path.join(root, 'mac.metallib'))
    if IOSOUT: clone(ioslib, os.path.join(root, 'ios.metallib'))
    atomic_json(os.path.join(root, 'fetch.json'), fmeta)

def write_refl(key, refl):
    for target, d in (('mac', out),) + ((('ios', IOSOUT),) if IOSOUT else ()):
        atomic_json(os.path.join(d, key + '_b.json'), MTL.with_compiler(target, refl))

def fetch_finish(key, src, refl):
    if refl['stage'] != 'ps': return refl
    # Those with rewritable samples get variants as before; those without get an output-mapping-only variant, and only for keys in the list (bf_want.tsv, produced by the host planner)
    fv, fmeta = dmsh_fetch.variant(src, refl['bindings'], om_only_ok=key in BF_WANT)
    if not fv:
        for path in (os.path.join(out, key + '_bf.metallib'), os.path.join(out, key + '_bf.metal')) + ((os.path.join(IOSOUT, key + '_bf.metallib'),) if IOSOUT else ()):
            try: os.unlink(path)
            except FileNotFoundError: pass
        write_refl(key, refl)
        return refl
    BF_STATS['generated'] += 1
    fmsl = os.path.join(out, key + '_bf.metal')
    digest = bf_digest(fv, key)
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('variant', digest), threading.Lock())
    with lock:
        try:
            if not bf_cache_restore(key, digest, fmeta, fmsl):
                BF_STATS['miss'] += 1
                open(fmsl, 'w').write(fv)
                fair = fmsl[:-6] + '.air'; flib = os.path.join(out, key + '_bf.metallib') if 'mac' in TARGETS else None
                try:
                    if flib:
                        tmp_flib = flib + '.tmp-' + str(os.getpid())
                        mac_compile(fmsl, fair, tmp_flib)
                        os.replace(tmp_flib, flib)
                    ilib = None
                    if IOSOUT:
                        ilib = os.path.join(IOSOUT, key + '_bf.metallib')
                        ios_metal.compile(fmsl, fmsl[:-6] + '.ios.air', ilib + '.tmp')
                        os.replace(ilib + '.tmp', ilib)
                except subprocess.CalledProcessError:
                    return refl
                BF_STATS['compile'] += 1
                bf_cache_publish(digest, fmsl, flib, ilib, fmeta)
        finally:
            for path in (fmsl, fmsl[:-6] + '.air', fmsl[:-6] + '.ios.air'):
                try: os.unlink(path)
                except FileNotFoundError: pass
    refl['fetch'] = fmeta
    write_refl(key, refl)
    return refl

VIN_STATS = collections.Counter()
PULL_STATS = collections.Counter()
ALPHA_STATS = collections.Counter()
LOGIC_STATS = collections.Counter()

def pull_finish(key, full, refl):
    """The pulling library <key>_bp of a vertex shader with stage inputs (dmsh_pull.py: no [[stage_in]], the inputs read through the per-draw layout block), compiled through the
    MSL-keyed Metal cache with the chain's flags; reflection 'pull' names its entry points, the layout block's buffer index and the inputs. Shaders without stage inputs get none
    (their pipelines have no vertex descriptor anyway), nor does a shader the variant does not fit (stale libraries removed). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    libs = [os.path.join(out, key + '_bp.metallib')] + ([os.path.join(IOSOUT, key + '_bp.metallib')] if IOSOUT else [])
    refl.pop('pull', None)
    def drop():
        for p in libs:
            try: os.unlink(p)
            except FileNotFoundError: pass
    v, info = dmsh_pull.variant(full)
    if not v:
        PULL_STATS['no stage input' if info == 'no stage input' else 'not generated'] += 1
        if info != 'no stage input': print(f'[RED] {key}: no pulling library ({info}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop(); return refl
    msl = os.path.join(w, key + '_bp.metal'); open(msl, 'w').write(v)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', msl, libs[0])
        if IOSOUT: MTL.compile('ios', msl, libs[1])
    except subprocess.CalledProcessError as e:
        PULL_STATS['failed'] += 1; print(f'[RED] {key}: the pulling library does not compile ({str(e)[:120]}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    PULL_STATS['generated'] += 1
    refl['pull'] = {k: v_ for k, v_ in info.items() if v_ is not None}
    return refl

def vin_finish(key, full, refl):
    """The shadow-input library <key>_bv of a vertex shader with integer inputs (dmsh_vin.py), compiled through the MSL-keyed Metal cache; reflection 'vin' names its entry points and
    the shadow attribute indices. Other shaders get none (stale libraries removed). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    libs = [os.path.join(out, key + '_bv.metallib')] + ([os.path.join(IOSOUT, key + '_bv.metallib')] if IOSOUT else [])
    refl.pop('vin', None)
    def drop():
        for p in libs:
            try: os.unlink(p)
            except FileNotFoundError: pass
    v, shadows = dmsh_vin.variant(full)
    if not v:
        if shadows != 'no integer input': VIN_STATS['not generated'] += 1; print(f'[RED] {key}: no shadow-input library ({shadows}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    VIN_STATS['generated'] += 1
    msl = os.path.join(w, key + '_bv.metal'); open(msl, 'w').write(v)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', msl, libs[0])
        if IOSOUT: MTL.compile('ios', msl, libs[1])
    except subprocess.CalledProcessError as e:
        VIN_STATS['failed'] += 1; print(f'[RED] {key}: the shadow-input library does not compile ({str(e)[:120]}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', 'shadow': shadows}
    VIN_STATS['inputs'] += len(shadows); VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
    return refl

def ps_side_finish(key, full, refl, mod, sfx, field, stats, what):
    """A pixel shader's side library <key>_<sfx> (mod.variant: dmsh_alpha.py, dmsh_logic.py), from the source with every entry point, compiled through the MSL-keyed Metal cache;
    reflection <field> names its entry points. A shader the variant does not apply to gets none (stale libraries removed): the host draws it without. Changes refl; the caller writes it"""
    if refl['stage'] != 'ps': return refl
    libs = [os.path.join(out, f'{key}_{sfx}.metallib')] + ([os.path.join(IOSOUT, f'{key}_{sfx}.metallib')] if IOSOUT else [])
    refl.pop(field, None)
    def drop():
        for p in libs:
            try: os.unlink(p)
            except FileNotFoundError: pass
    v, info = mod.variant(full)
    if not v: stats['not generated'] += 1; drop(); return refl
    msl = os.path.join(w, f'{key}_{sfx}.metal'); open(msl, 'w').write(v)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', msl, libs[0])
        if IOSOUT: MTL.compile('ios', msl, libs[1])
    except subprocess.CalledProcessError as e:
        stats['failed'] += 1; print(f'[RED] {key}: the {what} library does not compile ({str(e)[:120]}): its draws go without it', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    stats['generated'] += 1
    refl[field] = {'entries': info}
    return refl

def alpha_finish(key, full, refl):   # the alpha test library <key>_ba (dmsh_alpha.py): none for a shader whose colour 0 has no float alpha
    return ps_side_finish(key, full, refl, dmsh_alpha, 'ba', 'alpha', ALPHA_STATS, 'alpha test')

def logic_finish(key, full, refl):   # the logic operation library <key>_bl (dmsh_logic.py): none for a shader without colour outputs
    return ps_side_finish(key, full, refl, dmsh_logic, 'bl', 'logic', LOGIC_STATS, 'logic operation')

pc_of, loc = {}, {}
MAP_ROWS, MAP_CONFLICT_ROWS = {}, {}
for raw in open(MAP, 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 4: MAP_ROWS.setdefault(c[3].decode(), raw)
for raw in open(MAP + '.conflicts.tsv', 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): MAP_CONFLICT_ROWS.setdefault(c[0].decode(), raw)
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def vertex_ab(key, code, ctl):
    """A vertex shader in two parts, VertexA + VertexB, as one program (Catherine: Full Body's GLSLC, SDK 8.2.1, GPU code version 1.15, writes every vertex shader so; the other games' are one
    program of shader type 1). Part A sits at code +0, its SPH with shader type 0 (Maxwell's VERTEX_CULL_BEFORE_FETCH slot): it loads the position attribute and writes only the position.
    Part B starts at control +0x700 (its own 0x30-byte header, then an SPH of shader type 1) and writes the other outputs from the other attributes. The hardware runs A, then B; translated
    alone, A left every varying unwritten (a full-screen pass read its texture coordinate as 0 and the whole screen came out one texel's colour). Control +0x748 lists both parts as
    {SPH offset, length from the SPH, ?} for A, then for B. The merge is what Ryujinx's TranslatorContext.Translate(VertexA) builds: A's instructions, each unconditional EXIT a branch to
    B's first instruction, then B's, under B's SPH with A's flags and input / output maps ORed in and the larger local memory sizes. Measured on all 1,244 of the game's vertex shaders: A
    writes no generic output (so none of B's loads reads one of A's), A ends in two EXITs (one predicated on CC FCSM_TR, which Ryujinx never takes, left as it is) and a BRA to itself,
    and neither part has an absolute or table jump, so moving B keeps every branch. → (code area with its 0x30 header, c1), c1 being what follows part B; None for any other vertex shader"""
    if len(code) < 0x80 or len(ctl) < 0x760 or code[:4] != bytes.fromhex('78563412'): return None
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f != 1 or (w0 >> 10) & 0xf != 0: return None
    a_sph, a_len, _, b_sph, b_len, _ = struct.unpack_from('<6I', ctl, 0x748)
    if not any((a_sph, a_len, b_sph, b_len)): return None   # one program of shader type 0, no part B: translated as it is (Ryujinx reads type 0 as a vertex shader)
    bw0 =struct.unpack_from('<I', code, b_sph)[0] if 0x80 <= b_sph <= len(code) - 0x50 else 0
    if a_sph != 0x30 or b_sph != struct.unpack_from('<I', ctl, 0x700)[0] + 0x30 or bw0 & 0x1f != 1 or (bw0 >> 10) & 0xf != 1 or (a_len - 0x50) % 32 or (b_len - 0x50) % 32 or b_sph + b_len > len(code):
        sys.exit(f'[RED] {key}: vertex shader of shader type 0 without a VertexB part in the expected place (control +0x748 {a_sph:#x} {a_len:#x} {b_sph:#x} {b_len:#x})')
    sa, sb = list(struct.unpack_from('<20I', code, a_sph)), list(struct.unpack_from('<20I', code, b_sph))
    sph = [sb[0] | sa[0] & ~(0xf << 10)] + [max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffff for i in (1, 2, 3)] + [sb[4]] + [sa[i] | sb[i] for i in range(5, 20)]
    a = bytearray(code[a_sph + 0x50:a_sph + a_len]); b = code[b_sph + 0x50:b_sph + b_len]
    for o in range(0, len(a), 8):
        if o % 32 == 0: continue   # the scheduling word of each group of three instructions
        ins = struct.unpack_from('<Q', a, o)[0]
        if ins >> 52 == 0xe30 and ins & 0x1f == 0xf:   # EXIT with CC T (any predicate) → BRA to part B, keeping the predicate and CC bits; the offset counts from the next instruction
            struct.pack_into('<Q', a, o, 0xe24 << 52 | ((len(a) - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff)
    return code[:0x30] + struct.pack('<20I', *sph) + bytes(a) + b, code[b_sph + b_len:]

def early_fragment_tests(ctl, code):
    """Early fragment tests: control +0x718 of a pixel shader (the NVN driver's control accessor reads that byte beside the other fragment flags at +0x719..+0x71d, the same accessor in
    the sdk of all 21 games here; Ryujinx gets it as the 3D engine's EarlyZForce). On the Switch such a shader is tested against depth before it runs; Metal otherwise tests a shader that
    stores to memory after it, and the late depth differs by an ulp from a prepass's along contours, so an EQUAL pass fails there (MacNativeGames ae8f4f4: Tears of the Kingdom's dotted
    lattice on the Great Sky Island; 1,677 of its 13,646 pixel shaders set it, none writes depth). A shader whose SPH (code, after the 0x30 header) writes depth (word 19 bit 1, OmapDepth)
    is left as it is: tested early, the depth it computes would not be the one tested"""
    return len(ctl) > 0x718 and ctl[0x718] != 0 and len(code) >= 0x50 and not struct.unpack_from('<I', code, 19 * 4)[0] & 2

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]; stage = name[:2]
    extra = {}
    if dmsh_ampere.is_nvn2(rec[ci:]):
        # NVN2 (Switch 2): Ampere code (a 0x80-byte SPH for the graphics stages, then the instructions, control +0x38 long), which the translator's Ampere
        # frontend reads (job arch sm86); its c1 segment lies between control +0x40 and +0x44. The NVN control fields the steps below read (VertexA / B,
        # transform feedback, early fragment tests) are not in an NVN2 section
        c1 = dmsh_ampere.c1_segment(rec[ci:], code)
        code = code[:dmsh_ampere.inst_len(rec[ci:])]
        extra['arch'] = 'sm86'
        # compute lays out its driver constants in c[0] per shader: the control section gives its push constants' words (+0xa9), counts its uniform buffer, storage
        # buffer and texture entries (+0xb1, +0xb2, +0xb3) and gives the size (+0xba); the translator maps them to c[24] and NVN's fixed tables (InstEmitAmpere.cs
        # AMapC0). Graphics keeps NVN's tables
        if stage == 'cs': extra['nvn2_cs'] = [rec[ci + 0xb1], rec[ci + 0xb2], rec[ci + 0xb3], struct.unpack_from('<H', rec, ci + 0xba)[0], rec[ci + 0xa9] * 4]
    else:
        c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)   # control segment +0x700: c1 relative to the start of the instruction area, +0x704: total length of the instruction area (c1 = what lies between the two; lengths across the whole library are 0 / 256 / 512 / 768)
        c1 = rec[0x38 + c1o:0x38 + c1t]
        ab = vertex_ab(key, code, rec[ci:]) if stage == 'vs' else None
        if ab: code, c1 = ab
        if stage != 'cs': code = code[0x30:]   # strip the 0x30 header (the 0x50 SPH after it is read by the translator itself)
        elif code[:4] == bytes.fromhex('79563412'):
            # Compute in this game has a header (magic 0x12345679; D2R's package records have none): 0x30 header + zero padding, instructions start at code area +0x100 (measured on all 71; the BRA-to-self and the control words are 32-byte aligned to this start).
            # Stripping only 0x30 made the translator decode from +0x30: the control word every 32 bytes is off by 0x10 and the jump tables of indirect BRX (relative to the program start) are all misplaced → particle / post-effect compute shaders loop forever and hang the GPU.
            if any(code[0x30:0x100]): sys.exit(f'[RED] {key}: compute code area +0x30..+0x100 is not all zero, so the instructions do not start at +0x100')
            code = code[0x100:]
        tf = tf_layout(key, rec[ci:]) if stage in ('vs', 'gs') else None
        if tf: extra['tf'] = tf
        if stage == 'ps' and early_fragment_tests(rec[ci:], code): extra['early_fragment_tests'] = True
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # translation options (see dmsh_xlate/Program.cs); relaxed precision is on by default (GPU float last-bit error does not enter game state, whole frame −26% on the second Mac)   # c1: used by the translator to read indirect jump tables
    if key not in FACTS: j['no_facts'] = True; return j, c1   # not in the answer table: queries use defaults (listed separately in the summary)
    j.update(FACTS[key])
    return j, c1

TF_SLOT = 8   # Metal vertex buffer index of transform feedback buffer 0 (1-3 follow): slot 0 is the argument buffer, 16-29 the vertex streams, 30 the zero buffer (nvn_mtl.m tf_draw)
def tf_layout(key, ctl):
    """Decode the four feedback streams in the program's control section.
    Each stream has up to 128 output-location bytes and matching component-count
    bytes, captured from x on and interleaved in that order. Pokémon Legends:
    Z-A uses one output per stream, Tropical Freeze two in one stream, and
    Xenoblade Chronicles 3 uses up to eleven.
    Unknown layouts are reported and left without a capture entry."""
    strides = struct.unpack_from('<4H', ctl, 0x6dc)
    if not any(strides): return None
    out = []
    for b in range(4):
        if not strides[b]: out.append([]); continue
        count = ctl[0x6e8 + b]
        locs = list(ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + count])
        sizes = list(ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + count])
        if not 1 <= count <= 128 or any(not 0x20 <= x < 0x40 for x in locs) or any(not 1 <= n <= 4 for n in sizes) or strides[b] != 4 * sum(sizes):
            print(f'[RED] {key}: transform feedback buffer {b} has a layout of unknown form (stride {strides[b]}, outputs {locs}, components {sizes}): no capture entry point; see tf_layout', flush=True)
            return None
        out.append({'stride': strides[b], 'outputs': [[o - 0x20, n] for o, n in zip(locs, sizes)]})
    return out

def vs_tf(src, tf, geometry=False):
    """Capture interleaved output bits, using offsets prepared on the GPU.
    Geometry readers gather the ordered, decomposed primitive indices before
    reading their outputs. Their capture index remains linear, so duplicated
    strip vertices occupy distinct feedback records."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the transform feedback entry point')
    params, body = m.group(1), src[m.end():]   # the main function is the last function in the file
    vm = re.search(r'uint\s+(\w+)\s*\[\[vertex_id\]\]', params); vid = vm.group(1) if vm else 'dm_vid'
    im = re.search(r'uint\s+(\w+)\s*\[\[instance_id\]\]', params); iid = im.group(1) if im else 'dm_iid'
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    names = {int(l): nm for nm, l in re.findall(r'float4 (\w+) \[\[user\(locn(\d+)\)\]\]', st.group(1) if st else '')}
    stores = []
    for b, e in enumerate(tf):
        if not e: continue
        offset = 0
        for o, n in e['outputs']:
            for i, c in enumerate('xyzw'[:n]):
                value = f'as_type<uint>(out.{names[o]}.{c})' if o in names else ('0x3f800000u' if c == 'w' else '0u')
                stores.append(f'dm_tf{b}[dm_tf.offset[{b}] / 4 + dm_linear * {e["stride"] // 4} + {offset + i}] = {value};')
            offset += n
    extra = ('' if vm else ', uint dm_vid [[vertex_id]]') + ('' if im else ', uint dm_iid [[instance_id]]')
    extra += ''.join(f', device uint* dm_tf{b} [[buffer({TF_SLOT + b})]]' for b, e in enumerate(tf) if e)
    extra += ', constant dm_tf_capture& dm_tf [[buffer(14)]]'
    pro = f'uint dm_linear = ({iid} - dm_tf.base_instance) * dm_tf.count + {vid} - dm_tf.first;\n'
    if geometry: pro += f'{vid} = dm_tf.order[dm_linear];\n'
    decl = '\nstruct dm_tf_capture { uint4 offset; uint4 args; device const uint* order; uint first, count, base_instance, pad; ulong reserved; };\n'
    return decl + f'\nvertex void main0_tf({params}{extra})\n{{\n' + pro + body.replace('return out;', '{ if (dm_linear < dm_tf.args.x) { ' + ' '.join(stores) + ' } return; }')


def vs_writes(src, binds):
    """Whether a vertex shader has side effects that a draw with rasterizer discard still runs, so that it needs main0_discard: storage buffer writes (Xenoblade Chronicles 3's
    vertex-only work before its skinned models) or image stores (Metroid Prime Remastered's volumetric fog: 1cbc19f806a08166 fills its 100x57x64 RGBA16F froxel volume from
    5,700 points with rasterization off; without the entry the host skipped the draw, the volume stayed zero and the fog pass blended every scene to black)"""
    return any(b.get('rw') for b in binds) or bool(re.search(r'access::(read_)?write\b', src))


def vs_discard(src):
    """Metal requires a void vertex entry when rasterization is disabled.
    Preserve all original side effects and early returns; omit only outputs."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: raise ValueError('vertex shader has no main0 entry for rasterizer discard')
    return '\nvertex void main0_discard(' + m.group(1) + ')\n{' + src[m.end():].replace('return out;', 'return;')

def raster_facts(stage, src):
    """Static facts needed for the runtime proof that the load can be skipped (full-coverage first draw) (nvn_mtl.m first_draw_covers):
        Vertex: whether position x / y / z are taken unchanged from the .x / .y / .z of one and the same vertex attribute (w is the constant 1 or the same attribute's .w), with no branches and no clip distances in the main function - if so, pos_attr = attribute slot;
        Fragment: whether it may discard (discard_fragment), whether it writes sample_mask, and which color indices it writes. Anything unrecognized is treated as "unprovable" (pos_attr = -1 / discard = 1).
        Also, not for that proof: whether a fragment shader reads the primitive ID, which a specialized Metal 4 pipeline does not give it (nvn_mtl.m pso_build_ex), and a vertex shader's
        clip → screen varyings (screen, dmsh_fetch.py screen_varyings), the vertex side of the same-pixel proof for a projective sample's tile read (nvn_mtl.m fb_tile_plan)"""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in (out.group(1) if out else '')), 'primitive_id': int('[[primitive_id]]' in src),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', out.group(1) if out else '')})}
    if stage != 'vs': return {}
    scr = {'screen': dmsh_fetch.screen_varyings(src)}   # clip → screen varyings (dmsh_fetch.py): the vertex side of a projective sample's tile read
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # the main function is the last function in the file
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1, **scr}
    b = body.group(1)
    attr = {m.group(2): int(m.group(3)) for m in re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1, **scr}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1, **scr}
    # uv pass-through (used for the "this-pixel addressing" proof in merge B step 2): the .x / .y of interpolated output locn K are taken unchanged from the .x / .y of vertex attribute A → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y': uv[str(k)] = attr[sx[0]]; continue
        # Affine: component = fma(attribute component, uniform[slot].vector[i].component, uniform[slot].vector[j].component) (vp_cN is uniform slot N − 3)
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff, **scr}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}: texture slot, sampler slot (Ryujinx TextureHandle encoding; c[2] handle table). Handle type 3 (SeparateConstantSamplerHandle) carries a constant
    sampler ID instead of a second offset, so only the first offset can name the texture (const_sampler gives the ID)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // abi.C2_ROW if cb == 2 and word * 4 >= base and (word * 4 - base) % abi.C2_ROW == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):   # image
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < IMG_BASE + abi.C2_ROW * IMG_SLOTS and slot(x, IMG_BASE, tb) is not None), None), None, ty
    tw = [o0] if ty == 3 else words
    t = next((slot(x, TEX_BASE, tb) for x in tw if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
    if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None) if ty != 3 else None
    return t, s, ty

def const_sampler(name):
    """The constant sampler ID of a type-3 handle (fp_t_cb65538_3041005C: the texture handle at c2 word 0x5C ORed with sampler 0x104), or None"""
    m = re.match(r'[a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    h = int(m.group(2), 16)
    return (h >> 14) & 0x3fff if h >> 28 == 3 else None

def decode_ubo_tex(name):
    """Bindless texture: the handle is written by the game into its own uniform buffer (fp_t_cb7_2A = c[7] word 0x2A). Returns (texture: (uniform slot, byte offset), sampler: (slot, offset));
    handle type 0 = combined handle (one 32-bit word: low 20 bits texture ID, high 12 bits sampler ID), 1 = the sampler handle is in another word (its slot is the high 16 bits of slots − 1). This game's whole library uses only type 0 (1850 shaders)
    Type 3: the sampler is a constant ID (Monster Hunter Rise: a texture handle at c[5] word 0x28 with sampler 0x104); the sampler part is then ('id', ID)"""
    m = re.match(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    tb = slots & 0xffff; sb = (slots >> 16) - 1 if slots >> 16 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if not 3 <= tb <= 16 or ty not in (0, 1, 3) or (ty == 1 and not 3 <= sb <= 16): return None
    return (tb - 3, o0 * 4), (('id', o1) if ty == 3 else (sb - 3, o1 * 4) if ty else (tb - 3, o0 * 4))

def c2_array(name, ty):
    """An array of handles in the driver table c2 that the shader indexes at run time (Ryujinx's texture array from a buffer, here bank 2: <stage>_t<type>_cb<slots>_<handle>, declared
    array<element, n>, one element per c2 handle word; the index is the handle's byte offset ÷ 8). Luigi's Mansion 3: 586 pixel shaders sample their shadow cascades from
    array<texture2d_array<float>, n> fp_t2das_cb2_0, the index (0x80 + a cascade offset) ÷ 8. Handle type 1 ORs one fixed separate sampler word into every element's handle:
    EA SPORTS FC 27's lights, fp_t2d_cb196610_10570000 (texture words from c2 byte 0, the sampler word at c2 byte 0x570), indexed by separate texture i at element 45 + i.
    → (the c2 byte offset of element 0 (element k = the handle at that offset + 8k), the c2 byte offset of the sampler word or None), or None"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name[:-5] if name.endswith('Smplr') else name)
    if not m or not ty.startswith('array<'): return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    if h >> 28 == 0 and slots == 2: return (h & 0x3fff) * 4, None
    if h >> 28 == 1 and slots & 0xffff == 2 and (slots >> 16) - 1 == 2: return (h & 0x3fff) * 4, ((h >> 14) & 0x3fff) * 4
    return None

POOL_MIN = 33   # arrays at least this long are pools: Program.cs sizes Ryujinx's handle fallback by the pools, the vendored BindlessElimination keeps 32-entry slot arrays (NvnSlotArrayLength)

def pool_arrays(src):
    """Ryujinx's arrays for a texture handle it cannot trace to one slot (<stage>_<t|s>_cb0_40000000, indexed by the handle's texture / sampler ID), sized to the whole ID ranges
    (Program.cs QueryTextureArrayLengthFromPool, 2^20 and 2^12) → a pointer to a pool the replay keeps per element type (nvn_mtl.m pool_gpu): struct spvPool_<name> { uint n;
    array<element, 1> e; }, read as (&e[0])[min(index, n)], n being the highest ID the host filled. The struct declares one element: Metal's argument encoder walks every element
    of a declared array (AGX IndirectArgumentLayoutGen2::parseArray), and for 2^20 textures that took 57 ms and 66 MB per encoder (9.7 GB in Mario Party Superstars' prewarm);
    the elements are 8-byte resource IDs, so the pointer reaches the rest (checked on the GPU). Mario Party Superstars' characters read their light textures' handles from a
    storage buffer (6,370 pixel shaders). The argument IDs are then renumbered in member order, a pointer taking one and any other array its length, as the host lays the buffer out.
    Buffer textures and images keep their 32-entry slot arrays (tex_dyn_slots). ValueError on a use of a pool array the rewrite does not cover"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not body: return src
    pools = {}
    def to_pointer(m):
        if not m.group(4).endswith('_cb0_40000000') or int(m.group(3)) < POOL_MIN: return m.group(0)
        pools[m.group(4)] = (m.group(2), int(m.group(3)))
        return f'{m.group(1)}constant spvPool_{m.group(4)}* {m.group(4)} [[id('
    members = re.sub(r'^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(', to_pointer, body.group(1), flags=re.M)
    if not pools: return src
    aid = 0
    def renumber(m):
        nonlocal aid
        a = re.fullmatch(r'array<.+, (\d+)>', m.group(2).strip())
        out = f'{m.group(1)}{m.group(2)} {m.group(3)} [[id({aid}){m.group(5)}]];'
        aid += int(a.group(1)) if a else 1
        return out
    members = re.sub(r'^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];', renumber, members, flags=re.M)
    structs = ''.join(f'struct spvPool_{nm}\n{{\n    uint n;\n    array<{el}, 1> e;\n}};\n\n' for nm, (el, n) in pools.items())
    i = body.start()
    src = src[:i] + structs + src[i:body.start(1)] + members + src[body.end(1):]
    for nm in pools:   # base.name[index] → (&base.name->e[0])[min(uint(index), base.name->n)], the index taken with its brackets balanced
        out, pos = [], 0
        for m in re.finditer(r'([\w.]+)\.%s\[' % re.escape(nm), src):
            if m.start() < pos: continue
            depth, j = 1, m.end()
            while depth:
                if j >= len(src): raise ValueError(f'{nm}: unbalanced index')
                depth += {'[': 1, ']': -1}.get(src[j], 0); j += 1
            base = f'{m.group(1)}.{nm}'
            out.append(src[pos:m.start()] + f'(&{base}->e[0])[min(uint({src[m.end():j - 1]}), {base}->n)]'); pos = j
        src = ''.join(out) + src[pos:]
        left = len(re.findall(r'\b%s\b' % re.escape(nm), src)) - 2 * len(re.findall(r'\.%s->e\[0\]\)\[' % re.escape(nm), src)) - 1   # each read names it twice, the member once (spvPool_<name> is one word)
        if left: raise ValueError(f'{nm}: {left} uses of the pool array the rewrite does not cover')
    return src

def reflect(key, stage, msl, res):
    """MSL argument buffer struct member → replay reflection: kind (same as nvn_refl.h) + NVN slot number + argument buffer member index"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    sb_cb = {b['binding']: b for b in res['sbufs']}
    binds = []
    # A member may carry attributes after its id: SPIRV-Cross marks the resources a fragment shader writes inside its interlock [[id(n), raster_order_group(0)]]
    # (Mario Party Superstars' light_primitive_point_cluster.bnsh, a storage buffer and two images); a member the pattern skipped left the host's argument buffer longer than the reflection
    members = list(re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1) if body else '', re.M))
    if body and len(members) != body.group(1).count('[[id('): raise ValueError(f'{key}: an argument buffer member the reflection cannot read')
    for m in members:
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        pool = re.search(r'struct spvPool_%s\s*\{\s*uint n;\s*array<(.+), \d+> e;' % re.escape(nm), msl) if ty == f'constant spvPool_{nm}*' else None
        if ty == 'sampler' and nm.endswith('Smplr' + abi.BORDER_TWIN):   # a sampler's twin (border_emu): the same sampler, its bit in the block in the order of the twins
            nm = nm[:-len(abi.BORDER_TWIN)]; e['border_bit'] = sum(1 for b in binds if 'border_bit' in b)
        if nm == abi.BORDER_MEMBER: e.update(kind=abi.KIND_BORDER)
        elif nm in dmsh_geometry.HELPERS: e.update(kind=abi.KIND_GEOMETRY, reg=dmsh_geometry.HELPERS[nm])
        elif pool: e.update(kind=abi.KIND_SAMP_POOL if pool.group(1) == 'sampler' else abi.KIND_TEX_POOL, elem=pool.group(1))   # see pool_arrays
        elif nm == 'support_buffer': e.update(kind=abi.KIND_SUPPORT)
        elif re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
            e.update(kind=abi.KIND_CBUF, reg=n - 3) if 3 <= n <= 16 else e.update(kind=abi.KIND_C1 if n == 1 else abi.KIND_DRVCB, bank=n)
        elif re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind=abi.KIND_SBUF, index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
        elif decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm):   # bindless: the handle is in a uniform buffer (reg = uniform slot, off = byte offset)
            ut, us = decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm)
            if nm.endswith('Smplr'): e.update(kind=abi.KIND_SAMP_ID, reg=us[1]) if us[0] == 'id' else e.update(kind=abi.KIND_SAMP_UBO, reg=us[0], off=us[1])
            elif 'texture_buffer' in ty: e.update(kind=abi.KIND_UNKNOWN)   # bindless typed buffer: not implemented in the replay layer
            else: e.update(kind=abi.KIND_TEX_UBO, reg=ut[0], off=ut[1])
        elif c2_array(nm, ty) is not None:   # an array of the driver table's handles (c2_array): the replay fills element k from the slot whose c2 word is at off + 8k
            off, soff = c2_array(nm, ty)   # pyright: ignore[reportGeneralTypeIssues]  (not None: tested above)
            e.update(kind=abi.KIND_SAMP_C2A if nm.endswith('Smplr') else abi.KIND_TEX_C2A, off=off, **({'soff': soff} if soff is not None else {}))
        elif nm.endswith('Smplr'):   # handle type 0 = texture and sampler combined in one handle: the sampler index is taken from the high bits of that texture slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(kind=abi.KIND_SAMP_ID, reg=const_sampler(nm[:-5])) if ty == 3 else e.update(kind=abi.KIND_SAMP, reg=s) if ty else e.update(kind=abi.KIND_SAMP_TEX, reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind=abi.KIND_UNKNOWN)
            else: e.update(kind=abi.KIND_IMG if re.match(r'[a-z]+_i', nm) else (abi.KIND_TBUF if 'texture_buffer' in ty else abi.KIND_TEX), reg=d[0])
            # native texture atomics (dmsh_atomic.py): Metal allows them only on an R32Uint / R32Sint texture made with the shader-atomic usage, which the host checks for this slot
            if e.get('kind') == abi.KIND_IMG and re.search(r'\b%s\.atomic_' % re.escape(nm), msl): e['atomic'] = True
            # a 1D integer fetch the translator read as 2D rows of 16384 texels (Program.cs RecordTexelFetch1D): the host binds a texture buffer in this slot as such rows (nvn_mtl.m tbuf_as_2d)
            if e.get('kind') == abi.KIND_TEX and e.get('reg') in res.get('fetch1d', ()):
                e['fetch1d'] = True
                if re.search(r'\b%s\.get_width\(' % re.escape(nm), msl): e['fetch1d_size'] = True   # also size-queried: a texture buffer of more than 16384 elements reports 16384 there
        binds.append(e)
    # Storage buffers: SPIR-V binding number → c0 offset → NVN storage buffer slot (c0 base SSBO[stage], C0_ROW bytes per slot)
    base = SSBO.get(stage)   # storage buffer base for the hull / domain stages has not been measured: a storage buffer there is reported with no slot
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == abi.KIND_SBUF:
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT:   # not a game buffer: for addresses traced to no storage buffer, the host's pool table (offset 0) or the write log (DMSH_GMEM_LOG_CB_OFFSET)
                if b['cb_offset'] == abi.GMEM_LOG_CB_OFFSET: e.update(kind=abi.KIND_GMEM_LOG, rw=True); continue
                # rw: one of those accesses writes (the translator notes each one with its operation: Load, Store or an atomic)
                e.update(kind=abi.KIND_GMEM_MAP, rw=any(re.search(r'operation "(Store|Atomic\w*)" traced to no storage buffer', n) for n in res.get('notes', ()))); continue
            if stage == 'cs' and off is not None and abi.C0_CS_UBO <= off < abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS and (off - abi.C0_CS_UBO) % abi.C0_ROW == 0:
                e.update(kind=abi.KIND_UBO_PTR, reg=(off - abi.C0_CS_UBO) // abi.C0_ROW)   # compute-stage uniform slot r: address at c0[C0_CS_UBO + 0x10 r], read through global memory (checked against cs_lut_combine_c reading cb10[66])
            elif b and b['cb_slot'] >= 3:   # the address is in the game's own uniform buffer c[n] = uniform slot n - 3 (Monster Hunter Rise's compute shaders: c3 + 0x38); the replay reads it there
                e.update(kind=abi.KIND_SBUF_UBO, reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
            else: e['reg'] = (off - base) // abi.C0_ROW if off is not None and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else None
            e['rw'] = bool(b and b['flags'] & 1)   # BufferUsageFlags.Write; Program.cs serializes the translator enum unchanged (atomics count as writes too)
    return binds

DRV_FILL = {0: [(b, b + abi.C0_ROW * abi.C0_SSBO_SLOTS) for b in sorted(SSBO.values())],   # c0: storage buffer address tables (vertex / compute / geometry / pixel)
            2: [(TEX_BASE, SEPTEX_BASE), (IMG_BASE, IMG_BASE + abi.C2_ROW * IMG_SLOTS), (SEPTEX_BASE, SAMP_BASE), (SAMP_BASE, SAMP_BASE + abi.C2_ROW * abi.C2_SEP_SLOTS)]}   # c2: the handle tables above, all filled by the replay layer

def drv_reads(src):
    """Reads of driver segments c0 / c2 in the MSL: constant index → byte range; non-constant indices are recorded as dynamic"""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16)
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

SAMPLE_ROWS = [(abi.C0_SAMPLE_TABLE, abi.C0_SAMPLE_TABLE + abi.C0_SAMPLE_BYTES), (abi.C0_SAMPLE_INFO, abi.C0_SAMPLE_INFO + abi.C0_ROW)]   # c0: the pixel stage's sample position rows

def unfillable(dr, stage):
    bad = []
    for bank, d in dr.items():
        fill = DRV_FILL[int(bank)] + (SAMPLE_ROWS if bank == '0' and stage == 'ps' else [])
        # a computed index into c2 reads one of its handle tables, which the replay layer fills whole (nvn_mtl.m DRV2: EA SPORTS FC 27's lights read the separate texture words
        # at 0x168 + 8i to test each light's handle); c0 is filled only in its tables and words above, so a computed index there may read what the replay does not write.
        # The one computed c0 index known is GLSLC's sample position lookup in a pixel shader: a byte of the table at 0x620, at an index made of the sample index (PIXLD.MYINDEX)
        # and the pixel's place in the location grid masked by the words of the 0x630 row, at most 15 (Metroid Prime Remastered: 30 of its 438 shaders that read the sample index)
        if d['dynamic'] and bank == '0' and not (stage == 'ps' and abi.C0_SAMPLE_INFO in d['vec4_offsets']): bad.append(f'c{bank} dynamic index')
        for o in d['vec4_offsets']:
            # a read is a 16-byte row: overlapping any table is enough (Engage reads the c2+0x160 row for texture slot 0 at 0x168 in it)
            if not any(o < b and a < o + 16 for a, b in fill): bad.append(f'c{bank}+{o:#x}')
    return bad

RULES = []   # MSL rewrite rules attached by shader name (function, applicability test); the former dark-range remap toe has been removed (back to the original Switch brightness)
# CODE_FP hashes the code form of this file and its helper modules (dmsh_chain.code_form): editing a comment, a docstring or the layout does not change any shader's key.
# The library ABI enters as its values (dmsh_abi.VALUES), so a comment in dmsh_abi.h does not change a key either.
# The first-stage code fingerprint does not include dmsh_fetch.py; readable-tile variants get a separate second-stage cache keyed on the generated MSL.
CODE_FP = hashlib.sha256(source_without_rule_bodies() + b''.join(b'\0' + dmsh_chain.code_form(open(m.__file__, 'r').read()) for m in (dmsh_chain, dmsh_geometry, dmsh_atomic, abi) + ((ios_metal,) if IOSOUT else ()))
                         + b'\0abi\0' + repr(sorted(abi.VALUES.items())).encode()).hexdigest()
TOOL_FP = tool_fingerprint().encode()

_CAST = ('as_type<int>(', 'as_type<uint>(')
def int_inputs(src):
    """Inputs that a vertex shader only uses as integers (under a float declaration every use is an as_type<int/uint> bit cast, or a plain copy) → {location: 'uint'}.
    Maxwell does no type conversion when reading vertex attribute registers: the bit pattern of integer formats (bone indices R8UI / RG8UI / RGBA8UI etc.) goes into the register as is and the shader uses it as an integer.
    The answer table has no attribute types (the machine code does not say them), so the translator's attribute type queries all defaulted to float: Metal converts the integer format to a float value, and after the bit cast it is garbage - every bone index is wrong and skinned models (the characters) are stretched into streaks or thrown off screen."""
    ins = {m.group(2): int(m.group(3)) for m in re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src)}
    i = src.find('vertex main0_out main0(')
    if i < 0: return {}
    body = src[i:]; out = {}
    for name, loc_ in ins.items():
        uses = list(re.finditer(r'in\.%s\b' % re.escape(name), body))
        nint = 0; bad = False
        for u in uses:
            ls = body.rfind('\n', 0, u.start()) + 1; le = body.find('\n', u.end())
            m = re.match(r'\s*float\s+(_\w+)\s*=\s*in\.%s\.[xyzw];$' % re.escape(name), body[ls:le])
            if not m:
                if body[:u.start()].endswith(_CAST): nint += 1; continue
                bad = True; break
            v = m.group(1)
            for o in list(re.finditer(r'\b%s\b' % re.escape(v), body))[1:]:
                if body[:o.start()].endswith(_CAST): nint += 1; continue
                l2s = body.rfind('\n', 0, o.start()) + 1; l2e = body.find('\n', o.end())
                if re.match(r'\s*[\w.\[\]]+\s*=\s*%s;$' % re.escape(v), body[l2s:l2e]): continue   # plain copy: the bit pattern is the same under either declaration
                bad = True; break
            if bad: break
        if not bad and nint: out[str(loc_)] = 'uint'
    return out

# Both pair tables feed pipelines, so both must feed padding, build order and shader cache keys.
def load_pairs(game, package):
    pairs = collections.defaultdict(set)
    for table in ('shader_pairs.tsv', 'pso_pairs.tsv'):
        path = str(game.table(table))
        if table == 'pso_pairs.tsv' and not os.path.isfile(path):
            path = os.path.join(os.path.dirname(os.path.abspath(package)), table)   # the split's copy for profiles predating this table
        if table == 'pso_pairs.tsv' and not os.path.isfile(path): continue
        with open(path) as f:
            for ln in f:
                if ln.strip() and not ln.startswith('#'): v, p = ln.split(); pairs[v].add(p)
    return pairs

PAIRS = load_pairs(GAME, dm_dir)

def build_keys(want, locations, pairs):
    """A partial vertex build needs its available paired pixel shaders before padding; a full build keeps every shader."""
    return sorted(set(want) | {ps for vs in want for ps in pairs.get(vs, ()) if ps in locations}) if want else sorted(locations)

def vs_pad(key, src):
    """Give a vertex shader the outputs its paired pixel shaders read but it does not write, with the value (0,0,0,1).
    Maxwell attributes are untyped 32-bit slots, and a pixel shader reading a location the vertex shader did not write only gets a default; Ryujinx also fills them with (0,0,0,1) when it links stages. Here every shader is translated
    offline on its own without knowing what the next stage reads, and Metal requires every pixel shader input to be a vertex output: 331 of 9043 pairs failed to link and their draws were skipped
    (over 10,000 "Fragment input(s) user(locnN) ... not written by vertex shader" in one iPhone session after the opening course). Returns (new source, padded locations); pairs whose pixel MSL cannot be read are skipped."""
    want = set()
    for ps in PAIRS.get(key, ()):
        fn = os.path.join(out, ps + '_b.metal')
        if os.path.exists(fn):
            with open(fn) as f: want |= {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', f.read())}
    miss = sorted(want - {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', src)})
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if not miss or i < 0 or j < 0 or k < 0: return src, []
    src = src[:j] + ''.join(f'\n    float4 dm_pad{l} [[user(locn{l})]];' for l in miss) + src[j:]
    k = src.find('main0_out out = {};') + len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.dm_pad{l} = float4(0.0, 0.0, 0.0, 1.0);' for l in miss) + src[k:], miss

def vs_init(src):
    """Every vertex shader output starts as (0,0,0,1) before the translated body runs: on the hardware that is what an unwritten component reads. The translator's source starts with out = {}, all zeros;
    Ryujinx initializes the components the next stage reads this way when it links stages, and since every shader is translated offline on its own without knowing what the next stage reads, all are
    initialized (components the body writes are overwritten as before). An output written only in xyz (a world position) is divided by its w in the pixel shader: with w = 0 every shadow-map
    compare value became infinite and Link's Awakening was in shadow everywhere outdoors (dim, no shadows from the fences)."""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if i < 0 or j < 0 or k < 0: return src
    outs = re.findall(r'\n    float4 (\w+) \[\[user\(locn\d+\)\]\];', src[i:j])
    k += len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.{n} = float4(0.0, 0.0, 0.0, 1.0);' for n in outs) + src[k:]

VP_BASE_INSTANCE = '(*spvDescriptorSet0.vp_c0)._m0[100].y'   # c0 + 0x644, the driver's base-instance word (row 100, second component)

def vs_base_instance(src):
    """The draw's base instance from Metal, not from the driver segment. GLSLC adds c0 + 0x644 to Maxwell's instance ID where a shader needs the absolute instance (gl_InstanceID +
    gl_BaseInstance); the host writes that word from the draw command (nvn_mtl.m encode_args), but an indirect draw's base instance is in its argument record, which the GPU may write
    earlier in the same submission. Metal passes every draw's base instance, direct or indirect, as [[base_instance]], so a vertex function reads it there. Super Mario Party Jamboree's
    GPU-driven instances (276504dc5c987ecf writes the records, 4b05556737e9535f draws them: record = instance + c0 + 0x644, 80 bytes each) all took the first records' transforms
    with 0 there, and stray grey triangles crossed the board. The segment's rows are float4 holding raw bits, so the word becomes the float with the base instance's bits; a function without
    the parameter gets it"""
    if VP_BASE_INSTANCE not in src: return src
    src = src.replace(VP_BASE_INSTANCE, 'as_type<float>(gl_BaseInstance)')
    def add(m):
        return m.group(0) if '[[base_instance]]' in m.group(0) else m.group(0)[:-1] + ', uint gl_BaseInstance [[base_instance]])'
    return re.sub(r'\bvertex\s+\w+\s+main0\s*\((?:[^()]|\([^()]*\))*\)', add, src)   # the parameter list, whose attributes hold parentheses ([[buffer(0)]])

def vs_layer(src):
    """A vertex shader that writes the layer (render_target_array_index) loses the point size output. The translator writes gl_PointSize = 1.0 in every vertex shader; Metal builds a pipeline
    whose vertex function writes the layer only with an input primitive class, and one that writes a point size only with the point class (or none), so with both no pipeline can be built.
    Pokémon Legends: Z-A's layered full-screen passes (57246f9a04af8529, b29f08619f379a83: a triangle per instance, layer = instance + offset) draw triangles, where the point size is unused;
    the host gives such pipelines the triangle class (nvn_mtl.m pso_get_). Shaders that do not write the layer are unchanged"""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i)
    if i < 0 or j < 0 or '[[render_target_array_index]]' not in src[i:j] or '[[point_size]]' not in src[i:j]: return src
    src = src[:i] + re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', src[i:j], count=1) + src[j:]
    return re.sub(r'\n    out\.gl_PointSize = [^;\n]*;', '', src)

def gs_slice(spv):
    """A geometry shader that only picks the layer: triangles in, at most 3 vertices out, each vertex copying its input position and the input components it passes on, and gl_Layer taking the
    bits of one input component (Unreal Engine 4.18's FWriteToSliceGS, which writes one slice of a volume texture per instance: DRAGON QUEST XI S 0e7f58cd1f602f6d builds the 32x32x32 colour
    grading LUT the tone mapper reads, and without it the screen came out a flat blue). Metal has no geometry stage, so the host drops such a shader and draws with the vertex shader's
    layer entry points instead (vs_layer_entry), which write that component as the layer. Read from the SPIR-V as GLSL (SPIRV-Cross writes no MSL for a geometry stage).
    → {'loc': input location, 'comp': component} or None (any other geometry shader: one-pass cube shadows emit six transformed triangles)"""
    r = subprocess.run([SPIRV_CROSS, spv, '--version', '450'], capture_output=True, text=True)
    g = r.stdout
    if r.returncode or 'layout(triangles) in;' not in g or 'layout(max_vertices = 3, triangle_strip) out;' not in g or g.count('EmitVertex();') != 3 or 'EndPrimitive' in g: return None
    ins = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) in vec4 (\w+)\[3\];', g)}
    outs = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) out vec4 (\w+);', g)}
    body = g[g.find('void main()'):]
    var, layer = {}, set()
    for st in (s.strip() for s in re.split(r'[;{}]', body[body.find('{') + 1:])):
        if not st or st == 'if (false)' or re.fullmatch(r'int \w+ = -?\d+|\w+ = int\(0x80000000\)|int \w+ = \w+|EmitVertex\(\)', st): continue
        m = re.fullmatch(r'float (\w+) = (\w+)\[\w+\]\.([xyzw])', st)
        if m and m.group(2) in ins: var[m.group(1)] = (ins[m.group(2)], m.group(3)); continue
        m = re.fullmatch(r'float (\w+) = gl_in\[\w+\]\.gl_Position\.([xyzw])', st)
        if m: var[m.group(1)] = ('pos', m.group(2)); continue
        m = re.fullmatch(r'gl_Position\.([xyzw]) = (\w+)', st)
        if m and var.get(m.group(2)) == ('pos', m.group(1)): continue
        m = re.fullmatch(r'(\w+)\.([xyzw]) = (\w+)', st)
        if m and m.group(1) in outs and var.get(m.group(3)) == (outs[m.group(1)], m.group(2)): continue
        m = re.fullmatch(r'gl_Layer = floatBitsToInt\((\w+)\)', st)
        if m and m.group(1) in var and var[m.group(1)][0] != 'pos': layer.add(var[m.group(1)]); continue
        return None   # anything else: not a plain slice writer
    if len(layer) != 1 or g.count('gl_Layer =') != 3: return None
    (loc, comp), = layer
    return {'loc': loc, 'comp': comp}

def vs_layer_entry(full):
    """Layer entry points for a vertex shader that writes integer bits taken from the instance index into an output component (Unreal Engine 4.18's WriteToSliceMainVS: the slice = instance −
    base instance + its first slice, DRAGON QUEST XI S 7ceb7898171a9ae9). main0_l and main0_lz are main0 and main0_z writing that component as the layer (render_target_array_index), without
    the point size (see vs_layer); the host draws with them when the program's geometry shader is a slice writer reading that component (gs_slice). → (source with the two entry points added,
    {'loc', 'comp'}), or (source, None) when no output qualifies"""
    i = full.find('struct main0_out\n{\n'); j = full.find('\n};', i)
    if i < 0 or j < 0 or 'gl_InstanceIndex' not in full or '[[render_target_array_index]]' in full[i:j]: return full, None
    locs = {m.group(1): int(m.group(2)) for m in re.finditer(r'\n    float4 (\w+) \[\[user\(locn(\d+)\)\]\];', full[i:j])}
    m = next((m for m in re.finditer(r'\n    out\.(\w+)\.([xyzw]) = as_type<float>\(', full) if m.group(1) in locs), None)
    if not m: return full, None
    name, comp = m.group(1), m.group(2)
    st = re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', full[i:j + 3]).replace('struct main0_out', 'struct main0l_out', 1)
    st = st[:-3] + '\n    uint gl_Layer [[render_target_array_index]];\n};'
    add = '\n' + st + '\n'
    for ep, new in (('main0', 'main0_l'), ('main0_z', 'main0_lz')):
        a = re.search(rf'\bvertex\s+main0_out\s+{ep}\s*\(', full)
        if not a: return full, None
        b, depth = full.find('{', a.end()), 0
        for e in range(b, len(full)):
            depth += full[e] == '{'; depth -= full[e] == '}'
            if not depth: break
        fn = full[a.start():e + 1].replace('main0_out', 'main0l_out').replace(f' {ep}(', f' {new}(', 1)
        fn = re.sub(r'\n    out\.gl_PointSize = [^;\n]*;', '', fn)
        add += '\n' + fn.replace('return out;', f'out.gl_Layer = as_type<uint>(out.{name}.{comp});\n    return out;') + '\n'
    return full + add, {'loc': locs[name], 'comp': comp}

def vs_depth(src):
    """Two entry points for the depth convention: the translator's vertex shader leaves z unchanged (NVN's near=0 convention); the main function is copied here - main0 gets the GL convention's (near=-w) z' = z/2 + w/2 before every return out
    (bit-identical to the formula Ryujinx used to emit in the translator), main0_z stays as is. The convention a game sets is runtime state (nvnDeviceSetDepthMode); the host picks the entry point by it, with nothing to configure per game.
    Metal function constants could do this too, but a function with function constants must always be specialized before a pipeline can be built (measured about 2.4 ms per vertex shader); two entry points only compile the vertex shaders twice (about 30% of the library).
    Returns (the source with only main0: reflection / raster facts are computed from it as before, the full source written into the metallib)."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\(', src)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the depth entry points')
    body = src[m.start():]   # the main function is the last function in the file
    gl = body.replace('return out;', 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;')
    single = src[:m.start()] + gl
    return single, single + '\n' + re.sub(r'\bmain0\s*\(', 'main0_z(', body, count=1)

def fs_uint(src):
    """Integer colour targets. With no PC version the translator declares every fragment output float; a shader writes an integer as the float with its bits (as_type<float>(…)), and Metal
    refuses a pipeline whose float output meets an integer attachment. Luigi's Mansion 3 draws its whole scene into an R32Uint + RGBA8Uint G-buffer (1,377 of its 4,307 pixel shaders write
    an output from integer bits): a hand-written row per shader (shader_facts_manual.tsv) does not scale to that. A pixel shader that writes an output from integer bits gets a second entry
    point, main0_u, the same body returning every colour output as the unsigned integer vector of the same bits (as_type, nothing converted); the host takes it for a pipeline whose colour
    attachments are all unsigned integer formats (nvn_mtl.m).
    A G-buffer may mix the two: Pokémon Sword's scene shaders write colour 0 and 2 as floats and colour 1 from integer bits into an RGBA16Uint target. A shader whose outputs include both
    gets a third entry point, main0_m, that returns only the outputs written from integer bits as unsigned integers (their colour indices: the mask); the host takes it for a pipeline whose
    integer attachments are exactly those. → (the source with main0_u (and main0_m) added, the main0_m mask or 0), or (None, 0)"""
    m = re.search(r'^.*\bfragment\s+main0_out\s+main0\s*\(', src, re.M); st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not m or not st: return None, 0
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    cols = {nm: ty for ty, nm, at in mem if 'color(' in at and ty.startswith('float')}
    # The hardware writes an output's bits to an integer target whatever the shader computed, so main0_u is right for any pixel shader; only those that look like integer writers get one
    # (the library stays small): an output from integer bits, or nothing but constants (a clear drawn into an integer target, 35e1865e313f0553 writes 0 into Luigi's Mansion 3's R32Uint target)
    from_int = {nm for nm in cols if re.search(r'\bout\.%s(?:\.\w+)? = as_type<float\d?>\(' % nm, src)}
    writes = [m.group(1) for nm in cols for m in re.finditer(r'\bout\.%s(?:\.\w+)? = ([^;]+);' % nm, src)]
    if not from_int and not (writes and all(re.fullmatch(r'-?[0-9.]+f?|float\d\([-0-9., ]*\)', w.strip()) for w in writes)): return None, 0
    uty = lambda ty: 'u' + ty.replace('float', 'int')   # float4 → uint4, float → uint
    def entry(sfx, as_uint):   # main0 again as main0_<sfx>, returning the outputs in as_uint as the unsigned integer vectors of their bits (dmsh_fetch.py cuts the source at main0_out_u)
        struct = f'struct main0_out_{sfx}\n{{\n' + ''.join(f'    {uty(cols[nm]) if nm in as_uint else ty} {nm} {at};\n' for ty, nm, at in mem) + '};\n\n'
        ret = f'return main0_out_{sfx}{{' + ', '.join(f'as_type<{uty(cols[nm])}>(out.{nm})' if nm in as_uint else f'out.{nm}' for _, nm, _ in mem) + '};'
        return '\n' + struct + re.sub(r'\bfragment\s+main0_out\s+main0\s*\(', f'fragment main0_out_{sfx} main0_{sfx}(', src[m.start():], count=1).replace('return out;', ret)
    out, mask = src + entry('u', set(cols)), 0
    if from_int and set(cols) - from_int:   # some outputs from integer bits, some not: the mixed entry point
        out += entry('m', from_int)
        mask = sum(1 << int(re.search(r'color\((\d+)\)', at).group(1)) for _, nm, at in mem if nm in from_int)
    return out, mask

def fs_dual(src):
    """Dual-source blending. NVN's SRC1 blend factors read the pixel shader's output 1 as target 0's second source (Maxwell feeds MRT 1 to it), and the translator declares that
    output [[color(1)]]; Metal reads the second source only from [[color(0), index(1)]], and its compiler crashes building a pipeline that blends with an absent one (Pokémon Legends:
    Arceus's ha_post_effects, f4f36a03b0c6691d). A shader in the profile's dual_want.tsv that writes exactly colours 0 and 1 gets a second entry point, main0_d, the same body with
    output 1 as index 1 of colour 0; the host takes it for a pipeline whose target 0 blends with a SRC1 factor (nvn_mtl.m pso_desc). Not every such shader gets one: most write a
    second render target (5,474 of Arceus's 6,156 pixel shaders write exactly colours 0 and 1). → main0_d, to append to the source (with its other entry points), or None"""
    m = re.search(r'^.*\bfragment\s+main0_out\s+main0\s*\(', src, re.M); st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not m or not st: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    if sorted(at for _, _, at in mem if 'color(' in at) != ['[[color(0)]]', '[[color(1)]]']: return None
    struct = 'struct main0_out_d\n{\n' + ''.join(f'    {ty} {nm} {"[[color(0), index(1)]]" if at == "[[color(1)]]" else at};\n' for ty, nm, at in mem) + '};\n\n'
    ret = 'return main0_out_d{' + ', '.join(f'out.{nm}' for _, nm, _ in mem) + '};'
    return '\n' + struct + re.sub(r'\bfragment\s+main0_out\s+main0\s*\(', 'fragment main0_out_d main0_d(', src[m.start():], count=1).replace('return out;', ret)

SG_OPS = {333: 'true', 334: 'pred', 335: 'pred', 336: 'true', 337: 'value', 338: 'value', 339: 'ballot', 345: 'value', 346: 'value', 347: 'value', 348: 'value'}   # OpGroupNonUniform* → its one-lane value
SG_BUILTINS = {36: 'one', 41: 'zero', 4416: 'one4', 4417: 'one4', 4418: 'zero4', 4419: 'one4', 4420: 'zero4'}   # SubgroupSize, SubgroupLocalInvocationId, Eq / Ge / Gt / Le / Lt masks
def spv_single_lane(path):
    """Metal has no SIMD-group functions in vertex functions, and SPIRV-Cross's subgroup emulation covers only Elect. Monster Hunter Rise's vertex shaders (about 4%) scalarize uniform loads:
    ballot the active lanes, shuffle the first lane's value to all, and take a fast path when all agree. With a subgroup of one lane that is exact: the ballot is the lane's own bit, shuffles and
    broadcasts return the lane's own value, all / any are the predicate, all-equal and elect are true, the lane ID is 0, the size 1 and the masks follow. The SPIR-V is rewritten that way
    (the builtin variables, their decorations and the subgroup capabilities removed). → True when rewritten, False when there is nothing to do or an operation it does not cover"""
    with open(path, 'rb') as f: d = f.read()
    words = list(struct.unpack(f'<{len(d) // 4}I', d)); hdr, ins, i = words[:5], [], 5
    while i < len(words): n = words[i] >> 16; ins.append(words[i:i + n]); i += n
    op = lambda x: x[0] & 0xffff
    if not any(op(x) in SG_OPS or (op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS) for x in ins): return False
    if any(340 <= op(x) <= 366 and op(x) not in SG_OPS for x in ins): return False   # other subgroup operations (bit counts, arithmetic): not covered
    bvar = {x[1]: SG_BUILTINS[x[3]] for x in ins if op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS}
    # A mask component can be loaded through an input pointer (Xenoblade Chronicles 3's prefix ballot), rather than loading the whole vector first. Keep its index and extract
    # from the constant vector at the load; removing only the builtin variable would leave a dangling pointer. Other access-chain forms remain unsupported.
    bptr = {}
    for x in ins:
        if op(x) in (65, 66) and len(x) > 3 and x[3] in bvar:
            if len(x) != 5 or not bvar[x[3]].endswith('4'): return False
            bptr[x[2]] = (bvar[x[3]], x[4])
    if any(op(x) in (65, 66) and len(x) > 3 and x[3] in bptr for x in ins): return False
    uint = next((x[1] for x in ins if op(x) == 21 and x[2:4] == [32, 0]), None); boolt = next((x[1] for x in ins if op(x) == 20), None)
    uvec4 = next((x[1] for x in ins if op(x) == 23 and x[2] == uint and x[3] == 4), None)
    if uint is None or boolt is None or uvec4 is None: return False
    bound = hdr[3]; c = {}
    for nm in ('zero', 'one', 'zero4', 'one4', 'true'): c[nm] = bound; bound += 1
    consts = [[43 | 4 << 16, uint, c['zero'], 0], [43 | 4 << 16, uint, c['one'], 1], [44 | 7 << 16, uvec4, c['zero4'], c['zero'], c['zero'], c['zero'], c['zero']],
              [44 | 7 << 16, uvec4, c['one4'], c['one'], c['zero'], c['zero'], c['zero']], [41 | 3 << 16, boolt, c['true']]]
    out, placed = [], False
    for x in ins:
        o = op(x)
        if o == 17 and x[1] in (61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431): continue   # GroupNonUniform* / SubgroupBallotKHR / SubgroupVoteKHR capabilities
        if o == 10 and (b'ballot' in struct.pack(f'<{len(x) - 1}I', *x[1:]) or b'subgroup' in struct.pack(f'<{len(x) - 1}I', *x[1:])): continue
        if o in (71, 5) and x[1] in bvar: continue   # the builtin's decoration and name
        if o == 59 and x[2] in bvar: continue        # the builtin variable itself
        if o == 15: x = _ep_drop(x, bvar)
        if o == 54 and not placed: out += consts; placed = True
        if o == 61 and x[3] in bvar: out.append([83 | 4 << 16, x[1], x[2], c[bvar[x[3]]]]); continue   # a load of a builtin → its constant
        if o in (65, 66) and x[2] in bptr: continue
        if o == 61 and x[3] in bptr:
            nm, index = bptr[x[3]]
            out.append([77 | 5 << 16, x[1], x[2], c[nm], index]); continue   # OpVectorExtractDynamic, including a constant index
        if o in SG_OPS:
            kind = SG_OPS[o]
            if kind == 'ballot': out.append([169 | 6 << 16, x[1], x[2], x[4], c['one4'], c['zero4']]); continue   # OpSelect(pred, own bit, 0)
            out.append([83 | 4 << 16, x[1], x[2], c['true'] if kind == 'true' else x[4]]); continue
        out.append(x)
    if not placed: return False
    hdr[3] = bound
    flat = hdr + [v for x in out for v in x]
    with open(path, 'wb') as f: f.write(struct.pack(f'<{len(flat)}I', *flat))
    return True
def _ep_drop(x, ids):   # OpEntryPoint without the given interface IDs: model, function, the name (a nul-terminated string in whole words), then the interface list
    k = 3
    while k < len(x):
        k += 1
        if any((x[k - 1] >> (8 * b)) & 0xff == 0 for b in range(4)): break   # the word holding the string's nul ends the name
    y = x[:k] + [v for v in x[k:] if v not in ids]
    y[0] = len(y) << 16 | (x[0] & 0xffff)
    return y

def geometry_finish(key, w, j, c1):
    """Build all five input-topology variants; the draw selects one at replay.
    Resources retain logical NVN stage 2 even though Metal executes a kernel.
    """
    code = open(j['code'], 'rb').read()
    dmsh_geometry.output_layout(code, 0)   # validate before running the translator
    jobs = [{**j, 'key': key + '_g' + str(t), 'geometry_topology': t} for t in range(5)]
    jp = os.path.join(w, key + '_geometry.json'); atomic_json(jp, jobs)
    rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
    if rx.returncode: return 'geometry failed', rx.stderr.strip()[:160], None
    base = None
    for t, job in enumerate(jobs):
        layout = dmsh_geometry.output_layout(code, t)
        k = job['key']; spv = os.path.join(w, k + '.spv')
        dmsh_geometry.flatten_sets(spv)
        r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, spv), spv], capture_output=True, text=True)
        if r.returncode: return 'geometry MSL failed', r.stderr.strip()[:160], None
        plain = dmsh_geometry.topology_pointer(pool_arrays(precise(dmsh_atomic.check_msl(r.stdout))))
        src = dmsh_geometry.capture_input(plain)
        res = json.load(open(os.path.join(w, k + '.res.json')))
        refl = {'key': key + '_bg' + str(t), 'route': 'B', 'chain': CHAIN, 'dm_ctl': key,
                'source': f'{loc[key][0]}#{loc[key][1]}', 'pc_paired': pc_of[key], 'stage': 'gs',
                'entry': 'main0', 'msl_entry_kind': 'kernel', 'threadgroup_size': [32, 1, 1],
                'geometry': dict(layout, stride=res['geometry_stride']),
                'c1': c1.hex(), 'notes': res['notes'], 'drv_reads': drv_reads(src), 'drv_unfillable': [],
                'bindings': reflect(key, 'gs', src, res), 'vertex_inputs': [], 'raster': {}}
        def save(suffix, text, meta):
            path = os.path.join(out, key + suffix + '.metal'); open(path, 'w').write(text)
            if 'mac' in TARGETS: MTL.compile('mac', path, os.path.join(out, key + suffix + '.metallib'))
            atomic_json(os.path.join(out, key + suffix + '.json'), MTL.with_compiler('mac', meta))
            if IOSOUT:
                MTL.compile('ios', path, os.path.join(IOSOUT, key + suffix + '.metallib'))
                atomic_json(os.path.join(IOSOUT, key + suffix + '.json'), MTL.with_compiler('ios', meta))
        save('_bg' + str(t), src, refl)
        # Passthrough input without adjacency: a lean kernel the host can pair with the game's own vertex shader (dmsh_geometry.lean)
        if dmsh_geometry.passthrough(code) and t in (0, 1, 3):
            try: lean, reads = dmsh_geometry.lean(plain, res['geometry_stride'])
            except ValueError as e: lean = None; refl['notes'] = refl['notes'] + [str(e)]
            if lean:
                save('_bl' + str(t), lean, {**refl, 'key': key + '_bl' + str(t), 'geometry': dict(layout, stride=1, lean=True, capture_reads=reads),
                                            'drv_reads': drv_reads(lean), 'bindings': reflect(key, 'gs', lean, res)})
        if t == 0:
            base = {**refl, 'key': key + '_b'}
            save('_b', src, base)
            spv = os.path.join(w, k + '.reader.spv'); dmsh_geometry.flatten_sets(spv)
            r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, spv), spv], capture_output=True, text=True)
            if r.returncode: return 'geometry reader failed', r.stderr.strip()[:160], None
            reader = vs_layer(vs_init(dmsh_geometry.reader(pool_arrays(precise(dmsh_atomic.check_msl(r.stdout))))))
            analysis, full = vs_depth(reader)
            meta = {'key': key + '_bgv', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key,
                    'stage': 'vs', 'entry': 'main0', 'entry_z': 'main0_z', 'msl_entry_kind': 'vertex',
                    'geometry_reader': True, 'geometry_output': layout['output'], 'c1': '', 'notes': [], 'drv_reads': {}, 'drv_unfillable': [],
                    'bindings': reflect(key, 'vs', analysis, {'sbufs': []}), 'vertex_inputs': [], 'raster': {}}
            if j.get('tf'):
                full += vs_tf(analysis, j['tf'], geometry=True)
                meta.update(entry_tf='main0_tf', tf_strides=[e['stride'] if e else 0 for e in j['tf']])
            save('_bgv', full, meta)
    return 'ok', '', base


def finish(key, w, j, c1):
    name, k = loc[key]
    msl = os.path.join(w, key + '.metal'); lib = os.path.join(out, key + '_b.metallib')
    if j['stage'] == 'gs':   # keep the layer-only fold; other geometry runs as compute
        s = gs_slice(os.path.join(w, key + '.spv')) if not j.get('tf') else None
        if s:
            refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': 'gs', 'gs_slice': s,
                    'notes': [], 'drv_reads': '', 'drv_unfillable': [], 'bindings': []}   # no library: the host draws with the vertex shader's layer entry points
            write_refl(key, refl)
            return 'ok', '', refl
        try: return geometry_finish(key, w, j, c1)
        except (ValueError, subprocess.CalledProcessError) as e: return 'geometry failed', str(e)[:160], None
    if j['stage'] == 'vs': spv_single_lane(os.path.join(w, key + '.spv'))   # one-lane subgroups (see spv_single_lane)
    r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, os.path.join(w, key + '.spv')), os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
    src = open(msl).read()
    if j['stage'] == 'vs':   # second pass: re-translate with inputs that are only used as integers reported as uint (see int_inputs)
        extra = {l: t for l, t in int_inputs(src).items() if l not in j['attr']}
        if extra:
            j['attr'].update(extra); jp = os.path.join(w, f'job1_{key}.json'); json.dump([j], open(jp, 'w'))
            rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
            if rx.returncode or not os.path.exists(os.path.join(w, key + '.spv')): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
            spv_single_lane(os.path.join(w, key + '.spv'))
            r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, os.path.join(w, key + '.spv')), os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
            if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
            src = open(msl).read()
    try: src = dmsh_atomic.check_msl(src)
    except ValueError as e: return 'image atomic MSL failed', str(e), None
    src = precise(src)   # min / max / clamp with Maxwell's NaN results (see precise)
    try: src = hmma_msl(src)   # Ampere HMMA through simdgroup_matrix (see hmma_msl)
    except ValueError as e: return 'HMMA rewrite failed', str(e)[:160], None
    try: src = pool_arrays(src)   # handles traced to no slot read the pools the replay keeps (see pool_arrays)
    except ValueError as e: return 'pool rewrite failed', str(e)[:160], None
    src = dmsh_wide.wide_loads(src)   # storage words the translator split, read whole again (see dmsh_wide.py)
    src = border_emu(src); open(msl, 'w').write(src)   # border colours Metal lacks (see border_emu)
    first = []
    if j['stage'] == 'cs':
        src, first = first_store(src, j.get('local')); open(msl, 'w').write(src)   # the first invocation's store, for the host (see first_store)
    pad = []
    if j['stage'] == 'vs':   # outputs start as (0,0,0,1) (see vs_init); no point size beside a layer output (see vs_layer); the base instance from Metal (see vs_base_instance)
        src = vs_base_instance(vs_layer(vs_init(src))); open(msl, 'w').write(src)
    if j['stage'] == 'vs' and PAIRS.get(key):   # outputs the pixel shader reads but the vertex shader does not write get (0,0,0,1) (see vs_pad)
        src, pad = vs_pad(key, src)
        if pad: open(msl, 'w').write(src)
    lay = None; discard = False
    res = json.load(open(os.path.join(w, key + '.res.json')))
    if j['stage'] == 'vs':   # the two depth-convention entry points (see vs_depth); the analysis below uses the source with only main0
        capture = dmsh_geometry.capture(src)
        src, full = vs_depth(src)
        full += capture
        discard = vs_writes(src, reflect(key, 'vs', src, res))
        if discard: full += vs_discard(src)
        if j.get('tf'): full += vs_tf(src, j['tf'])   # transform feedback capture (see vs_tf)
        plain = full
        full, lay = vs_layer_entry(full)   # layer entry points for a slice-writing geometry shader (see vs_layer_entry)
        open(msl, 'w').write(full)
    fu, fmask = fs_uint(src) if j['stage'] == 'ps' else (None, 0)   # the integer-output entry points (see fs_uint); the analysis below uses the source with only main0
    fd = fs_dual(src) if j['stage'] == 'ps' and key in DUAL_WANT else None   # the dual-source entry point (see fs_dual), from the source with only main0
    if fu or fd: plain = src; open(msl, 'w').write((fu or src) + (fd or ''))
    def compile_targets():   # the selected targets from the same MSL (MSL identical to one compiled before takes that library, dmsh_mtlcache.py)
        if 'mac' in TARGETS: MTL.compile('mac', msl, lib)
        if 'ios' in TARGETS and IOSOUT: MTL.compile('ios', msl, os.path.join(IOSOUT, key + '_b.metallib'))
    try: compile_targets()
    except subprocess.CalledProcessError as e:
        if not lay and not fu and not fd: return 'metal failed', str(e)[:160], None
        open(msl, 'w').write(plain); lay = fu = fd = None   # the layer, integer-output and dual-source entry points are extras: a library that does not build with them is built without
        try: compile_targets()
        except subprocess.CalledProcessError as e: return 'metal failed', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = drv_reads(src)
    try: binds = reflect(key, j['stage'], src, res)
    except ValueError as e: return 'reflection failed', str(e)[:160], None
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': j['stage'],
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None, **({'entry_z': 'main0_z', 'entry_geometry_capture': 'main0_gs'} if j['stage'] == 'vs' else {}),
            **({'entry_discard': 'main0_discard'} if discard else {}),
            **({'entry_tf': 'main0_tf', 'tf_strides': [e['stride'] if e else 0 for e in j['tf']]} if j.get('tf') else {}),
            **({'entry_layer': 'main0_l', 'entry_layer_z': 'main0_lz', 'layer_from': lay} if lay else {}), **({'entry_u': 'main0_u'} if fu else {}), **({'entry_m': 'main0_m', 'uint_mask_m': fmask} if fu and fmask else {}), **({'entry_d': 'main0_d'} if fd else {}),
            **({'first_store': [{'reg': next(b['reg'] for b in binds if b['msl_name'] == n), 'word': w, 'value': v} for n, w, v in first]} if first else {}),
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []), 'drv_reads': dr, 'drv_unfillable': unfillable(dr, j['stage']),
            'bindings': binds,
            'vertex_inputs': [{'location': int(m.group(2)), 'msl_type': m.group(1)} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', src)],
            'raster': raster_facts(j['stage'], src), 'vs_pad': pad}
    if j['stage'] == 'vs': full = open(msl).read(); vin_finish(key, full, refl); pull_finish(key, full, refl)   # the shadow-input and pulling libraries (dmsh_vin.py, dmsh_pull.py), from the source with every entry point
    if j['stage'] == 'ps': full = open(msl).read(); alpha_finish(key, full, refl); logic_finish(key, full, refl)   # the alpha test and logic operation libraries (dmsh_alpha.py, dmsh_logic.py), from the source with every entry point
    write_refl(key, refl)
    fetch_finish(key, src, refl)
    return 'ok', '', refl

keys = build_keys(want, loc, PAIRS)
if want: want = set(keys)   # dependency outputs are cleaned too, so a failed pixel build cannot leave old MSL for padding
NJ = int(os.environ.get('J', '8'))
os.makedirs(out, exist_ok=True)
# A full build starts from no shader outputs, a partial one from none of the keys it builds, so what a run leaves is what it built: every output is restored from the cache anyway.
# The chain version is the same whatever the profile's tables, so a key that left the map, a shader that no longer builds or a variant no longer made would keep an old library.
# The stamp goes too, and a full build writes it at the end; a partial build keeps it only while it names the profile's current inputs
OUT_KEY = re.compile(r'([0-9a-f]{16})_b')
STAMP_BEFORE = dmsh_chain.read_stamp(out).get('inputs')
for _d in [out] + ([IOSOUT] if IOSOUT else []):
    for _n in os.listdir(_d):
        _m = OUT_KEY.match(_n)
        if _m and (not want or _m.group(1) in want): os.unlink(os.path.join(_d, _n))
    if (not want or STAMP_BEFORE != INPUTS) and os.path.exists(os.path.join(_d, dmsh_chain.STAMP)): os.unlink(os.path.join(_d, dmsh_chain.STAMP))
    if not want and os.path.exists(os.path.join(_d, CAS_USED)): os.unlink(os.path.join(_d, CAS_USED))   # written again at the end, with the stamp
w = tempfile.mkdtemp(prefix='dmsh_', dir=os.environ.get('WORK'))
MTL = dmsh_mtlcache.Cache(os.path.join(CAS, 'metallib'), {'mac': ('macosx', MAC_RECORD, mac_compile), **({'ios': ('iphoneos', ios_metal.RECORD, ios_metal.compile)} if IOSOUT else {})}, w)
# first take the libraries earlier chain versions left on disk: the b-*/ beside the output (iOS: the folder of the same name) and the roots DMSH_PREFILL lists
for _spec in [cache_root + ('=' + os.path.dirname(IOSOUT) if IOSOUT else '')] + [x for x in os.environ.get('DMSH_PREFILL', '').split(os.pathsep) if x]:
    _mac, _, _ios = _spec.partition('=')
    for _b in sorted(os.listdir(_mac)) if os.path.isdir(_mac) else []:
        if _b.startswith('b-') and os.path.realpath(os.path.join(_mac, _b)) != os.path.realpath(out):
            for _ln in MTL.fill(os.path.join(_mac, _b), os.path.join(_ios, _b) if _ios else None, targets=TARGETS): print(_ln, flush=True)
packs, jobs, c1s = {}, {}, {}
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
def rec_of(key):   # → (package name, the shader's record), read from the package each time and not kept (see record_index)
    name, k = loc[key]
    if name not in packs: packs[name] = record_index(os.path.join(dm_dir, name))
    fd, offs = packs[name]
    return name, os.pread(fd, offs[k + 1] - offs[k], offs[k])
for key in keys: digests[key] = shader_key(key, *rec_of(key))
def plain_digest(k):   # a paired pixel shader's key, also when it is not built this time
    if k in digests: return digests[k]
    return shader_key(k, *rec_of(k)) if k in loc else 'missing'
for key in keys:
    if PAIRS.get(key): digests[key] = pair_keyed(digests[key], [plain_digest(p) for p in sorted(PAIRS[key])])
for key in keys:
    name = loc[key][0]
    rule_fps[key] = hashlib.sha256(shader_rule_source(name) + ','.join(sorted(PAIRS.get(key, ()))).encode()).hexdigest()   # the vertex shader must be rebuilt when its pairs change (vs_pad)
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    else: jobs[key], c1s[key] = job_for(key, name, rec_of(key)[1], w)
misses = [key for key in keys if key not in cache_hits]
chunks = [misses[i::NJ] for i in range(NJ)]
def xl(ch):
    jp = os.path.join(w, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
    return subprocess.run([XLATE, jp, w], capture_output=True, text=True)
with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
xerr = {}
for r in xr:
    for ln in r.stderr.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def fin(key):
    if key in cache_hits:
        refl = json.load(open(os.path.join(out, key + '_b.json')))
        src = open(os.path.join(out, key + '_b.metal')).read() if refl['stage'] in ('ps', 'vs') else ''
        if refl['stage'] == 'vs': write_refl(key, pull_finish(key, src, vin_finish(key, src, refl)))
        return key, 'ok', '', fetch_finish(key, src, logic_finish(key, src, alpha_finish(key, src, refl)))
    if key in xerr or not os.path.exists(os.path.join(w, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
    result = (key,) + finish(key, w, jobs[key], c1s[key])
    if result[1] == 'ok': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
    return result
with cf.ThreadPoolExecutor(NJ) as ex:   # two phases: paired vertex shaders read their pixel shaders' MSL (vs_pad), so they go last
    res = list(ex.map(fin, [k for k in keys if k not in PAIRS])) + list(ex.map(fin, [k for k in keys if k in PAIRS]))
# Passthrough geometry as mesh shaders (dmsh_mesh.py): <vertex key>_<geometry key>_m3 for each pair in the profile's geo_mesh.tsv whose two libraries exist; the host
# draws a lean triangle-list geometry draw with it when its pair has one, and through the compute path otherwise (nvn_geometry.inc geo_mesh_draw)
MESH_STATS = collections.Counter()
def mesh_finish(pair):
    vk, gk = pair
    srcs = [os.path.join(out, n) for n in (vk + '_b.metal', gk + '_bl3.metal', gk + '_bl3.json')]
    libs = [os.path.join(out, f'{vk}_{gk}_m3.metallib')] + ([os.path.join(IOSOUT, f'{vk}_{gk}_m3.metallib')] if IOSOUT else [])
    if not all(os.path.exists(p) for p in srcs): MESH_STATS['missing a library'] += 1; return
    try: msl = dmsh_mesh.compose(open(srcs[0]).read(), open(srcs[1]).read(), json.load(open(srcs[2]))['geometry']['capture_reads'])
    except ValueError as e: MESH_STATS['not composed'] += 1; print(f'[RED] mesh {vk} {gk}: {e}', flush=True); return
    path = os.path.join(w, f'{vk}_{gk}_m3.metal'); open(path, 'w').write(msl)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', path, libs[0])
        if IOSOUT: MTL.compile('ios', path, libs[1])
        MESH_STATS['built'] += 1
    except subprocess.CalledProcessError as e: MESH_STATS['failed to compile'] += 1; print(f'[RED] mesh {vk} {gk}: does not compile ({str(e)[:120]})', flush=True)
_mesh = GAME.table('geo_mesh.tsv')
if _mesh.exists():
    with cf.ThreadPoolExecutor(NJ) as ex: list(ex.map(mesh_finish, [tuple(ln.split()[:2]) for ln in _mesh.read_text().splitlines() if ln.strip() and not ln.startswith('#')]))
shutil.rmtree(w, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'shaders {len(res)}: ok {st["ok"]}, ' + ', '.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print(f'shader cache: hit {PRE_STATS["hit"]}, rebuilt {PRE_STATS["miss"]}')
for reason in ('new key', 'rule changed', 'code fingerprint changed: all rebuilt'):
    if PRE_STATS[reason]: print(f'  {reason} {PRE_STATS[reason]}')
print(f'variant cache: generated {BF_STATS["generated"]}, hit {BF_STATS["hit"]}, rebuilt {BF_STATS["miss"]}, compiled {BF_STATS["compile"]}')
print(f'shadow-input libraries (<key>_bv, dmsh_vin.py): {VIN_STATS["generated"]} generated, {VIN_STATS["failed"]} failed to compile, {VIN_STATS["not generated"]} not generated; '
      f'{VIN_STATS["inputs"]} integer inputs shadowed, {VIN_STATS["float only"]} of them with no other-sign shadow (attribute indices ran out)')
print(f'alpha test libraries (<key>_ba, dmsh_alpha.py): {ALPHA_STATS["generated"]} generated, {ALPHA_STATS["failed"]} failed to compile, {ALPHA_STATS["not generated"]} pixel shaders without a float colour 0 alpha')
print(f'logic operation libraries (<key>_bl, dmsh_logic.py): {LOGIC_STATS["generated"]} generated, {LOGIC_STATS["failed"]} failed to compile, {LOGIC_STATS["not generated"]} pixel shaders without colour outputs')
print(f'pulling libraries (<key>_bp, dmsh_pull.py): {PULL_STATS["generated"]} generated, {PULL_STATS["failed"]} failed to compile, {PULL_STATS["not generated"]} not generated, '
      f'{PULL_STATS["no stage input"]} vertex shaders without stage inputs (none needed)')
print('mesh libraries (<vertex>_<geometry>_m3, dmsh_mesh.py): ' + (', '.join(f'{k} {v}' for k, v in MESH_STATS.items()) or 'none'))
print(MTL.summary())
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
ok = [r[3] for r in res if r[1] == 'ok']
rd = [x for x in ok if x['drv_reads']]
print(f'shaders reading driver segments at run time {len(rd)} (c0 {sum("0" in x["drv_reads"] for x in rd)}, c2 {sum("2" in x["drv_reads"] for x in rd)}); reading what the replay layer cannot fill {sum(bool(x["drv_unfillable"]) for x in ok)}')
for e, n in collections.Counter(u for x in ok for u in x['drv_unfillable']).most_common(15): print(f'  cannot fill {n:5d}  {e}')
f1 = [b for x in ok for b in x['bindings'] if b.get('fetch1d')]
print(f'1D integer fetches read as 2D rows of 16384 texels (a texture buffer there is bound as rows, nvn_mtl.m tbuf_as_2d): {len(f1)} bindings in {sum(any(b.get("fetch1d") for b in x["bindings"]) for x in ok)} shaders, '
      f'{sum(1 for b in f1 if b.get("fetch1d_size"))} of them also size-queried')
SLOTK = {abi.KIND_CBUF, abi.KIND_SAMP, abi.KIND_SAMP_TEX, abi.KIND_TEX, abi.KIND_IMG, abi.KIND_TBUF, abi.KIND_SBUF, abi.KIND_UBO_PTR, abi.KIND_SBUF_UBO, abi.KIND_TEX_UBO, abi.KIND_SAMP_UBO, abi.KIND_SAMP_ID}
noreg = [x for x in ok if any(b['kind'] in SLOTK and b.get('reg') is None for b in x['bindings'])]
unk = [x for x in ok if any(b['kind'] == abi.KIND_UNKNOWN for b in x['bindings'])]
print(f'shaders with a binding whose slot was not decoded {len(noreg)}, with an unrecognized binding kind {len(unk)} (loading them at run time prints [RED] lines)')
for e, n in collections.Counter(f"{b['kind']} {b['msl_name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['kind'] == abi.KIND_UNKNOWN or (b['kind'] in SLOTK and b.get('reg') is None)).most_common(8): print(f'  {n:5d}  {e}')
nt = collections.Counter(nn.split(' (')[0] for x in ok for nn in x['notes'] if not nn.startswith('translator: '))
for e, n in nt.most_common(8): print(f'  queries without an answer {n:5d}  {e}')
tm = collections.Counter(nn[len('translator: '):] for x in ok for nn in x['notes'] if nn.startswith('translator: '))   # the translator's own messages (dmsh_xlate Log): bindless reads resolved or removed, invalid encodings
for e, n in tm.most_common(12): print(f'  translator {n:6d} in {sum(e in [nn[len("translator: "):] for nn in x["notes"]] for x in ok):5d} shaders  {e}')
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
if not want:   # a full build: the stamp, then the c1 guard and uniform read extent tables from these Mac outputs, into the profile folder (the host takes them only beside this stamp)
    # The per-shader cache entries this build used (cas/shader/<digest>), which the NSP script keeps when it prunes the others (prune_shader_cache). Only beside the Mac outputs:
    # every full build writes them, and the iOS folder goes whole into the IPA. Before the stamp, so a stamped folder always has its list
    with open(os.path.join(out, CAS_USED + '.tmp'), 'w') as _f: _f.write(''.join(d + '\n' for d in sorted(set(digests.values()))))
    os.replace(os.path.join(out, CAS_USED + '.tmp'), os.path.join(out, CAS_USED))
    for _d in [out] + ([IOSOUT] if IOSOUT else []): dmsh_chain.write_stamp(_d, GAME.id, INPUTS)
    print(f'library stamp: game {GAME.id}, inputs {INPUTS}', flush=True)
    for _tool, _table in (('c1_guard.py', 'c1_full.tsv'), ('cb_extent.py', 'cb_extent.tsv')):
        _r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), _tool), out, str(GAME.table(_table))], capture_output=True, text=True)
        if _r.returncode: sys.exit(f'[RED] {_tool} failed ({_r.returncode}): {_r.stderr.strip()[-500:]}')
        print(f'{_table}: {(_r.stdout.strip().splitlines() or [""])[-1]}', flush=True)
elif STAMP_BEFORE != INPUTS: print(f'no library stamp: the profile\'s inputs changed since the last full build ({STAMP_BEFORE} → {INPUTS}), and a partial build does not rebuild the rest', flush=True)

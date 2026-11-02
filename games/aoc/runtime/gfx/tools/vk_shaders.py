#!/usr/bin/env python3
# Vulkan games' SPIR-V shaders → one Metal library + a resource table for the host's Vulkan layer (dm/gfx/vk/hle_vk.m). Hades ships its 74 shaders as SPIR-V files
# (RomFS Content/EffectsForgeSwitch/*.vert / *.frag); the game hands the same words to vkCreateShaderModule, so a shader is found again by the hash of its SPIR-V.
# Per shader:
#   · key = sha256(SPIR-V words)[:16] (hex); the Metal function is vk_<key>
#   · its descriptor bindings are renumbered per kind (uniform / storage buffers → [[buffer(n)]], images → [[texture(n)]], samplers → [[sampler(n)]], n from 0 in binding order)
#     by rewriting the Binding decorations in a copy of the SPIR-V, then translated with spirv-cross --msl-decoration-binding: the slots stay below Metal's limits whatever the
#     game's set / binding numbers are (Hades uses set 1 bindings up to 49), and the table records which (set, binding) each slot came from
#   · vertex shaders get --flip-vert-y: Vulkan's clip space has y pointing down, Metal's up, and the viewports stay as the game gives them
#   · vertex buffers are bound by the host from slot 30 downwards (30 - Vulkan binding), above every shader's buffer slots
#   · only resources the translated entry point takes are listed (a declared but unused one has no Metal slot to bind)
#   · a push-constant block gets the buffer slot after the descriptor buffers (spirv-cross puts it at buffer(0), where the first uniform buffer goes), listed as p:0.0=slot;
#     the host sets the pushed bytes there (Super Mario Galaxy: 54 of its 56 built-in shaders have one)
#   · stages other than vertex and fragment are skipped and counted: Metal has no geometry shaders (Super Mario Galaxy has 2)
#   · a fragment shader with an output at location 1 also gets a variant vk_<key>_d where that output is [[color(0), index(1)]]: Super Mario Galaxy's GX shaders write
#     the second blend source at location 1 (no Index decoration), which NVIDIA's hardware feeds to dual-source blending; the host picks the variant for pipelines
#     whose blend factors read source 1
# Super Mario Galaxy generates its GX shaders at run time; they come from a capture folder (the Mac host's DM_VK_CAPTURE), read like a RomFS: every input folder is
# searched for SPIR-V files by their magic
# Output: <out>/vk/vk_shaders.metallib and <out>/vk/vk_shaders.tsv; --ios <dir> also writes <dir>/vk/ with an iPhone library (same table)
# Table rows: key <TAB> stage (vert / frag) <TAB> resources, space-separated kind:set.binding=slot[*count] (count: an array of descriptors in consecutive slots; u uniform buffer, b storage buffer, t image, s sampler, c combined image sampler = texture and sampler slot n,
#   p push constants)
# Usage: vk_shaders.py <SPIR-V dir>... <out dir> [--ios <dir>]   (SPIR-V dirs: the RomFS, capture folders)
import concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, tempfile

MAGIC = 0x07230203
OP_DECORATE, OP_MEMBER_DECORATE, OP_TYPE_ARRAY, OP_TYPE_RTARRAY, OP_TYPE_STRUCT = 71, 72, 28, 29, 30
DEC_ARRAY_STRIDE, DEC_BINDING, DEC_SET = 6, 33, 34
KINDS = {'ubos': 'u', 'ssbos': 'b', 'separate_images': 't', 'images': 't', 'separate_samplers': 's', 'textures': 'c'}
SLOT_CLASS = {'u': 'buffer', 'b': 'buffer', 't': 'texture', 's': 'sampler', 'c': 'texture', 'p': 'buffer'}
# GLSL's FMin / FMax / FClamp, which spirv-cross writes as fast::min / max / clamp, leave a NaN operand's result to the GPU; the Switch's GPU returns the other operand, as Metal's
# precise:: functions (IEEE fmin / fmax) do on every GPU. Super Mario Galaxy 2's GX lighting normalizes light directions the game leaves at (0, 0, 0) (the key and fill lights of
# its characters) and takes max(0, dot(ldir, NaN)) as the spot term: 0 on the Switch and on the M4 Pro, so the attenuation is 1; on an A20 Pro (iPhone) the half of Mario
# that the fill light (from the camera's left) faces came out white (GitHub issue 72)
PRECISE = re.compile(r'\bfast::(min|max|clamp)\(')

def red(msg): sys.exit('[RED] ' + msg)

def spirv_files(root):
    for d, _, fs in os.walk(root):
        for f in sorted(fs):
            p = os.path.join(d, f)
            with open(p, 'rb') as fh: head = fh.read(4)
            if len(head) == 4 and struct.unpack('<I', head)[0] == MAGIC: yield p

def decorations(words):
    """variable id → {'set': n, 'binding': n, 'binding_at': word index of the binding value}"""
    out, i = {}, 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n == 0: red('SPIR-V instruction length 0')
        if op == OP_DECORATE and n >= 4:
            v = out.setdefault(words[i + 1], {})
            if words[i + 2] == DEC_BINDING: v['binding'], v['binding_at'] = words[i + 3], i + 3
            elif words[i + 2] == DEC_SET: v['set'] = words[i + 3]
        i += n
    return out

def fix_strides(words):
    """Super Mario Galaxy's generated shaders put ArrayStride on the struct member (OpMemberDecorate) instead of on the array type: NVIDIA's driver accepts it, spirv-cross
    stops with "Struct member does not have ArrayStride set". Copy each such stride onto its array type, inserted after the last of those member decorations"""
    arrays, have, member, members, at, i = set(), set(), [], {}, None, 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n == 0: red('SPIR-V instruction length 0')
        if op == OP_DECORATE and n >= 4 and words[i + 2] == DEC_ARRAY_STRIDE: have.add(words[i + 1])
        elif op == OP_MEMBER_DECORATE and n >= 5 and words[i + 3] == DEC_ARRAY_STRIDE: member.append((words[i + 1], words[i + 2], words[i + 4])); at = i + n
        elif op in (OP_TYPE_ARRAY, OP_TYPE_RTARRAY): arrays.add(words[i + 1])
        elif op == OP_TYPE_STRUCT: members[words[i + 1]] = words[i + 2:i + n]
        i += n
    add = {}
    for st, m, stride in member:
        t = members.get(st, [])[m] if m < len(members.get(st, [])) else None
        if t in arrays and t not in have: add.setdefault(t, stride)
    if not add: return words
    return words[:at] + [w for t, s in add.items() for w in ((4 << 16) | OP_DECORATE, t, DEC_ARRAY_STRIDE, s)] + words[at:]

def one(path, tmp, root):
    data = open(path, 'rb').read()
    if len(data) % 4: red(f'{path}: SPIR-V length {len(data)} is not a multiple of 4')
    key = hashlib.sha256(data).hexdigest()[:16]   # of the game's words, which the host hashes
    words = fix_strides(list(struct.unpack(f'<{len(data) // 4}I', data)))
    src_spv = os.path.join(tmp, key + '.in.spv'); open(src_spv, 'wb').write(struct.pack(f'<{len(words)}I', *words))
    refl = json.loads(subprocess.run(['spirv-cross', src_spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    eps = refl.get('entryPoints', [])
    if len(eps) != 1: red(f'{path}: entry points {eps} are not exactly one')
    stage = eps[0]['mode']
    if stage not in ('vert', 'frag'): return stage   # skipped (main counts them)
    dec = decorations(words)
    by_pair, count = {}, {}
    for k, kind in KINDS.items():
        for r in refl.get(k, []):
            pair = (r.get('set', 0), r['binding']); by_pair[pair] = kind
            dims = r.get('array') or [1]
            if len(dims) != 1 or not dims[0]: red(f'{path}: array dimensions {dims} of resource {r.get("name")} are not implemented')
            count[pair] = dims[0]   # an array of n descriptors takes n consecutive slots (Super Mario Galaxy: 4 position-matrix uniform buffers, 8 textures)
    pcs = refl.get('push_constants', [])
    if len(pcs) > 1: red(f'{path}: {len(pcs)} push constant blocks')
    slots, nxt, res = {}, {'buffer': 0, 'texture': 0, 'sampler': 0}, []
    for vid, d in sorted(dec.items(), key=lambda t: (t[1].get('set', 0), t[1].get('binding', 0))):
        if 'binding_at' not in d: continue
        pair = (d.get('set', 0), d['binding'])
        kind = by_pair.get(pair)
        if not kind: continue   # declared but unused: spirv-cross drops it
        if pair in slots: n = slots[pair]
        else:
            cls, k = SLOT_CLASS[kind], count[pair]; n = nxt[cls]; nxt[cls] += k
            if kind == 'c': nxt['sampler'] = max(nxt['sampler'], n + k)
            slots[pair] = n; res.append(f'{kind}:{pair[0]}.{pair[1]}={n}' + (f'*{k}' if k > 1 else ''))
        words[d['binding_at']] = n
    pc_slot = nxt['buffer'] if pcs else None
    if pcs: nxt['buffer'] += 1
    if nxt['buffer'] > 16 or nxt['texture'] > 31 or nxt['sampler'] > 16: red(f'{path}: resource slots exceeded {nxt}')
    spv = os.path.join(tmp, key + '.spv'); msl = os.path.join(tmp, key + '.metal')
    open(spv, 'wb').write(struct.pack(f'<{len(words)}I', *words))
    cmd = ['spirv-cross', spv, '--msl', '--msl-version', '20300', '--msl-decoration-binding', '--rename-entry-point', 'main', f'vk_{key}', stage, '--output', msl]
    if stage == 'vert': cmd.append('--flip-vert-y')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: red(f'{path}: spirv-cross failed: {r.stderr.strip()[:300]}')
    # The reflection lists every declared resource; the Metal entry point only takes the ones the shader uses (Hades' ShadowMap.frag declares a buffer it never reads,
    # which the game's own pipeline layout leaves out): keep the rows whose slot is an argument of the entry point, so the host only binds, and only reports, real ones
    src = open(msl).read(); m = re.search(r'\bvk_' + key + r'\((.*?)\)\s*\{', src, re.S)
    if not m: red(f'{path}: entry point vk_{key} not found in the translated MSL')
    if pcs:   # the push-constant argument is `constant <block type>& name [[buffer(0)]]`: move it to its own slot
        tname = refl['types'][pcs[0]['type']]['name']
        a = re.search(r'constant\s+' + re.escape(tname) + r'&\s+\w+\s+\[\[buffer\((\d+)\)\]\]', m.group(1))
        if a:
            s0, s1 = m.start(1) + a.start(1), m.start(1) + a.end(1)
            src = src[:s0] + str(pc_slot) + src[s1:]; open(msl, 'w').write(src)
            m = re.search(r'\bvk_' + key + r'\((.*?)\)\s*\{', src, re.S)
            res.append(f'p:0.0={pc_slot}')
    used = {(c, int(n)) for c, n in re.findall(r'\[\[(buffer|texture|sampler)\((\d+)\)\]\]', m.group(1))}
    res = [x for x in res if (SLOT_CLASS[x[0]], int(x.rsplit('=', 1)[1].split('*')[0])) in used]
    src = PRECISE.sub(r'precise::\1(', src); open(msl, 'w').write(src)
    msls = [msl]
    if stage == 'frag' and '[[color(1)]]' in src:   # dual-source variant vk_<key>_d: the output at location 1 becomes the second blend source of attachment 0
        msl_d = os.path.join(tmp, key + '_d.metal'); msls.append(msl_d)
        open(msl_d, 'w').write(src.replace(f'vk_{key}', f'vk_{key}_d').replace('[[color(1)]]', '[[color(0), index(1)]]'))
    return key, stage, res, msls, '/' + os.path.relpath(path, root)

def metallib(msls, out, sdk, extra):
    os.makedirs(out, exist_ok=True)
    airs = []
    def comp(m):
        a = m[:-6] + f'.{sdk}.air'
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', *extra, m, '-o', a], capture_output=True, text=True)
        if r.returncode: red(f'{m}: metal compile failed: {r.stderr.strip()[:400]}')
        return a
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: airs = list(ex.map(comp, msls))
    lib = os.path.join(out, 'vk_shaders.metallib')
    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', *airs, '-o', lib], check=True)
    return lib

def main():
    a = sys.argv[1:]
    ios = None
    if '--ios' in a: i = a.index('--ios'); ios = a[i + 1]; del a[i:i + 2]
    if len(a) < 2: sys.exit('usage: vk_shaders.py <SPIR-V dir>... <out dir> [--ios <dir>]')
    roots, out = a[:-1], a[-1]
    files = [(p, r) for r in roots for p in spirv_files(r)]
    if not files: red(f'no SPIR-V files in {" ".join(roots)}')
    with tempfile.TemporaryDirectory() as tmp:
        with cf.ThreadPoolExecutor(os.cpu_count()) as ex: rows = list(ex.map(lambda pr: one(pr[0], tmp, pr[1]), files))
        seen, skipped = {}, {}
        for row in rows:
            if isinstance(row, str): skipped[row] = skipped.get(row, 0) + 1; continue
            key, stage, res, msl, src = row
            if key in seen: continue
            seen[key] = (stage, res, msl, src)
        if skipped: print('[RED] skipped shader stages Metal does not have: ' + ', '.join(f'{k} {v}' for k, v in sorted(skipped.items())))
        msls = [m for v in seen.values() for m in v[2]]
        tsv = ['# Vulkan shaders (dm/gfx/tools/vk_shaders.py): key = sha256(SPIR-V)[:16], Metal function vk_<key>; resources kind:set.binding=slot (u uniform buffer, b storage buffer, t image, s sampler, c combined, p push constants)',
               '# spirv-cross ' + ' '.join((lambda r: r.stdout + r.stderr)(subprocess.run(['spirv-cross', '--revision'], capture_output=True, text=True)).split())]
        tsv += [f'{k}\t{v[0]}\t{" ".join(v[1])}\t{v[3]}' for k, v in sorted(seen.items())]
        dirs = [(os.path.join(out, 'vk'), 'macosx', [])]
        # The iPhone library is compiled with -fmetal-math-mode=relaxed, as the NVN chain's (dm/ios/tools/ios_metal.py): the default fast mode lets the GPU compiler assume there is
        # no NaN / Inf, which would let it drop the NaN handling of the precise:: functions above
        if ios: dirs.append((os.path.join(ios, 'vk'), 'iphoneos', ['-mios-version-min=18.0', '-fmetal-math-mode=relaxed']))
        for d, sdk, extra in dirs:
            lib = metallib(msls, d, sdk, extra)
            open(os.path.join(d, 'vk_shaders.tsv'), 'w').write('\n'.join(tsv) + '\n')
            print(f'{sdk}: {len(seen)} shaders → {lib}')

if __name__ == '__main__': main()

#!/usr/bin/env python3
# OpenGL games' GLSL shaders → one Metal library + a table for the host's GL layer (dm/gfx/gl/hle_gl.m). Digimon Story Cyber Sleuth: Complete Edition renders with
# NVIDIA's desktop GL through EGL and compiles GLSL ES 3.20 source at run time (glShaderSource / glCompileShader); the host finds each shader again by the hash of its source.
# Sources, all searched in every input path:
#   · MVGL archives (*.mvgl, dm/gfx/tools/mvgl.py): every .shad file, named <material>_vp / <material>_fp (1,682 files, 615 distinct sources in Digimon);
#   · an NSO (a file that starts with NSO0, the game's main): every NUL-terminated string that starts with '#version' (its built-in post effects, movie and shadow shaders);
#   · capture folders (the Mac host's DM_GL_CAPTURE): <key>.vert / <key>.frag, what the game really compiled, for sources it puts together at run time.
# Per shader:
#   · key = sha256(the source text up to its first NUL)[:16] (hex), as the host hashes the strings glShaderSource gets; the Metal function is gl_<key>_v or gl_<key>_f;
#   · the stage: a capture's extension, an archive file's _vp / _fp; a string in main says nothing of its stage, so it is translated as each stage that compiles (one of
#     Digimon's built-in fragment shaders names gl_Position in a #define);
#   · GLSL → SPIR-V with glslang (-V -R: relaxed Vulkan rules, which gather the loose uniforms into one block, gl_DefaultUniformBlock; --aml / --amb number the inputs,
#     outputs and bindings), SPIR-V → MSL with spirv-cross (--msl-decoration-binding: Metal slots = those bindings; vertex shaders --fixup-clipspace: GL's z from −w to w → 0 to w);
#   · varyings are matched by name, as GL links them: --aml numbers each stage's outputs in its own declaration order, so [[user(locnN)]] becomes [[user(<variable name>)]];
#   · the host renders in GL's orientation (row 0 at the bottom, the NVN layer's lower-left origin), so nothing is flipped here.
# Output: <out>/gl/gl_shaders.metallib and <out>/gl/gl_shaders.tsv; --ios <dir> also writes <dir>/gl/ with an iPhone library (same table)
# Table rows: key <TAB> stage (vert / frag) <TAB> fields, space-separated (a key can have a row per stage):
#   a:<name>=<slot>:<type>                       a vertex input, [[attribute(slot)]]
#   u:<name>=<offset>:<type>:<count>:<stride>    a member of the default uniform block (count 0 = not an array)
#   b:<slot>:<size>                              the default uniform block: its buffer slot and size in bytes
#   t:<name>=<slot>:<type>                       a combined sampler: texture and sampler slot
#   o:<name>=<location>                          a fragment output, [[color(location)]]
# <TAB> where it came from
# Usage: gl_shaders.py <input path>... <out dir> [--ios <dir>]   (input paths: the RomFS, the main NSO, capture folders)
import concurrent.futures as cf, hashlib, json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvgl
from dmsh_chain import SPIRV_CROSS   # the in-repo spirv-cross build the NVN chain uses too (spirv_cross_bin)

VEC_TYPES = {'float', 'vec2', 'vec3', 'vec4', 'int', 'ivec2', 'ivec3', 'ivec4', 'uint', 'uvec2', 'uvec3', 'uvec4', 'bool', 'mat2', 'mat3', 'mat4'}

def red(msg): sys.exit('[RED] ' + msg)

def text_of(b):
    b = bytes(b); z = b.find(b'\0')
    return b[:z] if z >= 0 else b

def nso_strings(path):
    """'#version <number>' strings of an NSO's read-only segment (dm/tools/nso.py); a main that links glslang (Balatro's LÖVE) also has its messages that start with '#version:'.
    nso.Error (a ValueError) on a damaged file"""
    import nso
    with open(path, 'rb') as f: m = nso.load(f.read(), path)
    (mo, sz) = m.segments[1]; img = bytes(m.image[mo:mo + sz])
    return [text_of(img[m.start():m.start() + 0x10000]) for m in re.finditer(rb'(?:^|(?<=\0))#version [0-9]', img)]

def sources(roots):
    """(source bytes, stage or None, where) for every shader in the input paths"""
    for r in roots:
        if os.path.isfile(r):
            with open(r, 'rb') as f: head = f.read(4)
            if head == b'NSO0':
                for k, s in enumerate(nso_strings(r)): yield s, None, f'{os.path.basename(r)}#{k}'
            continue
        for d, _, fs in os.walk(r):
            for fn in sorted(fs):
                p = os.path.join(d, fn)
                if fn.endswith('.mvgl'):
                    try: a = mvgl.Archive(p)
                    except ValueError: continue   # the sound archives are CRI AFS2 under the same extension
                    for nm in sorted(a.files):
                        if not nm.endswith('.shad'): continue
                        st = 'vert' if nm.endswith('_vp.shad') else 'frag' if nm.endswith('_fp.shad') else None
                        yield text_of(a.read(nm)), st, f'/{os.path.relpath(p, r)}:{nm}'
                elif fn.endswith(('.vert', '.frag')):
                    with open(p, 'rb') as f: source = text_of(f.read())
                    yield source, fn.rsplit('.', 1)[1], '/' + os.path.relpath(p, r)

# NVIDIA's GLSL compiler accepts more than the GLSL ES 3.20 specification, and glslang does not; these fix-ups run on the preprocessed text (glslang -E, so macros are expanded):
#   · the GLSL ES 1.0 texture functions (89 of Digimon's fragment shaders call textureCube; with -V glslang reads textureCube as Vulkan's texture type, so a macro cannot rename it);
#   · a redeclaration of gl_Position as a plain output (the built-in sprite shader of main declares `out float4 vPosition;` with vPosition defined as gl_Position);
#   · textureOffset with an offset that is not a constant (the built-in FXAA shader): Metal's sample offsets are constants too, so it becomes texture at P + offset / level 0 size
OLD_TEX = [(r'\btexture2DLod\s*\(', 'textureLod('), (r'\btextureCubeLod\s*\(', 'textureLod('), (r'\btexture2DProj\s*\(', 'textureProj('),
           (r'\btexture2D\s*\(', 'texture('), (r'\btextureCube\s*\(', 'texture(')]

def call_args(t, i):
    """the arguments of the call whose '(' is at t[i], and the index after its ')'"""
    depth, args, start = 0, [], i + 1
    for k in range(i, len(t)):
        c = t[k]
        if c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: args.append(t[start:k]); return args, k + 1
        elif c == ',' and depth == 1: args.append(t[start:k]); start = k + 1
    raise ValueError('unbalanced parentheses')

def fixups(t):
    for a, b in OLD_TEX: t = re.sub(a, b, t)
    t = re.sub(r'^[ \t]*out\s+(?:highp\s+|mediump\s+|lowp\s+)?vec4\s+gl_Position\s*;', '', t, flags=re.M)
    out, i = [], 0
    for m in re.finditer(r'\btextureOffset\s*\(', t):
        if m.start() < i: continue
        args, end = call_args(t, m.end() - 1)
        if len(args) == 3 and re.search(r'[A-Za-z_]\w*', re.sub(r'\b[iu]?vec[234]\b', '', args[2])):   # an offset with a variable in it
            s_, p_, o_ = (x.strip() for x in args)
            out.append(t[i:m.start()] + f'texture({s_}, ({p_}) + vec2({o_}) / vec2(textureSize({s_}, 0)))'); i = end
    return ''.join(out) + t[i:]

def glsl_type(t, refl):
    return refl['types'][t]['name'] if t in refl.get('types', {}) else t

def uni_members(refl, ms, prefix, base, f, bools):
    """Table rows for default block members. A struct's members recurse with their dotted name, which is the name the game passes to GL (u_LightEnv.m_dirLights_dir[0]); the
    reflection gives a nested member's offset relative to its parent, so the parent's offset is added"""
    for mb in ms:
        arr = mb.get('array', [])
        name = prefix + mb['name']
        if len(arr) > 1: return f'uniform {name}: arrays of arrays'
        if 'members' in refl['types'].get(mb['type'], {}):
            if arr: return f'uniform {name}: an array of structs is not implemented'
            e = uni_members(refl, refl['types'][mb['type']]['members'], name + '.', base + mb['offset'], f, bools)
            if e: return e
            continue
        ty = glsl_type(mb['type'], refl)
        if ty == 'uint' and mb['name'] in bools: ty = 'bool'
        if ty not in VEC_TYPES: return f'uniform {name}: type {ty} (structs are not implemented)'
        f.append(f'u:{name}={base + mb["offset"]}:{ty}:{arr[0] if arr else 0}:{mb.get("array_stride", 0)}')

# GLSL ES 1.00, with its version statement (#version 100) or, as GameMaker's runner writes it, without one (Well Dweller's 93 run-time shaders): `attribute`, `varying`, `texture2D`, `gl_FragColor`, precision statements and
# lowp / mediump / highp qualifiers. SPIR-V needs ES 3.10 or newer, so the text is rewritten before preprocessing (glslang rejects those keywords in 310). Metal has no use for the precision qualifiers;
# the texture functions stay for fixups(), which renames them after preprocessing (a macro could hide one)
def es100(t, stage):
    t = re.sub(r'^[ \t]*precision[ \t]+\w+[ \t]+\w+[ \t]*;\r?[ \t]*\r?\n', '', t, flags=re.M)
    t = re.sub(r'\b(?:lowp|mediump|highp)\b[ \t]*', '', t)
    t = re.sub(r'\battribute\b', 'in', t)
    t = re.sub(r'\bvarying\b', 'out' if stage == 'vert' else 'in', t)
    if stage == 'frag' and 'gl_FragColor' in t:
        t = 'out vec4 gm_FragColor;\n' + t.replace('gl_FragColor', 'gm_FragColor')
    t = re.sub(r'\bsample\b', 'gm_sample', t)   # `sample` became a reserved word after ES 1.00 (GameMaker's sprite shaders use it as a variable)
    return '#version 310 es\nprecision highp float;\nprecision highp int;\n' + t

def translate(src, stage, key, tmp):
    """GLSL → MSL; returns (msl path, table fields) or an error string"""
    t = src.decode('latin1')
    v = re.search(r'^[ \t]*#version[ \t]+(\d+)[^\n]*\n?', t, re.M)
    if not v or v.group(1) == '100': src = es100(t.replace(v.group(0), '', 1) if v else t, stage).encode('latin1')
    g = os.path.join(tmp, f'{key}.{stage}'); spv = g + '.spv'; msl = os.path.join(tmp, f'{key}_{stage[0]}.metal')
    open(g + '.in', 'wb').write(src)
    r = subprocess.run(['glslangValidator', '-E', '-S', stage, g + '.in'], capture_output=True, text=True)
    if r.returncode: return 'glslang -E: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    pre = r.stdout[r.stdout.find('#version'):]   # warnings come first
    open(g, 'w').write(fixups(pre))
    r = subprocess.run(['glslangValidator', '-V', '-R', '--aml', '--amb', '-S', stage, g, '-o', spv], capture_output=True, text=True)
    if r.returncode: return 'glslang: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    refl = json.loads(subprocess.run([SPIRV_CROSS, spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    cmd = [SPIRV_CROSS, spv, '--msl', '--msl-version', '20300', '--msl-decoration-binding', '--rename-entry-point', 'main', f'gl_{key}_{stage[0]}', stage, '--output', msl]
    if stage == 'vert': cmd.append('--fixup-clipspace')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: return 'spirv-cross: ' + r.stderr.strip()[:300]
    m = open(msl).read()
    # Varyings by name: the out struct of a vertex shader and the in struct of a fragment shader (stage_in members of a vertex shader are [[attribute(n)]])
    m = re.sub(r'(\b(\w+)\s+\[\[)user\(locn\d+\)', lambda x: f'{x.group(1)}user({x.group(2)})', m)
    m = re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', m)   # a NaN operand gives the other one, as on the Switch's GPU (vk_shaders.py PRECISE)
    open(msl, 'w').write(m)
    f = []
    if stage == 'vert':
        for v in refl.get('inputs', []): f.append(f'a:{v["name"]}={v["location"]}:{v["type"]}')
    else:
        for v in refl.get('outputs', []): f.append(f'o:{v["name"]}={v["location"]}')
    # a bool uniform is a uint in the SPIR-V block (it has no bool members), but the game asks GL for its declared type: LÖVE's Shader:send takes only a boolean for a GLSL bool
    bools = {n for n in re.findall(r'\buniform\s+(?:(?:lowp|mediump|highp)\s+)?bool\s+(\w+)\s*;', pre)}
    for u in refl.get('ubos', []):
        t = refl['types'][u['type']]
        if t['name'] != 'gl_DefaultUniformBlock': return f'uniform block {t["name"]} is not the default block'
        f.append(f'b:{u["binding"]}:{u["block_size"]}')
        e = uni_members(refl, t['members'], '', 0, f, bools)
        if e: return e
    for t in refl.get('textures', []):
        if t.get('array'): return f'sampler array {t["name"]}'
        f.append(f't:{t["name"]}={t["binding"]}:{t["type"]}')
    for k in ('separate_images', 'separate_samplers', 'images', 'ssbos', 'push_constants'):
        if refl.get(k): return f'{k} are not implemented'
    if any(int(x) > 15 for x in re.findall(r'\[\[(?:sampler|buffer)\((\d+)\)\]\]', m)): return 'a buffer or sampler slot above 15'
    return msl, f

def one(item, tmp):
    """[(key, stage, fields, msl, where)], or [(key, None, errors, None, where)] when no stage translates"""
    src, stage, where = item
    key = hashlib.sha256(src).hexdigest()[:16]
    out, errs = [], []
    for st in [stage] if stage else ['vert', 'frag']:
        r = translate(src, st, key, tmp)
        if isinstance(r, tuple): out.append((key, st, r[1], r[0], where))
        else: errs.append(f'{st}: {r}')
    return out or [(key, None, errs, None, where)]

def metallib(msls, out, sdk, extra):
    os.makedirs(out, exist_ok=True)
    def comp(m):
        a = m[:-6] + f'.{sdk}.air'
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', *extra, m, '-o', a], capture_output=True, text=True)
        if r.returncode: red(f'{m}: metal compile failed: {r.stderr.strip()[:400]}')
        return a
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: airs = list(ex.map(comp, msls))
    lib = os.path.join(out, 'gl_shaders.metallib')
    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', *airs, '-o', lib], check=True)
    return lib

def main():
    a = sys.argv[1:]
    ios = None
    if '--ios' in a: i = a.index('--ios'); ios = a[i + 1]; del a[i:i + 2]
    if len(a) < 2: sys.exit('usage: gl_shaders.py <input path>... <out dir> [--ios <dir>]')
    roots, out = a[:-1], a[-1]
    items, seen_src = [], set()
    for s, st, where in sources(roots):
        if (s, st) in seen_src: continue
        seen_src.add((s, st)); items.append((s, st, where))
    if not items: red(f'no GLSL shaders in {" ".join(roots)}')
    with tempfile.TemporaryDirectory() as tmp:
        with cf.ThreadPoolExecutor(os.cpu_count()) as ex: rows = [r for rs in ex.map(lambda it: one(it, tmp), items) for r in rs]
        seen, failed = {}, []
        for key, stage, f, msl, where in rows:
            if not stage: failed.append((key, where, f)); continue
            if (key, stage) not in seen: seen[(key, stage)] = (stage, f, msl, where)
        for key, where, errs in failed:
            if not any(k == key for k, _ in seen): print(f'[RED] {where} ({key}) does not translate: {" / ".join(errs)}')
        tsv = ['# GL shaders (dm/gfx/tools/gl_shaders.py): key = sha256(GLSL source)[:16], Metal function gl_<key>_v / gl_<key>_f; a: vertex inputs, u: default uniform block members '
               '(offset:type:count:stride), b: the block (slot:size), t: combined samplers (slot:type), o: fragment outputs',
               '# ' + ' '.join(subprocess.run(['glslangValidator', '--version'], capture_output=True, text=True).stdout.split()[:3]) + '; spirv-cross '
               + ' '.join((lambda r: r.stdout + r.stderr)(subprocess.run([SPIRV_CROSS, '--revision'], capture_output=True, text=True)).split())]
        tsv += [f'{k[0]}\t{v[0]}\t{" ".join(v[1])}\t{v[3]}' for k, v in sorted(seen.items())]
        dirs = [(os.path.join(out, 'gl'), 'macosx', [])]
        if ios: dirs.append((os.path.join(ios, 'gl'), 'iphoneos', ['-mios-version-min=18.0', '-fmetal-math-mode=relaxed']))   # relaxed as in vk_shaders.py and ios_metal.py: no NaN / Inf assumption on the iPhone's GPU compiler
        msls = [v[2] for v in seen.values()]
        for d, sdk, extra in dirs:
            lib = metallib(msls, d, sdk, extra)
            open(os.path.join(d, 'gl_shaders.tsv'), 'w').write('\n'.join(tsv) + '\n')
            print(f'{sdk}: {len(seen)} shaders ({sum(1 for v in seen.values() if v[0] == "vert")} vertex) → {lib}' + (f'; {len(failed)} failed' if failed else ''))

if __name__ == '__main__':
    import nso
    try: main()
    except nso.Error as e: red(str(e))   # a damaged main

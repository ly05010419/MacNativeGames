# "Readable-tile" version of a fragment shader (merge B step 2): within one pass, when a later draw samples, at its own pixel, a color attachment written by an earlier draw,
# it is changed to read the color directly from the tile (framebuffer fetch); the two render passes become one, and the intermediate attachment need not be stored to main memory and loaded back just to be sampled.
# Built offline; at runtime only function-constant specialization is done (no source compilation). Usage: variant(msl, bindings, om_only_ok=False) → (new MSL, metadata) or (None, reason)
# Function constant numbering: FX_i (which color index the i-th rewritable sample reads, -1 = sample as usual) = i; FB_k (enable color input k) = 32 + k;
#               OM_j_k (the j-th output writes to attachment k) = 64 + 8 j + k
# Rewritable samples: .sample(sampler, select(_N, float2(0.0), isnan(_N))) on a 2D float texture, where _N = float2(in.m_X.x, in.m_X.y) (interpolated output unchanged);
# whether the texel is really fetched at this pixel is proven at runtime (correspondence between vertex uv and position, texture size = target size); if it cannot be proven, the original version is used
import re

NFB = 8

def variant(msl, bindings, om_only_ok=False):
    # main0_u (dmsh_build.py int_outputs: integer colour targets) is appended to the source after main0; the host takes it from the shader's own library, never from this variant.
    # Its body reads main0's output struct, whose members the variant renames, and its samples were rewritten to fb0–fb7, which only main0 declares: 483 of Breath of the
    # Wild's variants failed to compile once their cached copies were gone (use of undeclared identifier 'fb0'). The variant is built from the source without it
    cut = msl.find('\nstruct main0_out_u\n')
    if cut >= 0: msl = msl[:cut + 1]
    tex = {m.group(1) for m in re.finditer(r'texture2d<float> (\w+) \[\[id\(\d+\)\]\];', msl)}
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'float4 (m_\d+) \[\[user\(locn(\d+)\)\]\];', msl.split('struct main0_in', 1)[-1].split('};', 1)[0])}
    reg = {b['msl_name']: b['reg'] for b in bindings if b.get('kind') == 'tex' and b.get('msl_name') and b.get('reg') is not None}
    pat = re.compile(r'spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+)Smplr, select\((_\d+), float2\(0\.0\), isnan\(\3\)\)\)')
    calls, out_parts, pos = [], [], 0
    for m in pat.finditer(msl):
        member, var = m.group(1), m.group(3)
        if member not in tex or member != m.group(2) or member not in reg: continue
        d = re.search(r'float2 %s = float2\((_\d+), (_\d+)\);' % var, msl)
        a = d and re.search(r'float %s = in\.(m_\d+)\.x;' % d.group(1), msl); b = d and re.search(r'float %s = in\.(m_\d+)\.y;' % d.group(2), msl)
        if not (a and b) or a.group(1) != b.group(1) or a.group(1) not in locn: continue
        i = len(calls)
        if i >= 32: break
        sel = ''.join(f'FX_{i} == {k} ? fb{k} : ' for k in range(NFB))
        out_parts.append(msl[pos:m.start()]); out_parts.append(f'({sel}{m.group(0)})'); pos = m.end()
        calls.append({'fc': i, 'tex_reg': reg[member], 'varying_locn': locn[a.group(1)]})
    if not calls and not om_only_ok:
        return None, 'no rewritable sample'
    # Mixed-use slots: the same texture also has other (non-rewritable) uses - at runtime this texture must not be switched to tile reads (the rewritten reads and the unrewritten ones would not see the same data)
    conv = {}
    for c in calls: conv[c['tex_reg']] = conv.get(c['tex_reg'], 0) + 1
    mixed = sorted({reg[mb] for mb in reg if mb in tex and msl.count(f'spvDescriptorSet0.{mb}.') > conv.get(reg[mb], 0)})
    body = ''.join(out_parts) + msl[pos:]
    om = re.search(r'struct main0_out\s*\{(.*?)\};', body, re.S)
    if not om: return None, 'no output struct'
    outs = [(t, n, int(c)) for t, n, c in re.findall(r'(\w+) (m_\d+) \[\[color\((\d+)\)\]\];', om.group(1))]
    if not outs or len(outs) > 4 or re.sub(r'\w+ m_\d+ \[\[color\(\d+\)\]\];', '', om.group(1)).strip(): return None, 'the output struct is not plain color outputs (or has more than 4)'
    members = ''.join(f'    {t} {n}_at{k} [[color({k}), function_constant(OM_{j}_{k})]];\n' for j, (t, n, c) in enumerate(outs) for k in range(NFB))
    body = body[:om.start(1)] + '\n' + members + body[om.end(1):]
    consts = ''.join(f'constant int FX_{c["fc"]} [[function_constant({c["fc"]})]];\n' for c in calls) + \
             ''.join(f'constant bool FB_{k} [[function_constant({32 + k})]];\n' for k in range(NFB)) + \
             ''.join(f'constant bool OM_{j}_{k} [[function_constant({64 + 8 * j + k})]];\n' for j in range(len(outs)) for k in range(NFB))
    body = body.replace('using namespace metal;\n', 'using namespace metal;\n\n' + consts, 1)
    i0 = body.find('fragment main0_out main0(')
    i1 = body.find(')\n{', i0) if i0 >= 0 else -1   # end of signature: closing parenthesis immediately followed by a newline and an opening brace (parameter attributes contain brackets such as [[buffer(0)]])
    if i0 < 0 or i1 < 0: return None, 'main function signature not recognized'
    fbs = ''.join(f', float4 fb{k} [[color({k}), function_constant(FB_{k})]]' for k in range(NFB))
    if body[i1 - 1] == '(': fbs = fbs[2:]   # main0() with no parameters at all: no leading comma (30 of Breath of the Wild's output-mapping-only variants)
    body = body[:i1] + fbs + body[i1:]
    decl = ''.join(f'    {t} spv_o_{n} = {t}(0);\n' for t, n, c in outs)
    body = body.replace('    main0_out out = {};\n', '    main0_out out = {};\n' + decl, 1)
    for t, n, c in outs: body = body.replace(f'out.{n}', f'spv_o_{n}')
    put = ''.join(f'    if (OM_{j}_{k}) out.{n}_at{k} = spv_o_{n};\n' for j, (t, n, c) in enumerate(outs) for k in range(NFB))
    if body.count('    return out;\n') < 1: return None, 'return out not found'
    body = body.replace('    return out;\n', put + '    return out;\n')
    return body, {'calls': calls, 'mixed_regs': mixed, 'outputs': [{'color': c, 'type': t} for t, n, c in outs]}

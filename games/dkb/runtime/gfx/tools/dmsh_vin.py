# Shadow vertex inputs (<key>_bv): dmsh_build.py declares a vertex shader input an integer when its first translation only bit-casts it (int_inputs), and the game may still feed it
# a format of another class: a float-type format (half, unorm / snorm, 10:10:10:2, 11:11:10), whose converted float32 bits Maxwell's fetch puts in the register, or an 8 / 16-bit
# integer of the other signedness, which Maxwell widens by the format's own sign. Metal's fetch reads a format only into an input of its own class, so the host converted those
# attributes on the CPU every draw (nvn_mtl.m vconv_bind; about a quarter of Age of Calamity's replay thread in battle). This variant declares every integer input also as a float4
# and as the integer of the other sign, at Metal attribute indices 16-30 (NVN attributes are 0-15), and ORs the three into the input the body reads before it runs. The host sends each
# attribute to the index of its format's class and the zero buffer to the others (nvn_mtl.m vin_ct), so the shader sees Maxwell's register bits, whatever mix of formats a pipeline has.
# Built offline into a library of its own beside <key>_b, with the same argument buffer; entry points main0 / main0_z (the transform feedback entry is left out: those draws keep
# the CPU conversion). Usage: variant(msl) → (new MSL, [{'location', 'float', 'sint' | 'uint'}]) or (None, reason)
import re

SLOT0, SLOT_END = 16, 31   # Metal vertex attribute indices 16..30

def variant(msl):
    m = re.search(r'\nstruct main0_in\n\{\n(.*?)\n\};', msl, re.S)
    if not m: return None, ('no main0_in struct' if '[[stage_in]]' in msl else 'no integer input')   # no stage input: the shader reads no vertex attribute
    members = []
    for ln in m.group(1).split('\n'):
        mm = re.fullmatch(r'    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];', ln)   # m_28_0: an input array split per element
        if not mm: return None, 'an input declaration not understood: ' + ln.strip()[:60]
        members.append((mm.group(1), mm.group(2), int(mm.group(3))))
    ints = [x for x in members if x[0] != 'float4']
    if not ints: return None, 'no integer input'
    if any(l >= SLOT0 for _, _, l in members): return None, 'an input at attribute 16 or above'
    slot, shadows, extra = SLOT0, [], []
    for t, n, l in ints:   # a float4 shadow for every integer input first (half and normalized formats are the common case) ...
        if slot >= SLOT_END: break
        extra.append(f'    float4 {n}_vf [[attribute({slot})]];'); shadows.append({'location': l, 'float': slot}); slot += 1
    for e, (t, n, l) in zip(shadows, ints):   # ... then the other signedness while indices last
        if slot >= SLOT_END: break
        o = 'int4' if t == 'uint4' else 'uint4'
        extra.append(f'    {o} {n}_vo [[attribute({slot})]];'); e['sint' if o == 'int4' else 'uint'] = slot; slot += 1
    covered = {e['location']: e for e in shadows}
    head = msl[:m.end()] + '\n\nstruct main0_in_v\n{\n' + m.group(1) + '\n' + '\n'.join(extra) + '\n};'
    body = msl[m.end():]
    cut = body.find('\nvertex void main0_tf(')
    if cut >= 0: body = body[:cut] + '\n'
    pro = '    main0_in in = {};\n'
    for t, n, l in members:
        e = covered.get(l)
        ors = '' if not e else f' | as_type<{t}>(dm_in.{n}_vf)' + (f' | as_type<{t}>(dm_in.{n}_vo)' if ('sint' in e or 'uint' in e) else '')
        pro += f'    in.{n} = dm_in.{n}{ors};\n'
    n_entry = 0
    for entry in ('main0', 'main0_z'):
        hdr = re.search(r'\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]([^\n]*)\)\n\{\n' % entry, body)
        if not hdr:
            if entry == 'main0': return None, 'main0 signature not recognized'
            continue
        new = f'\nvertex main0_out {entry}(main0_in_v dm_in [[stage_in]]{hdr.group(1)})\n{{\n' + pro
        body = body[:hdr.start()] + new + body[hdr.end():]; n_entry += 1
    return head + body, shadows

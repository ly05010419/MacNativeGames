# Alpha test (<key>_ba): NVN's ColorStateSetAlphaTest and SetAlphaRef compare the alpha a pixel shader writes to colour 0 with a reference, before blending, and drop the fragment
# when the comparison fails (Maxwell's fixed-function alpha test, as in OpenGL's compatibility profile). Metal has none. Foliage, hair, fences and dissolve effects draw with it
# (Dark Souls Remastered, ASTRAL CHAIN, Animal Crossing's island map). This variant is the pixel shader with every fragment entry point (main0 and the main0_u / main0_m / main0_d
# dmsh_build.py adds) taking the test's function and reference from a constant buffer at fragment buffer DMSH_ALPHA_BUFFER, and testing colour 0's alpha before each return.
# The function and reference are data, not part of the code: one pipeline serves every function, and a reference that changes every draw (a dissolve) keeps it. Built offline
# into a library of its own beside <key>_b, with the same argument buffer and entry point names; the host loads it only for pipelines drawn with an alpha function other than
# ALWAYS (nvn_mtl.m at_fn), so draws without the test keep the base library as it was. [[early_fragment_tests]] stays: a shader that forces early depth tests has its depth written before the alpha test, on Maxwell too.
# Usage: variant(msl) → (new MSL, the entry point names) or (None, reason)
import re
import dmsh_abi

HELPER = '''
struct dm_alpha_state { uint func; float ref; };   // NVN_ALPHA_FUNC_* and the reference (dmsh_alpha.py)
static inline bool dm_alpha_pass(float a, constant dm_alpha_state& s)
{
    switch (s.func)
    {
        case 1u: return false;
        case 2u: return a < s.ref;
        case 3u: return a == s.ref;
        case 4u: return a <= s.ref;
        case 5u: return a > s.ref;
        case 6u: return a != s.ref;
        case 7u: return a >= s.ref;
        default: return true;
    }
}
'''
ENTRY = re.compile(r'^[^\n]*?\bfragment\s+\w+\s+(\w+)\s*\(', re.M)   # an attribute may come first on the line ([[ early_fragment_tests ]])


def variant(msl):
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    if not st: return None, 'no colour output'
    col = re.search(r'^\s*(float4|half4)\s+(\w+)\s*\[\[color\(0\)\]\];', st.group(1), re.M)
    if not col:   # no colour 0, or one written as an integer or with fewer than four components: the test has no alpha to read
        return None, 'colour 0 has no float alpha' if re.search(r'\[\[color\(0\)\]\]', st.group(1)) else 'no colour 0 output'
    if 'main0_out out = {};' not in msl: return None, 'no output variable out'
    entries = list(ENTRY.finditer(msl))
    if not entries or entries[0].group(1) != 'main0': return None, 'no fragment entry point main0'
    if f'[[buffer({dmsh_abi.ALPHA_BUFFER})]]' in msl: return None, f'fragment buffer {dmsh_abi.ALPHA_BUFFER} is taken'
    param = f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER})]]'
    out, at = [], 0
    for m in entries:   # the alpha state as each entry point's last parameter; parameter attributes hold parentheses ([[buffer(0)]]), so the list ends where they balance
        i, depth = m.end(), 1
        while depth and i < len(msl): depth += (msl[i] == '(') - (msl[i] == ')'); i += 1
        if depth: return None, 'an unclosed parameter list'
        empty = not msl[m.end():i - 1].strip()
        out += [msl[at:i - 1], ('' if empty else ', ') + param]; at = i - 1
    src = ''.join(out) + msl[at:]
    first = entries[0].start()   # the text up to the first entry point's parameter list is msl's, so its offset holds in src
    head, body = src[:first], src[first:]
    # Every return of an entry point returns the shader's outputs, out, or a copy of them made from out (main0_u, main0_m, main0_d): colour 0's alpha is out's before the copy.
    # SPIR-V Cross writes helper functions before main0, so every return after it is an entry point's
    test = f'if (!dm_alpha_pass(float(out.{col.group(2)}.w), dm_alpha)) discard_fragment();'
    body, n = re.subn(r'\breturn\b([^;]*);', lambda r: '{ ' + test + ' return' + r.group(1) + '; }', body)
    if not n: return None, 'no return'
    return head + HELPER + body, [m.group(1) for m in ENTRY.finditer(body)]

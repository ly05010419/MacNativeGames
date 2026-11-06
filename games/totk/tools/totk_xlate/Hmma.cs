// Ampere HMMA.16816.F16 lowered to Metal SIMD-group matrices.
// From seedds/nxnative dm/gfx/tools/dmsh_build.py hmma_msl / HMMA_BODY (MIT).
// Keep the translator helper's signature and scratch layout; native half accumulators
// preserve the instruction's rounding and avoid the scalar helper's long FMA chains.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

static class ShaderHmma
{
    const string Body = """
{
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
}
""";
    public static string Rewrite(string src)
    {
        var m = Regex.Match(src, @"\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{");
        if (!m.Success) return src;
        var parameters = new List<string>();
        var current = new StringBuilder(); int depth = 0;
        foreach (char c in m.Groups[2].Value + ",")
        {
            if (c == '<') depth++; else if (c == '>') depth--;
            if (c == ',' && depth == 0) { parameters.Add(current.ToString().Trim()); current.Clear(); }
            else current.Append(c);
        }
        var scratch = parameters.Where(p => p.StartsWith("threadgroup ", StringComparison.Ordinal)).ToArray();
        var plain = parameters.Where(p => !p.StartsWith("threadgroup ", StringComparison.Ordinal)).ToArray();
        if (scratch.Length != 1 || plain.Length != 11) throw new InvalidOperationException("HMMA helper signature changed");
        string Name(string p) { var n = Regex.Match(p, @"(\w+)$"); if (!n.Success) throw new InvalidOperationException("HMMA parameter name missing"); return n.Value; }
        var output = Regex.Match(plain[10], @"^thread (\w+)&");
        if (!output.Success) throw new InvalidOperationException("HMMA output parameter changed");
        var names = new Dictionary<string, string>();
        string[] keys = {"lane", "blk", "a0", "a1", "a2", "a3", "b0", "b1", "acc0", "acc1", "d1"};
        for (int n = 0; n < keys.Length; n++) names[keys[n]] = Name(plain[n]);
        names["scr"] = Name(scratch[0]); names["dt"] = output.Groups[1].Value; names["rt"] = m.Groups[1].Value;
        int start = m.Index + m.Length - 1, end = start; depth = 0;
        for (; end < src.Length; end++) { if (src[end] == '{') depth++; else if (src[end] == '}' && --depth == 0) break; }
        if (end == src.Length) throw new InvalidOperationException("HMMA helper body is not closed");
        string body = Regex.Replace(Body, @"%\((\w+)\)s", p => names[p.Groups[1].Value]);
        return src[..start] + body + src[(end + 1)..];
    }
}

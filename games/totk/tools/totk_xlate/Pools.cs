using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

// Ryujinx's arrays for a texture handle it cannot trace to one slot (<stage>_<t|s>_cb0_40000000, indexed by the handle's texture / sampler ID), sized to the whole ID ranges
// (Program.cs QueryTextureArrayLengthFromPool) -> a pointer to a table the replay keeps per element type (nvn_mtl.m pool_table): struct spvPool_<name> { uint n; ulong seen; array<element, 1> e; },
// read as (&e[0])[min(index, n)], n being the highest ID the host filled. The struct declares one element because Metal's argument encoder walks every element of a declared array.
// The argument IDs are renumbered in member order, a pointer taking one and any other array its length. Port of seedds/nxnative dmsh_build.py pool_arrays (MIT).
static class ShaderPools
{
    const int PoolMin = 33;   // arrays at least this long are pools
    // A read of a texture pool sets the ID's bit in the bitmap at seen, as which IDs a shader reads is known only here: the host makes those textures instead of all of them. The word is read
    // first: a bit is set once, so nearly every read costs a load, not an atomic (nxnative dmsh_build.py POOL_MARK, MIT)
    const string PoolMark = "template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)\n{\n    device atomic_uint* w = reinterpret_cast<device atomic_uint*>(p->seen) + (i >> 5);\n    uint b = 1u << (i & 31u);\n    if (!(atomic_load_explicit(w, memory_order_relaxed) & b)) atomic_fetch_or_explicit(w, b, memory_order_relaxed);\n    return i;\n}\n\n";

    public static string Rewrite(string src)
    {
        Match body = Regex.Match(src, @"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", RegexOptions.Singleline);
        if (!body.Success) return src;
        var pools = new List<(string Name, string Element)>();
        string members = Regex.Replace(body.Groups[1].Value, @"^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(", m =>
        {
            if (!m.Groups[4].Value.EndsWith("_cb0_40000000", StringComparison.Ordinal) || long.Parse(m.Groups[3].Value) < PoolMin) return m.Value;
            pools.Add((m.Groups[4].Value, m.Groups[2].Value));
            return $"{m.Groups[1].Value}constant spvPool_{m.Groups[4].Value}* {m.Groups[4].Value} [[id(";
        }, RegexOptions.Multiline);
        if (pools.Count == 0) return src;
        long aid = 0;
        members = Regex.Replace(members, @"^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];", m =>
        {
            Match a = Regex.Match(m.Groups[2].Value.Trim(), @"^array<.+, (\d+)>$");
            string line = $"{m.Groups[1].Value}{m.Groups[2].Value} {m.Groups[3].Value} [[id({aid}){m.Groups[5].Value}]];";
            aid += a.Success ? long.Parse(a.Groups[1].Value) : 1;
            return line;
        }, RegexOptions.Multiline);
        var structs = new StringBuilder();
        foreach ((string nm, string el) in pools) structs.Append($"struct spvPool_{nm}\n{{\n    uint n;\n    ulong seen;\n    array<{el}, 1> e;\n}};\n\n");
        bool marks = pools.Any(q => q.Element != "sampler");
        if (marks) structs.Insert(0, PoolMark);
        src = src.Substring(0, body.Index) + structs + src.Substring(body.Index, body.Groups[1].Index - body.Index) + members + src.Substring(body.Groups[1].Index + body.Groups[1].Length);
        foreach ((string nm, string el) in pools)
        {
            // base.name[index] -> (&base.name->e[0])[min(uint(index), base.name->n)], the index taken with its brackets balanced
            var o = new StringBuilder(); int pos = 0;
            foreach (Match m in Regex.Matches(src, @"([\w.]+)\." + Regex.Escape(nm) + @"\["))
            {
                if (m.Index < pos) continue;
                int depth = 1, j = m.Index + m.Length;
                while (depth > 0)
                {
                    if (j >= src.Length) throw new InvalidOperationException(nm + ": unbalanced index");
                    if (src[j] == '[') depth++; else if (src[j] == ']') depth--;
                    j++;
                }
                string @base = m.Groups[1].Value + "." + nm;
                string index = src.Substring(m.Index + m.Length, j - 1 - (m.Index + m.Length));
                o.Append(src, pos, m.Index - pos).Append(el == "sampler" ? $"(&{@base}->e[0])[min(uint({index}), {@base}->n)]" : $"(&{@base}->e[0])[dm_pool_mark({@base}, min(uint({index}), {@base}->n))]");
                pos = j;
            }
            src = o.Append(src, pos, src.Length - pos).ToString();
        }
        // The mark stores to memory, so Metal would test this fragment shader against depth after running it: every covered pixel shaded. It is the host's own, so it must not move the test: early,
        // unless the shader discards or writes depth itself, or stores anything of its own, where it stays as the game's shader has it (nxnative pool_arrays, MIT)
        if (marks && !Regex.IsMatch(src.Replace(PoolMark, ""), @"\bdiscard_fragment\b|\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s", RegexOptions.Multiline))
            src = Regex.Replace(src, @"^(?<!\]\] )fragment\s", "[[early_fragment_tests]] fragment ", RegexOptions.Multiline);
        return src;
    }
}

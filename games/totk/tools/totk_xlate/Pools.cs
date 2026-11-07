using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

// Ryujinx's arrays for a texture handle it cannot trace to one slot (<stage>_<t|s>_cb0_40000000, indexed by the handle's texture / sampler ID), sized to the whole ID ranges
// (Program.cs QueryTextureArrayLengthFromPool) -> a pointer to a table the replay keeps per element type (nvn_mtl.m pool_table): struct spvPool_<name> { uint n; array<element, 1> e; },
// read as (&e[0])[min(index, n)], n being the highest ID the host filled. The struct declares one element because Metal's argument encoder walks every element of a declared array.
// The argument IDs are renumbered in member order, a pointer taking one and any other array its length. Port of seedds/nxnative dmsh_build.py pool_arrays (MIT).
static class ShaderPools
{
    const int PoolMin = 33;   // arrays at least this long are pools

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
        foreach ((string nm, string el) in pools) structs.Append($"struct spvPool_{nm}\n{{\n    uint n;\n    array<{el}, 1> e;\n}};\n\n");
        src = src.Substring(0, body.Index) + structs + src.Substring(body.Index, body.Groups[1].Index - body.Index) + members + src.Substring(body.Groups[1].Index + body.Groups[1].Length);
        foreach ((string nm, _) in pools)
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
                o.Append(src, pos, m.Index - pos).Append($"(&{@base}->e[0])[min(uint({index}), {@base}->n)]");
                pos = j;
            }
            src = o.Append(src, pos, src.Length - pos).ToString();
        }
        return src;
    }
}

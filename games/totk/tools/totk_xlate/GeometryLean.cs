// The lean kernel of a passthrough geometry shader, ported from seedds/nxnative dm/gfx/tools/dmsh_geometry.py lean (MIT).
// A passthrough geometry shader (SPH word 0 bit 24) cannot change the vertices it passes on: it decides per primitive
// (the viewport mask) and may write storage. Its lean kernel keeps exactly that: of the output vertex it stores only the
// viewport mask, one word per output vertex, and it loads only the capture words it still reads, from a record of that
// many words in increasing word order. The host then draws the game's own vertex shader through the kept primitives'
// original vertex numbers (nvn_geometry_rt.inc), instead of capturing 144 words a vertex, writing the whole output vertex
// and reading it back through the reader.
// Input reads and output stores must be the translator's literal offsets; anything else is a ShaderGeometry.GeometryError and the host
// keeps the full kernel.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

static class ShaderGeometryLean
{
    static readonly Regex Def = new(@"^\s*(?:int|uint|float|bool) (_\d+) = (.+);$");
    static readonly Regex Pure = new(@"atomic|Subgroup|simd_|\+\+|--|[^=!<>]=[^=]");   // an expression with an effect
    static readonly Regex Store = new(@"^\s*\(\*spvDescriptorSet0\.geometry_vb_output\)\._m0\[(_\d+)\] = (_\d+);$");
    static readonly Regex CaptureRead = new(@"= \(\*spvDescriptorSet0\.vertex_output\)\._m0\[(_\d+)\];");
    static readonly Regex ArrayDecl = new(@"^\s*spvUnsafeArray<\w+, \d+> (_\d+);$");
    static readonly Regex ArrayStore = new(@"^\s*(_\d+)\[(\d+)\] = (.+);$");
    static readonly Regex Token = new(@"_\d+(?:\[\d+\])?");

    /// (source, reads): the lean kernel, and the capture words it reads (its record holds them in this order).
    public static (string, int[]) Lean(string src, int outStride)
    {
        string[] lines = src.Split('\n');
        var defs = new Dictionary<string, (int line, string expr)>();
        for (int i = 0; i < lines.Length; i++)
        {
            Match m = Def.Match(lines[i]);
            if (m.Success) defs.TryAdd(m.Groups[1].Value, (i, m.Groups[2].Value));
        }
        string Expr(string v) => defs.TryGetValue(v, out var d) ? d.expr : "";
        string Base(string v, int stride)   // v = _X * stride -> _X
        {
            Match m = Regex.Match(Expr(v), $@"^(_\d+) \* {stride}$");
            return m.Success ? m.Groups[1].Value : null;
        }
        (string, int) Offset(string v, int stride)   // v = _B + K with _B = _X * stride (or v itself) -> (_B, K)
        {
            Match m = Regex.Match(Expr(v), @"^(_\d+) \+ (\d+)$");
            if (m.Success && Base(m.Groups[1].Value, stride) != null) return (m.Groups[1].Value, int.Parse(m.Groups[2].Value));
            if (Base(v, stride) != null) return (v, 0);
            return (null, -1);
        }

        // Output stores: keep the viewport mask's, now at the output vertex's number; drop the others
        int kept = 0;
        for (int i = 0; i < lines.Length; i++)
        {
            string ln = lines[i];
            if (!ln.Contains("geometry_vb_output", StringComparison.Ordinal) || ln.TrimStart().StartsWith("device ", StringComparison.Ordinal)) continue;
            Match m = Store.Match(ln);
            if (!m.Success) throw new ShaderGeometry.GeometryError("lean geometry: unknown output store");
            (string b, int k) = Offset(m.Groups[1].Value, outStride);
            if (b == null) throw new ShaderGeometry.GeometryError("lean geometry: unknown output store");
            if (k != ShaderGeometry.MaskWord) { lines[i] = ""; continue; }
            int j = defs[m.Groups[1].Value].line;
            lines[j] = Regex.Replace(lines[j], @"= .+;$", $"= {Base(b, outStride)};");
            kept++;
        }
        if (kept == 0) throw new ShaderGeometry.GeometryError("lean geometry: no viewport mask store");

        // Remove what only fed the removed stores: unused pure definitions, and local array elements written at a
        // literal index and never read
        var arrays = new HashSet<string>(lines.Select(l => ArrayDecl.Match(l)).Where(m => m.Success).Select(m => m.Groups[1].Value));
        string nonDecl = string.Join("\n", lines.Where(l => !ArrayDecl.IsMatch(l)));
        foreach (string a in arrays.ToList())
            if (Regex.IsMatch(nonDecl, @"\b" + a + @"\b(?!\[\d+\])")) arrays.Remove(a);   // used whole or at a computed index: keep every element
        int body = Array.FindIndex(lines, l => l.StartsWith("kernel void main0(", StringComparison.Ordinal));
        if (body < 0) throw new ShaderGeometry.GeometryError("lean geometry: no kernel entry");
        var target = new string[lines.Length];
        var rvalues = new List<string>[lines.Length];
        var uses = new Dictionary<string, int>();
        var writers = new Dictionary<string, List<int>>();
        for (int i = 0; i < lines.Length; i++)
        {
            rvalues[i] = new List<string>();
            if (i <= body) continue;   // type names (struct _22) are not uses of a variable _22
            string ln = lines[i], expr = ln;
            Match m = Def.Match(ln);
            Match a = m.Success ? Match.Empty : ArrayStore.Match(ln);
            if (m.Success && !Pure.IsMatch(m.Groups[2].Value)) { target[i] = m.Groups[1].Value; expr = m.Groups[2].Value; }
            else if (a.Success && arrays.Contains(a.Groups[1].Value) && !Pure.IsMatch(a.Groups[3].Value)) { target[i] = $"{a.Groups[1].Value}[{a.Groups[2].Value}]"; expr = a.Groups[3].Value; }
            foreach (Match t in Token.Matches(expr))
            {
                string tok = t.Value;
                if (arrays.Contains(tok.Split('[')[0]) && !tok.Contains('[')) continue;
                rvalues[i].Add(tok);
                uses[tok] = uses.GetValueOrDefault(tok) + 1;
            }
            if (target[i] != null) { if (!writers.TryGetValue(target[i], out var w)) writers[target[i]] = w = new List<int>(); w.Add(i); }
        }
        var work = new Stack<int>(Enumerable.Range(0, lines.Length).Where(i => target[i] != null && uses.GetValueOrDefault(target[i]) == 0));
        while (work.Count > 0)
        {
            int i = work.Pop();
            if (lines[i] == "") continue;
            lines[i] = "";
            foreach (string t in rvalues[i])
            {
                uses[t]--;
                if (uses[t] == 0 && writers.TryGetValue(t, out var w)) foreach (int x in w) work.Push(x);
            }
        }

        // The remaining capture reads: literal words of the 144-word record, renumbered
        string text = string.Join("\n", lines);
        MatchCollection found = CaptureRead.Matches(text);
        if (Regex.Matches(text, "vertex_output").Count != found.Count + 1) throw new ShaderGeometry.GeometryError("lean geometry: unknown capture read");
        var reads = new List<(string n, string b, int k)>();
        foreach (Match f in found)
        {
            string n = f.Groups[1].Value;
            (string b, int k) = Offset(n, ShaderGeometry.Words);
            if (b == null) throw new ShaderGeometry.GeometryError("lean geometry: computed capture read");
            reads.Add((n, b, k));
        }
        int[] words = reads.Select(r => r.k).Distinct().OrderBy(k => k).ToArray();
        var slot = words.Select((k, i) => (k, i)).ToDictionary(p => p.k, p => p.i);
        var bases = reads.Select(r => r.b).Distinct().ToList();
        foreach (string b in bases)   // each record base must serve only these reads
            if (Regex.Matches(text, @"\b" + b + @"\b").Count - 1 > reads.Count(r => r.b == b))
                throw new ShaderGeometry.GeometryError("lean geometry: record base used elsewhere");
        foreach (var (n, b, k) in reads)
            if (n != b) { int j = defs[n].line; lines[j] = Regex.Replace(lines[j], @"\+ \d+;$", $"+ {slot[k]};"); }
        foreach (string b in bases)
        {
            int j = defs[b].line;
            lines[j] = Regex.Replace(lines[j], $@"\* {ShaderGeometry.Words};$", $"* {Math.Max(words.Length, 1)};");
        }
        return (string.Join("\n", lines), words);
    }
}

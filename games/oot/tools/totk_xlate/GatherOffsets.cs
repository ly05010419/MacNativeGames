// textureGatherOffsets with its four offsets written out.
// Ported from seedds/nxnative (MIT), dm/gfx/tools/dmsh_rewrite.py gather_offsets / gather_quad and docs/reference/shaders.md
// "textureGatherOffsets and a loop over its offsets" (commit 0cae4830, 2026-11-10).
//
// spirv-cross's spvGatherConstOffsets passes coffsets[i] from a loop to gather(), whose offset Metal takes only as a
// constant expression. The loop stays, the compiler takes it without a word, and the gathers read texels at offsets that
// differ between lanes and between runs (nxnative: about 40 % of the pixels of one captured dispatch of b106afba1a18faa9
// differed between three runs). agl's NDSR upscaler builds its input tile this way: 32 of Ocarina of Time's shaders, 208
// calls (counted in the v24 seed, 2026-11-10).
// Each call becomes its gathers with the offsets written out, from the constant array the call names. Offsets that form a
// 2x2 square (b, b+(1,0), b+(0,1), b+(1,1), in any order) take one gather at b: each offset's texel is the (i0, j0) corner of
// its own footprint, and with integer offsets those corners are the four texels of b's footprint (Metal's order: x (i0, j1),
// y (i1, j1), z (i1, j0), w (i0, j0)). A call whose array is not such a constant stops the translation.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

static class GatherOffsets
{
    const string Gather4 = "template<typename T> static inline __attribute__((always_inline)) vec<T, 4> dm_gather4(T a, T b, T c, T d) { return vec<T, 4>(a, b, c, d); }\n";
    static readonly Regex Used = new(@"\bspvGather(Compare)?ConstOffsets\(spvDesc|= spvGather(Compare)?ConstOffsets\(", RegexOptions.Compiled);
    static readonly Regex Array = new(@"\bconstant spvUnsafeArray<int2, 4> (\w+) = spvUnsafeArray<int2, 4>\(\{ (.*?) \}\);", RegexOptions.Compiled);
    static readonly Regex Offset = new(@"int2\([^()]*\)", RegexOptions.Compiled);
    static readonly Regex Call = new(@"\bspvGather(Compare)?ConstOffsets\((?=[^)]*?\w)", RegexOptions.Compiled);
    static readonly Regex Int2 = new(@"^int2\(\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*)?\)$", RegexOptions.Compiled);
    static readonly Dictionary<(int, int), char> Quad = new() { [(0, 0)] = 'w', [(1, 0)] = 'z', [(0, 1)] = 'x', [(1, 1)] = 'y' };

    /// <summary>The square's corner as MSL and the swizzle that puts its texels in the offsets' order, or null.</summary>
    static (string corner, string swizzle)? Square(string[] offsets)
    {
        var xy = new List<(int x, int y)>();
        foreach (string o in offsets)
        {
            Match m = Int2.Match(o);
            if (!m.Success) return null;
            int x = int.Parse(m.Groups[1].Value);
            xy.Add((x, m.Groups[2].Success ? int.Parse(m.Groups[2].Value) : x));
        }
        int bx = xy.Min(p => p.x), by = xy.Min(p => p.y);
        var rel = xy.Select(p => (p.x - bx, p.y - by)).ToList();
        if (!rel.OrderBy(r => r).SequenceEqual(Quad.Keys.OrderBy(r => r))) return null;
        return ($"int2({bx}, {by})", new string(rel.Select(r => Quad[r]).ToArray()));
    }

    public static string Rewrite(string msl)
    {
        if (!Used.IsMatch(msl)) return msl;
        var arrays = new Dictionary<string, string[]>();
        foreach (Match m in Array.Matches(msl))
        {
            string[] off = Offset.Matches(m.Groups[2].Value).Select(o => o.Value).ToArray();
            if (off.Length != 4) throw new InvalidOperationException("gather offsets: unexpected array " + Head(m.Value, 120));
            arrays[m.Groups[1].Value] = off;
        }
        var result = new StringBuilder(); int at = 0; bool four = false;
        foreach (Match m in Call.Matches(msl))
        {
            if (m.Index < at || string.CompareOrdinal(msl, m.Index + m.Length, "const ", 0, 6) == 0) continue;   // the wrappers' own definitions
            int depth = 1, j = m.Index + m.Length; var args = new List<string>(); var cur = new StringBuilder();
            while (depth > 0)
            {
                if (j >= msl.Length) throw new InvalidOperationException("gather offsets: unterminated call");
                char ch = msl[j++];
                if (ch == '(') depth++; else if (ch == ')') depth--;
                if (depth == 0 || (ch == ',' && depth == 1)) { args.Add(cur.ToString().Trim()); cur.Clear(); }
                else cur.Append(ch);
            }
            bool compare = m.Groups[1].Success; int first = compare ? 3 : 4;
            if (args.Count <= first || !arrays.TryGetValue(args[2], out string[] offsets))
                throw new InvalidOperationException("gather offsets: unexpected call " + Head(msl.Substring(m.Index, j - m.Index), 160));
            string tex = args[0], sampler = args[1], rest = string.Join(", ", args.Skip(first));
            string tail = compare ? "" : ", " + args[3], fn = compare ? "gather_compare" : "gather";
            result.Append(msl, at, m.Index - at);
            var square = Square(offsets);
            if (square != null) result.Append($"({tex}.{fn}({sampler}, {rest}, {square.Value.corner}{tail}).{square.Value.swizzle})");
            else { four = true; result.Append("dm_gather4(" + string.Join(", ", offsets.Select(o => $"{tex}.{fn}({sampler}, {rest}, {o}{tail}).w")) + ")"); }
            at = j;
        }
        result.Append(msl, at, msl.Length - at);
        string text = result.ToString();
        if (!four) return text;
        int ns = text.IndexOf("using namespace metal;", StringComparison.Ordinal);
        if (ns < 0) throw new InvalidOperationException("gather offsets: no namespace line");
        int line = text.IndexOf('\n', ns) + 1;
        return text.Substring(0, line) + Gather4 + text.Substring(line);
    }

    static string Head(string s, int n) => s.Length <= n ? s : s.Substring(0, n);
}

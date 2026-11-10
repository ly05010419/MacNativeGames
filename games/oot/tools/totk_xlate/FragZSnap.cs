// A pixel shader's gl_FragCoord.z against a depth of its own surface that an earlier pass stored.
// Ported from seedds/nxnative (MIT), dm/gfx/tools/dmsh_rewrite.py frag_z_snap and docs/reference/shaders.md
// "gl_FragCoord.z against the depth stored for its own surface" (commit 0cae4830, 2026-11-10).
//
// NVIDIA stores a fragment's depth as the z its pixel shader reads, so a shader can test a fragment against a depth image
// of its own surface and find them equal. Apple's GPU stores a value below the [[position]].z the fragment shader reads at
// some pixels. Measured here on an M4 (games/oot/tests/frag_z_snap_gpu.m): one flat quad drawn depth-only and then again,
// 5,035 of 230,080 pixels read a z exactly one ulp above the stored depth, none below. nxnative measured at most one ulp
// plus 2^-23 of the depth's step across a pixel, and no gap on an M6.
// Ocarina of Time's ground mist (fragment 5ad65c0eefd09c7f) discards a fragment whose gl_FragCoord.z is above the depth its
// depth-only pass stored, and lost its own surface at scattered pixels: dark specks flickering in the mist (2026-11-10;
// leaving the depth-only pass out removed them).
//
// Where a gl_FragCoord.z read of main0 and a scalar read from a texture meet in one comparison, through any arithmetic on
// either side (z > d; z * k > d * k), the read becomes dm_zsnap(gl_FragCoord.z, d, fwidth(gl_FragCoord.z)): d when it lies
// in the window the stored depth of the fragment's own surface can take, [z - ulp(z) - 2^-20 fwidth(z), z), else z. The
// comparison then sees the equality NVIDIA's does, whatever its operator. A value outside the window, constants, uniforms,
// a texture value read after z or in a block z does not see, and z read in another function stay as they were.
// Not chosen (nxnative's reasons, and the same here): the depth-only pass writing [[position]].z as its depth moves its
// depth test after the shader, and a stored depth other than the rasterizer's fails an EQUAL pass on the same geometry.
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

static class FragZSnap
{
    public const string Helper =
        "static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)   // see FragZSnap.cs\n" +
        "{\n    float lo = as_type<float>(as_type<int>(z) - 1) - zw * 0x1p-20f;\n    return (z > 0.0f && d < z && d >= lo) ? d : z;\n}\n";
    const string Width = "dm_zw";
    static readonly Regex Entry = new(@"\bfragment\s+\S+\s+main0\s*\(", RegexOptions.Compiled);
    static readonly Regex Decl = new(@"^\s*(?:const )?(float|bool|int|uint)\s+(_\d+) = (.+);$", RegexOptions.Compiled);
    static readonly Regex Ref = new(@"\b_\d+\b", RegexOptions.Compiled);
    static readonly Regex TexRead = new(@"\.(sample|read)\(", RegexOptions.Compiled);
    static readonly Regex Compare = new(@"^(_\d+) (?:<=|>=|<|>|==|!=) (_\d+)$", RegexOptions.Compiled);
    static readonly Regex PositionParam = new(@"\bfloat4 gl_FragCoord \[\[position\]\]\s*$", RegexOptions.Compiled);

    /// <summary>The MSL with each paired gl_FragCoord.z read of main0 snapped, and the number of reads rewritten.</summary>
    public static string Rewrite(string msl, out int count)
    {
        count = 0;
        if (!msl.Contains("gl_FragCoord.z")) return msl;
        Match e = Entry.Match(msl);
        if (!e.Success) return msl;
        int close = Matching(msl, e.Index + e.Length - 1, '(', ')');
        if (close < 0) return msl;
        string parameters = msl.Substring(e.Index + e.Length, close - (e.Index + e.Length));
        if (!parameters.Split(',').Any(p => PositionParam.IsMatch(p.Trim()))) return msl;
        int open = msl.IndexOf('{', close);
        int end = open < 0 ? -1 : Matching(msl, open, '{', '}');
        if (end < 0) return msl;
        string[] lines = msl.Substring(open + 1, end - open - 1).Split('\n');
        // each local's gl_FragCoord.z reads and the texture values it is computed from; the blocks it is declared in; its line
        var zr = new Dictionary<string, HashSet<string>>(); var xr = new Dictionary<string, HashSet<string>>();
        var scope = new Dictionary<string, int[]>(); var lineOf = new Dictionary<string, int>();
        var pairs = new Dictionary<string, HashSet<string>>();
        var stack = new List<int> { 0 }; int blocks = 0;
        for (int i = 0; i < lines.Length; i++)
        {
            Match m = Decl.Match(lines[i]);
            if (m.Success)
            {
                string type = m.Groups[1].Value, v = m.Groups[2].Value, ex = m.Groups[3].Value;
                var z = new HashSet<string>(); var x = new HashSet<string>();
                foreach (Match r in Ref.Matches(ex))
                {
                    if (zr.TryGetValue(r.Value, out var rz)) z.UnionWith(rz);
                    if (xr.TryGetValue(r.Value, out var rx)) x.UnionWith(rx);
                }
                if (ex == "gl_FragCoord.z") z = new HashSet<string> { v };
                else if (type == "float" && TexRead.IsMatch(ex) && z.Count == 0) x = new HashSet<string> { v };   // a scalar read from a texture; not sample_compare or gather
                zr[v] = z; xr[v] = x; scope[v] = stack.ToArray(); lineOf[v] = i;
                Match c = type == "bool" ? Compare.Match(ex) : Match.Empty;
                if (c.Success)
                {
                    string a = c.Groups[1].Value, b = c.Groups[2].Value;
                    foreach (var (zs, xs) in new[] { (a, b), (b, a) })
                        if (Has(zr, zs) && Has(xr, xs) && !Has(xr, zs) && !Has(zr, xs))
                            foreach (string zv in zr[zs])
                            {
                                if (!pairs.TryGetValue(zv, out var set)) pairs[zv] = set = new HashSet<string>();
                                set.UnionWith(xr[xs]);
                            }
                }
            }
            foreach (char ch in lines[i])
            {
                if (ch == '{') { blocks++; stack.Add(blocks); }
                else if (ch == '}' && stack.Count > 1) stack.RemoveAt(stack.Count - 1);
            }
        }
        foreach (string zv in pairs.Keys.OrderBy(v => lineOf[v]))
        {
            // declared before the read, in a block it sees
            var xs = pairs[zv].OrderBy(v => lineOf[v]).Where(x => lineOf[x] < lineOf[zv] && scope[zv].Take(scope[x].Length).SequenceEqual(scope[x])).ToList();
            if (xs.Count == 0) continue;
            string expr = "gl_FragCoord.z";
            foreach (string x in xs) expr = $"dm_zsnap({expr}, {x}, {Width})";
            string was = $"{zv} = gl_FragCoord.z;";
            if (!lines[lineOf[zv]].Contains(was)) continue;
            lines[lineOf[zv]] = lines[lineOf[zv]].Replace(was, $"{zv} = {expr};"); count++;
        }
        if (count == 0) return msl;
        string body = $"\n    float {Width} = fwidth(gl_FragCoord.z);" + string.Join("\n", lines);   // in uniform control flow, before anything the body does
        string result = msl.Substring(0, open + 1) + body + msl.Substring(end);
        const string ns = "using namespace metal;\n";
        int at = result.IndexOf(ns, System.StringComparison.Ordinal);
        if (at < 0) { count = 0; return msl; }
        return result.Substring(0, at + ns.Length) + "\n" + Helper + result.Substring(at + ns.Length);
    }

    static bool Has(Dictionary<string, HashSet<string>> map, string key) => map.TryGetValue(key, out var set) && set.Count > 0;

    static int Matching(string s, int open, char up, char down)
    {
        int depth = 0;
        for (int i = open; i < s.Length; i++)
        {
            if (s[i] == up) depth++;
            else if (s[i] == down && --depth == 0) return i;
        }
        return -1;
    }
}

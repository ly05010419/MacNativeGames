// Switch 2 (and Maxwell) geometry shaders as Metal compute kernels: the MSL / SPIR-V steps around the translator's
// geometry-as-compute lowering (Program.cs Translate with stage "gs", patch 0007). Ported from seedds/nxnative
// dm/gfx/tools/dmsh_geometry.py and dmsh_build.py geometry_finish (bb585cd5, 31b5944e, 2c31212f; MIT, see LICENSE).
//
// ABI (shared with the host):
//  - The vertex stage runs first as a capture pass: a vertex shader translated with "geometry_capture" gets a second entry
//    point, main0_gs (Capture), a rasterizer-discard vertex function that writes each vertex's outputs as one record of
//    up to 144 words in the Maxwell attribute map: position 0..3, point size 4, clip distances 5..12, layer 13, viewport
//    index 14, word 15 reserved, generic attribute a component c at 16 + 4a + c (ResourceReservations.FillGeometryInputMap).
//    Words the shader does not write hold (0, 0, 0, 1) / point size 1, as the hardware reads them.
//  - The geometry kernel (main0 of <variant>_b.metal) reads those records (vertex_output), writes output vertices of
//    geometry_stride words (geometry_vb_output) and a strip index list with restarts (geometry_ib_output).
//  - The reader (<variant>_r.metal) is the raster vertex shader that reads an output vertex back (vb_input).
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static class ShaderGeometry
{
    public const int Words = 144;        // capture record words (the Maxwell attribute map above)
    public const int CaptureSlot = 13;   // [[buffer(n)]] of main0_gs's dm_geometry_capture
    public const int MaskWord = 5;       // the viewport mask's word in a passthrough kernel's output vertex
    // The lowering's own buffers, by MSL name: the reflection's "geometry" kind, reg = this index (as nxnative's HELPERS)
    public static readonly Dictionary<string, int> Helpers = new()
    {
        ["vertex_output"] = 0, ["geometry_vb_output"] = 1, ["geometry_ib_output"] = 2, ["vb_info"] = 3, ["trb_data"] = 4, ["vb_input"] = 1,
    };

    internal sealed class GeometryError : Exception { public GeometryError(string message) : base(message) { } }

    /// Output bounds in the translator's fixed-input compute ABI, from the SPH (words 2..4 of the code, the 0x50-byte SPH v3
    /// layout that NVN2's 0x80-byte SPH v4 starts with). A passthrough shader (SPH word 0 bit 24) takes the input primitive's
    /// vertex count and strip, as ShaderDefinitions does (patch 0007).
    public static JsonObject OutputLayout(byte[] code, int inputTopology)
    {
        if (code.Length < 20 || inputTopology < 0 || inputTopology > 4) throw new GeometryError("invalid geometry header or input topology");
        uint w2 = BitConverter.ToUInt32(code, 8), w3 = BitConverter.ToUInt32(code, 12), w4 = BitConverter.ToUInt32(code, 16);
        int vertices = (int)(w4 & 4095), invocations = (int)(w2 >> 24), topology = (int)(w3 >> 24 & 15);
        if (Passthrough(code))
        {
            vertices = new[] { 1, 2, 2, 3, 3 }[inputTopology];
            topology = new[] { 1, 6, 6, 7, 7 }[inputTopology];
        }
        if ((topology != 1 && topology != 6 && topology != 7) || vertices == 0 || invocations == 0) throw new GeometryError("invalid geometry output declaration");
        return new JsonObject { ["vertices"] = vertices, ["invocations"] = invocations, ["output"] = topology, ["index_stride"] = vertices * 2 };
    }

    /// SPH word 0 bit 24 (GpPassthrough): NV passthrough geometry. Per-vertex attributes reach the rasterizer unchanged; the
    /// shader only decides per primitive (viewport mask) and may write storage.
    public static bool Passthrough(byte[] code) => code.Length >= 4 && (BitConverter.ToUInt32(code, 0) >> 24 & 1) != 0;

    /// The compute kernel's LocalSize execution mode (OpExecutionMode, mode 17), or null.
    public static JsonArray LocalSize(byte[] spv)
    {
        for (int i = 20; i + 4 <= spv.Length;)
        {
            uint w = BitConverter.ToUInt32(spv, i); int n = (int)(w >> 16);
            if (n == 0) break;
            if ((w & 0xffff) == 16 && n == 6 && BitConverter.ToUInt32(spv, i + 8) == 17)
                return new JsonArray(BitConverter.ToUInt32(spv, i + 12), BitConverter.ToUInt32(spv, i + 16), BitConverter.ToUInt32(spv, i + 20));
            i += 4 * n;
        }
        return null;
    }

    /// Give the lowering's auxiliary descriptor sets (1: its storage buffers, 2: its texture buffers) their own binding
    /// ranges in the ordinary set 0 (bindings + 10000 / + 11000), so SPIRV-Cross writes one argument buffer.
    public static void FlattenSets(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        uint[] w = new uint[data.Length / 4]; Buffer.BlockCopy(data, 0, w, 0, w.Length * 4);
        var sets = new Dictionary<uint, (uint set, int at)>(); var bindings = new Dictionary<uint, int>();
        for (int i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16); uint op = w[i] & 0xffff;
            if (n == 0 || i + n > w.Length) throw new GeometryError("invalid SPIR-V instruction");
            if (op == 71 && n == 4)
            {
                if (w[i + 2] == 34) sets[w[i + 1]] = (w[i + 3], i + 3);
                if (w[i + 2] == 33) bindings[w[i + 1]] = i + 3;
            }
            i += n;
        }
        foreach (var (id, (set, at)) in sets)
        {
            if (set == 0) continue;
            if ((set != 1 && set != 2) || !bindings.ContainsKey(id)) throw new GeometryError("unknown geometry auxiliary descriptor set");
            w[bindings[id]] += set == 1 ? 10000u : 11000u;
            w[at] = 0;
        }
        byte[] outp = new byte[w.Length * 4]; Buffer.BlockCopy(w, 0, outp, 0, outp.Length);
        File.WriteAllBytes(path, outp);
    }

    static int CloseParen(string s, int at)   // index just past the ')' that closes a '(' ending at `at`
    {
        int depth = 1, end = at;
        while (depth != 0)
        {
            if (end >= s.Length) throw new GeometryError("unbalanced parentheses");
            if (s[end] == '(') depth++;
            if (s[end] == ')') depth--;
            end++;
        }
        return end;
    }

    /// The topology remap buffer as uint words (no texture view per draw): `device const uint* trb_data`.
    public static string TopologyPointer(string src)
    {
        var decl = new Regex(@"texture_buffer<(?:float|uint|int)> trb_data\b");
        if (decl.Matches(src).Count != 1) throw new GeometryError("missing geometry topology buffer");
        src = decl.Replace(src, "device const uint* trb_data", 1);
        const string prefix = "spvDescriptorSet0.trb_data.read(";
        int start;
        while ((start = src.IndexOf(prefix, StringComparison.Ordinal)) >= 0)
        {
            int at = start + prefix.Length, end = CloseParen(src, at);
            if (string.CompareOrdinal(src, end, ".x", 0, 2) != 0) throw new GeometryError("unexpected topology read");
            src = src.Substring(0, start) + "as_type<float>(spvDescriptorSet0.trb_data[" + src.Substring(at, end - 1 - at) + "])" + src.Substring(end + 2);
        }
        if (src.Contains("trb_data.read", StringComparison.Ordinal)) throw new GeometryError("unconverted geometry topology read");
        return src;
    }

    /// Volatile capture reads: copying all 128 contiguous generic components can make Metal's optimized AIR fail AGX
    /// pipeline compilation; volatile reads keep each scalar load without changing values (nxnative capture_input).
    public static string CaptureInput(string src)
    {
        if (!src.Contains("vertex_output", StringComparison.Ordinal)) return src;
        var decl = new Regex(@"\bdevice (\w+)\* vertex_output\b");
        if (decl.Matches(src).Count != 1) throw new GeometryError("unknown geometry capture-input declaration");
        return decl.Replace(src, "device volatile ${1}* vertex_output", 1);
    }

    /// The reader's viewport mask (builtin 5253, ViewportMaskNV), which SPIRV-Cross copies as an invalid array and Metal
    /// does not have: the host binds viewport 0 only, so its bit decides visibility; a 0 moves the vertex beyond the X clip
    /// plane (the mask must agree across a primitive). Unknown expressions are rejected rather than losing the culling.
    public static string Reader(string src)
    {
        const string builtin = "gl_BuiltIn_5253";
        if (!src.Contains(builtin, StringComparison.Ordinal)) return src;
        // spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(<value>).elements); where the value is a name (DebugMode)
        // or the vb_input read itself (AST optimizer on): taken up to its balanced parenthesis
        var store = new Regex(@"\bspvArrayCopyFromStackToStack\(\s*" + builtin + @"\s*,\s*as_type<int>\(");
        var tail = new Regex(@"\G\.elements\s*\);");
        string lowered = src; int n = 0;
        for (Match m = store.Match(lowered); m.Success; m = store.Match(lowered))
        {
            int open = m.Index + m.Length, close = CloseParen(lowered, open);
            Match t = tail.Match(lowered, close);
            if (!t.Success) throw new GeometryError("geometry reader: unsupported viewport-mask expression");
            lowered = lowered.Substring(0, m.Index) + "dm_gs_viewport_mask = as_type<uint>(" + lowered.Substring(open, close - 1 - open) + ");" + lowered.Substring(t.Index + t.Length);
            n++;
        }
        if (n == 0 || lowered.Contains(builtin, StringComparison.Ordinal) || src.Contains("dm_gs_viewport_mask", StringComparison.Ordinal))
            throw new GeometryError("geometry reader: unsupported viewport-mask expression");
        Match output = Regex.Match(lowered, @"struct main0_out\s*\{(.*?)\};", RegexOptions.Singleline);
        Match position = output.Success ? Regex.Match(output.Groups[1].Value, @"\bfloat4 (\w+) \[\[position(?:, invariant)?\]\];") : Match.Empty;
        Match entry = Regex.Match(lowered, @"\bvertex\s+main0_out\s+main0\s*\([^{}]*\)\s*\{");
        if (!position.Success || !entry.Success) throw new GeometryError("geometry reader: missing vertex position or entry");
        int begin = entry.Index + entry.Length, end = BodyEnd(lowered, begin);
        string body = lowered.Substring(begin, end - 1 - begin);
        if (Regex.Matches(body, "dm_gs_viewport_mask").Count != n || !body.Contains("return out;", StringComparison.Ordinal))
            throw new GeometryError("geometry reader: viewport mask outside the vertex entry");
        body = body.Replace("return out;", "{ if ((dm_gs_viewport_mask & 1u) == 0u) out." + position.Groups[1].Value + " = float4(2.0, 0.0, 0.0, 1.0); return out; }");
        return lowered.Substring(0, begin) + "\n    uint dm_gs_viewport_mask = 1u;" + body + lowered.Substring(end - 1);
    }

    static int BodyEnd(string s, int begin)   // index just past the '}' closing a function body that starts after `begin`'s '{'
    {
        int depth = 1, end = begin;
        while (depth != 0 && end < s.Length) { depth += s[end] == '{' ? 1 : s[end] == '}' ? -1 : 0; end++; }
        if (depth != 0) throw new GeometryError("unterminated vertex entry");
        return end;
    }

    /// nvnDeviceSetDepthMode(NEAR_IS_MINUS_W) in the reader: z' = z/2 + w/2 before every return, the formula and operation
    /// order the translator writes into a vertex shader for "depth_minus_w" (EmitterContext.PrepareForVertexReturn), so a
    /// passthrough draw through the kernel gives the same depth bits as the vertex shader drawn alone.
    public static string ReaderDepthMinusW(string src)
    {
        Match output = Regex.Match(src, @"struct main0_out\s*\{(.*?)\};", RegexOptions.Singleline);
        Match position = output.Success ? Regex.Match(output.Groups[1].Value, @"\bfloat4 (\w+) \[\[position(?:, invariant)?\]\];") : Match.Empty;
        Match entry = Regex.Match(src, @"\bvertex\s+main0_out\s+main0\s*\(");
        if (!position.Success || !entry.Success) throw new GeometryError("geometry reader: missing vertex position or entry");
        string p = "out." + position.Groups[1].Value, body = src.Substring(entry.Index);   // the entry is the last function
        if (!body.Contains("return out;", StringComparison.Ordinal)) throw new GeometryError("geometry reader: missing return");
        return src.Substring(0, entry.Index) + body.Replace("return out;", $"{p}.z = fma({p}.z, 0.5, {p}.w * 0.5);\n    return out;");
    }

    /// The capture entry point main0_gs for a vertex shader's MSL (appended to it; it reuses its structs and helpers):
    /// the same body, writing every output word of the 144-word record through the host's slot table instead of returning.
    public static string Capture(string src)
    {
        Match m = Regex.Match(src, @"\bvertex main0_out main0\((.*?)\)\s*\{", RegexOptions.Singleline);
        if (!m.Success) throw new GeometryError("geometry capture: missing main0 vertex entry");
        string parameters = m.Groups[1].Value, body = src.Substring(m.Index + m.Length);
        if (!body.TrimEnd().EndsWith('}')) throw new GeometryError("geometry capture: unexpected function ending");
        var pro = new StringBuilder();
        foreach (var (semantic, name) in new[] { ("vertex_id", "dm_vid"), ("instance_id", "dm_iid") })
        {
            Match found = Regex.Match(parameters, @"uint (\w+) \[\[" + semantic + @"\]\]");
            if (found.Success) pro.Append($"    uint {name} = {found.Groups[1].Value};\n");
            else parameters += (parameters.Length > 0 ? ", " : "") + $"uint {name} [[{semantic}]]";
        }
        parameters += $", constant dm_geometry_capture& dm_gs [[buffer({CaptureSlot})]]";
        pro.Append("    uint4 dm_ci = dm_gs.info; device uint* dm_capture = dm_gs.dst;\n");
        pro.Append("    if (dm_vid - dm_ci.x >= dm_ci.y || dm_iid - dm_ci.z >= dm_ci.w) return;\n");
        pro.Append("    device uint* dm_p = dm_capture + ((ulong(dm_iid - dm_ci.z) * dm_ci.y + dm_vid - dm_ci.x) * dm_gs.stride);\n");
        // Missing attributes hold what the normal vertex builder gives them: (0, 0, 0, 1), point size 1. Each word is written once, through its slot.
        var words = new SortedDictionary<int, string>();
        for (int k = 0; k < Words; k++) words[k] = k == 3 || k == 4 || (k >= 19 && k % 4 == 3) ? "0x3f800000u" : "0u";
        foreach (var (k, e) in OutputWords(src)) words[k] = $"as_type<uint>({e})";
        string epilogue = "{ " + string.Join(" ", words.Select(kv => $"dm_cw(dm_gs, dm_p, {kv.Key}, {kv.Value});")) + " return; }";
        if (!body.Contains("return out;", StringComparison.Ordinal)) throw new GeometryError("geometry capture: missing return");
        return $"\nstruct dm_geometry_capture {{ device uint* dst; uint4 info; uint stride; short slot[{Words}]; }};\n" +
            "static inline void dm_cw(constant dm_geometry_capture& g, device uint* p, uint k, uint v) { short s = g.slot[k]; if (s >= 0) p[s] = v; }\n" +
            "vertex void main0_gs(" + parameters + ")\n{\n" + pro + body.Replace("return out;", epilogue);
    }

    /// The capture words a vertex shader writes: word -> expression over `out` (the 144-word map above).
    public static SortedDictionary<int, string> OutputWords(string src)
    {
        Match st = Regex.Match(src, @"struct main0_out\s*\{(.*?)\};", RegexOptions.Singleline);
        if (!st.Success) throw new GeometryError("geometry capture: missing output struct");
        var stores = new SortedDictionary<int, string>();
        foreach (string raw in st.Groups[1].Value.Split('\n'))
        {
            string line = raw.Trim();
            if (line.Length == 0) continue;
            Match v = Regex.Match(line, @"^(?:float4|uint4|int4) (\w+) \[\[user\(locn(\d+)\)\]\];$");
            if (v.Success)
            {
                int loc = int.Parse(v.Groups[2].Value);
                if (loc >= 32) throw new GeometryError("geometry capture: attribute outside Maxwell map");
                for (int c = 0; c < 4; c++) stores[16 + loc * 4 + c] = $"out.{v.Groups[1].Value}.{"xyzw"[c]}";
                continue;
            }
            v = Regex.Match(line, @"^float4 (\w+) \[\[position(?:, invariant)?\]\];$");
            if (v.Success) { for (int c = 0; c < 4; c++) stores[c] = $"out.{v.Groups[1].Value}.{"xyzw"[c]}"; continue; }
            v = Regex.Match(line, @"^(?:float|uint|int) (\w+) \[\[(point_size|render_target_array_index|viewport_array_index)\]\];$");
            if (v.Success)
            {
                stores[v.Groups[2].Value switch { "point_size" => 4, "render_target_array_index" => 13, _ => 14 }] = $"out.{v.Groups[1].Value}";
                continue;
            }
            // SPIRV-Cross writes the array size after the attribute (float gl_ClipDistance [[clip_distance]] [8])
            v = Regex.Match(line, @"^float (\w+)(?:\[(\d+)\])? \[\[clip_distance\]\](?: \[(\d+)\])?;$");
            if (v.Success && (v.Groups[2].Success || v.Groups[3].Success))
            {
                int count = int.Parse(v.Groups[2].Success ? v.Groups[2].Value : v.Groups[3].Value);
                if (count <= 8) { for (int c = 0; c < count; c++) stores[5 + c] = $"out.{v.Groups[1].Value}[{c}]"; continue; }
            }
            v = Regex.Match(line, @"^float (\w+) \[\[user\(clip(\d)\)\]\];$");   // the same distances again as varyings for the pixel stage
            if (v.Success) { stores[5 + int.Parse(v.Groups[2].Value)] = $"out.{v.Groups[1].Value}"; continue; }
            throw new GeometryError("geometry capture: unknown output " + line);
        }
        return stores;
    }
}

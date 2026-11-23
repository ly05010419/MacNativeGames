// The service's tessellation requests (Service.cs Handle). Metal 4's render encoders have no tessellator (no drawPatches),
// so a tessellated draw runs as (the host side is nvn_tess_rt.inc of the ports that draw them, first The Witcher 3):
//   1. the vertex shader's capture entry (GeometryService.cs AppendCapture, "geometry_capture"): one 144-word record per
//      vertex of the draw (position 0..3, point size 4, clip distances 5..12, attribute a component c at 16 + 4a + c);
//   2. the control stage ("tcs") as a kernel over all patches: SPIRV-Cross's multi-patch tessellation control kernel, whose
//      input array is rebuilt here from the capture records (dm_rec, through the patch vertex map dm_map) instead of the
//      stage-input buffer MoltenVK fills; it writes its control points (spvOut), patch data (spvPatchOut) and tessellation
//      levels (spvTessLevel, Metal's MTL{Quad,Triangle}TessellationFactorsHalf);
//   3. the host's tessellator kernel: integer partitioning of each patch from those levels into triangles of domain points;
//   4. the evaluation stage ("tes") as an ordinary vertex function: SPIRV-Cross's post-tessellation vertex function with raw
//      buffer input, its patch attribute and [[position_in_patch]] / [[patch_id]] replaced by a read of the tessellator's
//      domain point for [[vertex_id]] (dm_dom: patch number, u = k/n and v = k/n as four bytes), drawn with the program's
//      fragment shader.
// Request: {"variant", "stage": "tcs", "code", "c1", "tex", "img"}
//   -> <variant>_b.metal (kernel main0) / _b.json ("tess_control": out_vertices, out_stride, patch_stride, factors).
// Request: {"variant", "stage": "tes", "code", "c1", "tex", "img", "depth_minus_w", "tess_patch", "tess_spacing", "tess_cw",
//   "tess_in_vertices" (the control stage's out_vertices)} -> <variant>_b.metal (vertex main0) / _b.json ("tess_eval").
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static partial class ShaderService
{
    // Buffer slots of the tessellation entries (SPIRV-Cross's defaults for the ones it declares)
    const int TessDomainSlot = 21, TessRecordSlot = 22, TessMapSlot = 23, TessPatchInSlot = 20, TessInSlot = 22;
    const int TessMaxPatchVertices = 32;

    static void HandleTess(JsonObject rq)
    {
        string variant = (string)rq["variant"], stage = (string)rq["stage"];
        bool control = stage == "tcs";
        JsonObject job = MakeJob(rq, variant, stage);
        int inVertices = 0;
        if (!control)
        {
            foreach (string k in new[] { "tess_patch", "tess_spacing" })
                if (Get(rq, k) is not JsonValue v || !v.TryGetValue(out int n) || n < 0 || n > 2) throw new RuntimeError($"tessellation evaluation without a {k} of 0..2");
            if (Get(rq, "tess_in_vertices") is not JsonValue iv || !iv.TryGetValue(out inVertices) || inVertices < 1 || inVertices > TessMaxPatchVertices)
                throw new RuntimeError("tessellation evaluation without tess_in_vertices (the control stage's output vertices, 1..32)");
            job["tess_patch"] = rq["tess_patch"].DeepClone(); job["tess_spacing"] = rq["tess_spacing"].DeepClone();
            job["tess_cw"] = Truthy(Get(rq, "tess_cw"));
        }
        string spv = Path.Combine(outDir, variant + ".spv"), msl = Path.Combine(outDir, variant + "_b.metal");
        uint flags = control ? XcMultiPatch : XcRawTeseInput;
        Translate(job);
        SpirvCross(spv, msl, $"spirv-cross ({stage}): ", flags);
        string src = File.ReadAllText(msl);
        bool softfloat64 = Fp64.IsMatch(src);
        if (softfloat64)
        {
            job["softfloat64"] = true;   // Metal has no FP64: Ryujinx's DoubleToFloat lowering, as for the other stages
            Translate(job);
            SpirvCross(spv, msl, $"spirv-cross ({stage}) FP64 fallback: ", flags);
            src = File.ReadAllText(msl);
            if (Fp64.IsMatch(src)) throw new RuntimeError("Ryujinx FP64 fallback still emitted Metal-unsupported double");
        }
        JsonObject res = ReadRes(variant);
        JsonObject tex = GetObj(rq, "tex"), img = GetObj(rq, "img");
        HashSet<long> depthSlots = DepthSlots(tex);
        var compareSlots = CompareSlots(src);
        src = PostProcess(src, depthSlots);
        JsonObject layout;
        if (control)
        {
            // the input members' locations: the single-patch form declares them as stage-input attributes
            string located = Path.Combine(outDir, variant + ".loc.metal");
            try { SpirvCross(spv, located, "spirv-cross (tcs locations): "); src = TessControl(src, File.ReadAllText(located), File.ReadAllBytes(spv), out layout); }
            finally { File.Delete(located); }
        }
        else src = TessEval(src, inVertices, File.ReadAllBytes(spv), out layout);
        File.WriteAllText(msl, src);
        Match entry = Regex.Match(src, @"(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(");
        if (!entry.Success || entry.Groups[1].Value != (control ? "kernel" : "vertex")) throw new RuntimeError($"{stage}: unexpected entry point after the rewrite");
        JsonObject dr = DrvReads(src, stage);
        var refl = new JsonObject
        {
            ["key"] = variant + "_b", ["route"] = "B", ["chain"] = chain, ["stage"] = stage,
            ["entry"] = entry.Groups[2].Value, ["msl_entry_kind"] = entry.Groups[1].Value,
            ["threadgroup_size"] = null, ["c1"] = GetStr(rq, "c1", ""), ["notes"] = res["notes"].DeepClone(),
            ["drv_reads"] = dr, ["drv_unfillable"] = Unfillable(dr, stage), ["bindings"] = Reflect(stage, src, res),
            ["vertex_inputs"] = new JsonArray(), ["raster"] = new JsonObject(), ["deps"] = TextureDeps(src, res, tex, img, depthSlots),
            ["softfloat64"] = softfloat64, ["compare_slots"] = new JsonArray(compareSlots.Select(s => (JsonNode)s).ToArray()),
            [control ? "tess_control" : "tess_eval"] = layout,
        };
        WriteReflection(variant + "_b", refl);
    }

    // ---- MSL struct sizes (the host allocates the control stage's outputs by them; the evaluation stage reads them back) ----
    static (int size, int align) MslTypeSize(string ty)
    {
        ty = ty.Trim();
        Match arr = Regex.Match(ty, @"^(.*)\[(\d+)\]$");
        if (arr.Success) { var (s, a) = MslTypeSize(arr.Groups[1].Value); return (s * int.Parse(arr.Groups[2].Value, CultureInfo.InvariantCulture), a); }
        Match v = Regex.Match(ty, @"^(packed_)?(float|int|uint|half|short|ushort)([234])?$");
        if (!v.Success) throw new RuntimeError($"tessellation: struct member type {ty} not sized");
        int scalar = v.Groups[2].Value is "half" or "short" or "ushort" ? 2 : 4;
        int n = v.Groups[3].Success ? int.Parse(v.Groups[3].Value, CultureInfo.InvariantCulture) : 1;
        if (v.Groups[1].Success) return (scalar * n, scalar);
        int lanes = n == 3 ? 4 : n;
        return (scalar * lanes, scalar * lanes);
    }

    /// The members of struct `name` (type, member name, the text in [[...]] or ""), in order.
    static List<(string ty, string name, string attr)> MslStruct(string src, string name)
    {
        Match st = Regex.Match(src, @"struct " + name + @"\s*\{(.*?)\};", RegexOptions.Singleline);
        if (!st.Success) return null;
        var members = new List<(string, string, string)>();
        foreach (string raw in st.Groups[1].Value.Split('\n'))
        {
            string line = raw.Trim();
            if (line.Length == 0) continue;
            Match m = Regex.Match(line, @"^(.+?)\s+(\w+)(\[\d+\])?(?:\s*\[\[(.*?)\]\])?;$");
            if (!m.Success) throw new RuntimeError($"tessellation: struct {name} member not understood: {line}");
            members.Add((m.Groups[1].Value + m.Groups[3].Value, m.Groups[2].Value, m.Groups[4].Value));
        }
        return members;
    }

    static int MslStructSize(List<(string ty, string name, string attr)> members)
    {
        if (members == null) return 0;
        int off = 0, align = 1;
        foreach (var (ty, _, _) in members)
        {
            var (s, a) = MslTypeSize(ty);
            off = (off + a - 1) / a * a + s; align = Math.Max(align, a);
        }
        return (off + align - 1) / align * align;
    }

    /// The control stage: SPIRV-Cross's multi-patch kernel, its input array read from the capture records. `located` is the
    /// single-patch translation of the same SPIR-V, whose main0_in members carry their locations ([[attribute(n)]]).
    // ---- The interface between the two stages: one layout for both, whatever each declares ----
    // The control stage's outputs and the evaluation stage's inputs are translated apart, and each declares only the members it
    // writes or reads (The Witcher 3: a control kernel writing three attributes and gl_Position, an evaluation stage reading the
    // three). Both structs (and the patch structs) are rewritten to the same layout: location L in slot L (16 bytes; 32 slots),
    // then gl_Position. SPIRV-Cross names an interface member m_<SPIR-V id>; the id's Location decoration gives L.
    const int TessSlots = 32;

    static Dictionary<int, int> SpirvLocations(byte[] spv)
    {
        var locs = new Dictionary<int, int>();
        int words = spv.Length / 4;
        for (int i = 5; i < words;)
        {
            uint w = BitConverter.ToUInt32(spv, i * 4); int op = (int)(w & 0xffff), n = (int)(w >> 16);
            if (n == 0) break;
            if (op == 71 && n >= 4 && BitConverter.ToUInt32(spv, (i + 2) * 4) == 30)   // OpDecorate <id> Location <n>
                locs[(int)BitConverter.ToUInt32(spv, (i + 1) * 4)] = (int)BitConverter.ToUInt32(spv, (i + 3) * 4);
            i += n;
        }
        return locs;
    }

    static string CanonicalInterface(string src, string name, Dictionary<int, int> locs, bool position)
    {
        var members = MslStruct(src, name);
        if (members == null) return src;
        var slot = new string[TessSlots]; string pos = null;
        foreach (var (ty, member, _) in members)
        {
            if (member == "gl_Position") { pos = $"    {ty} gl_Position;"; continue; }
            Match id = Regex.Match(member, @"^m_(\d+)$");
            if (!id.Success || !locs.TryGetValue(int.Parse(id.Groups[1].Value, CultureInfo.InvariantCulture), out int loc) || loc < 0 || loc >= TessSlots)
                throw new RuntimeError($"tessellation: {name}.{member} has no location");
            if (MslTypeSize(ty).size != 16) throw new RuntimeError($"tessellation: {name}.{member} of type {ty} (16-byte members only)");
            slot[loc] = $"    {ty} {member};";
        }
        var text = new StringBuilder($"struct {name}\n{{\n");
        for (int l = 0; l < TessSlots; l++) text.Append(slot[l] ?? $"    float4 dm_slot{l};").Append('\n');
        if (position) text.Append(pos ?? "    float4 dm_position;").Append('\n');
        text.Append("};");
        Match st = Regex.Match(src, @"struct " + name + @"\s*\{(.*?)\};", RegexOptions.Singleline);
        return src.Substring(0, st.Index) + text + src.Substring(st.Index + st.Length);
    }

    static string TessControl(string src, string located, byte[] spv, out JsonObject layout)
    {
        var locs = SpirvLocations(spv);
        src = CanonicalInterface(CanonicalInterface(src, "main0_out", locs, true), "main0_patchOut", locs, false);
        Match head = Regex.Match(src, @"\bkernel void main0\((.*?)\)\s*\{", RegexOptions.Singleline);
        if (!head.Success) throw new RuntimeError("tcs: missing main0 kernel");
        Match gin = Regex.Match(src, @"device main0_in\* gl_in = &spvIn\[(.*?)\];\n");
        Match gout = Regex.Match(src, @"device main0_out\* gl_out = &spvOut\[gl_GlobalInvocationID\.x - gl_GlobalInvocationID\.x % (\d+)\];");
        if (!gout.Success) throw new RuntimeError("tcs: no output control point array");
        int outVertices = int.Parse(gout.Groups[1].Value, CultureInfo.InvariantCulture);
        Match factors = Regex.Match(src, @"device MTL(Quad|Triangle)TessellationFactorsHalf\* spvTessLevel \[\[buffer\((\d+)\)\]\]");
        if (!factors.Success) throw new RuntimeError("tcs: no tessellation level buffer");
        string parameters = head.Groups[1].Value;
        var bufs = new JsonObject();
        foreach (Match b in Regex.Matches(parameters, @"(spvOut|spvPatchOut|spvTessLevel|spvIndirectParams)\s*\[\[buffer\((\d+)\)\]\]"))
            bufs[b.Groups[1].Value] = int.Parse(b.Groups[2].Value, CultureInfo.InvariantCulture);
        foreach (string need in new[] { "spvOut", "spvTessLevel", "spvIndirectParams" })
            if (!bufs.ContainsKey(need)) throw new RuntimeError($"tcs: kernel without {need}");
        string body = src.Substring(head.Index + head.Length);
        if (gin.Success)
        {
            // where each input member lives in a capture record (the single-patch form names the locations)
            var inLocs = new Dictionary<string, int>();
            foreach (var (_, name, attr) in MslStruct(located, "main0_in") ?? new())
            {
                Match a = Regex.Match(attr, @"attribute\((\d+)\)");
                if (a.Success) inLocs[name] = int.Parse(a.Groups[1].Value, CultureInfo.InvariantCulture);
            }
            var fill = new StringBuilder();
            foreach (var (ty, name, _) in MslStruct(src, "main0_in") ?? throw new RuntimeError("tcs: no main0_in"))
            {
                int word;
                if (name == "gl_Position") word = 0;
                else if (name == "gl_PointSize") word = 4;
                else if (inLocs.TryGetValue(name, out int loc) && loc < 32) word = 16 + 4 * loc;
                else throw new RuntimeError($"tcs: input {name} has no location in the capture record");
                Match vt = Regex.Match(ty, @"^(float|int|uint)([234])?$");
                if (!vt.Success) throw new RuntimeError($"tcs: input {name} of type {ty}");
                int n = vt.Groups[2].Success ? int.Parse(vt.Groups[2].Value, CultureInfo.InvariantCulture) : 1;
                string words = string.Join(", ", Enumerable.Range(0, n).Select(c => $"dm_r[{word + c}]"));
                string value = n == 1 ? $"as_type<{ty}>({words})" : $"as_type<{ty}>(uint{n}({words}))";
                fill.Append($"        gl_in[dm_k].{name} = {value};\n");
            }
            string rebuilt =
                $"main0_in gl_in[{TessMaxPatchVertices}];\n" +
                $"    for (uint dm_k = 0; dm_k < spvIndirectParams[0] && dm_k < {TessMaxPatchVertices}; dm_k++)\n    {{\n" +
                $"        const device uint* dm_r = dm_rec + ulong(dm_map[{gin.Groups[1].Value} + dm_k]) * {ShaderGeometry.Words};\n" + fill + "    }\n";
            body = body.Replace(gin.Value, rebuilt);
            body = Regex.Replace(body, @"\(device (\w+)\*\)&gl_in\b", "(thread $1*)&gl_in");   // dynamic component reads of an input
            parameters = Regex.Replace(parameters, @",\s*device main0_in\* spvIn \[\[buffer\(\d+\)\]\]", "");
            parameters += $", const device uint* dm_rec [[buffer({TessRecordSlot})]], const device uint* dm_map [[buffer({TessMapSlot})]]";
        }
        if (Regex.IsMatch(body, @"\bspvIn\b")) throw new RuntimeError("tcs: input read outside the rebuilt array");
        layout = new JsonObject
        {
            ["out_vertices"] = outVertices, ["out_stride"] = MslStructSize(MslStruct(src, "main0_out")),
            ["patch_stride"] = MslStructSize(MslStruct(src, "main0_patchOut")), ["factors"] = factors.Groups[1].Value == "Quad" ? "quad" : "triangle",
            ["buffers"] = bufs, ["record_buffer"] = TessRecordSlot, ["map_buffer"] = TessMapSlot, ["record_words"] = ShaderGeometry.Words,
            ["max_patch_vertices"] = TessMaxPatchVertices,
        };
        return src.Substring(0, head.Index) + "kernel void main0(" + parameters + ")\n{" + body;
    }

    /// The evaluation stage as a vertex function over the tessellator's domain points.
    static string TessEval(string src, int inVertices, byte[] spv, out JsonObject layout)
    {
        var locs = SpirvLocations(spv);
        src = CanonicalInterface(CanonicalInterface(src, "main0_in", locs, true), "main0_patchIn", locs, false);
        Match head = Regex.Match(src, @"\[\[ patch\((\w+), \d+\) \]\] vertex main0_out main0\((.*?)\)\s*\{", RegexOptions.Singleline);
        if (!head.Success) throw new RuntimeError("tes: missing the post-tessellation vertex entry");
        string domain = head.Groups[1].Value, parameters = head.Groups[2].Value;
        // a triangle domain takes float3 gl_TessCoord; a quad domain float2 gl_TessCoordIn, widened in the body
        Match coord = Regex.Match(parameters, @"(float[23]) (gl_TessCoord(?:In)?) \[\[position_in_patch\]\]");
        Match pid = Regex.Match(parameters, @"uint gl_PrimitiveID \[\[patch_id\]\]");
        if (!coord.Success) throw new RuntimeError("tes: no gl_TessCoord");
        parameters = Regex.Replace(parameters, @",?\s*float[23] gl_TessCoord(?:In)? \[\[position_in_patch\]\]", "");
        parameters = Regex.Replace(parameters, @",?\s*uint gl_PrimitiveID \[\[patch_id\]\]", "");
        parameters = parameters.Trim().TrimStart(',').Trim();
        var bufs = new JsonObject();
        foreach (Match b in Regex.Matches(parameters, @"(spvPatchIn|spvIn)\s*\[\[buffer\((\d+)\)\]\]"))
            bufs[b.Groups[1].Value] = int.Parse(b.Groups[2].Value, CultureInfo.InvariantCulture);
        parameters += (parameters.Length > 0 ? ", " : "") + $"uint dm_vid [[vertex_id]], const device uint2* dm_dom [[buffer({TessDomainSlot})]]";
        // the domain point: patch number, then u = a / b and v = c / d as the bytes a, b, c, d (n <= 64 segments an edge)
        string uv = "float2(float(dm_d.y & 0xffu) / float((dm_d.y >> 8) & 0xffu), float((dm_d.y >> 16) & 0xffu) / float(dm_d.y >> 24))";
        string pro = "\n    uint2 dm_d = dm_dom[dm_vid];\n    uint gl_PrimitiveID = dm_d.x;\n" +
            (coord.Groups[1].Value == "float2" ? $"    float2 {coord.Groups[2].Value} = {uv};\n" : $"    float2 dm_uv = {uv};\n    float3 {coord.Groups[2].Value} = float3(dm_uv, 1.0 - dm_uv.x - dm_uv.y);\n");
        string body = src.Substring(head.Index + head.Length);
        body = body.Replace("&spvIn[gl_PrimitiveID * 0]", $"&spvIn[gl_PrimitiveID * {inVertices}]");
        body = body.Replace("uint gl_PatchVerticesIn = 0;", $"uint gl_PatchVerticesIn = {inVertices};");
        if (Regex.IsMatch(body, @"\* 0\]")) throw new RuntimeError("tes: an input indexed without its patch size");
        layout = new JsonObject
        {
            ["domain"] = domain, ["in_vertices"] = inVertices, ["in_stride"] = MslStructSize(MslStruct(src, "main0_in")),
            ["patch_stride"] = MslStructSize(MslStruct(src, "main0_patchIn")), ["domain_buffer"] = TessDomainSlot, ["buffers"] = bufs,
            ["uses_primitive_id"] = pid.Success,
        };
        return src.Substring(0, head.Index) + "vertex main0_out main0(" + parameters + ")\n{" + pro + body;
    }
}

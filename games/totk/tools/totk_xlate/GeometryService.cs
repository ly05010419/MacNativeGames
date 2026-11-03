// The service's geometry requests (Service.cs Handle): a geometry shader as a Metal compute kernel and its raster reader,
// and the capture entry point of a vertex shader that feeds one. The MSL steps are in Geometry.cs; the order follows
// seedds/nxnative dm/gfx/tools/dmsh_build.py geometry_finish (MIT): flatten the auxiliary sets, SPIRV-Cross, the usual
// MSL steps, the topology buffer as a pointer, volatile capture reads.
//
// Request: {"variant", "stage": "gs", "code", "c1", "arch", "tex", "img", "depth_minus_w", "geometry_topology": 0..4}
//   -> <variant>_b.metal / _b.json (kernel main0) and <variant>_r.metal / _r.json (reader vertex main0).
// Request: {..., "stage": "vs", "geometry_capture": true} -> the vertex shader's MSL with main0_gs appended; _b.json gains
//   "entry_geometry_capture" and "geometry_capture".
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static partial class ShaderService
{
    static readonly int[] InputVertexCount = { 1, 2, 4, 3, 6 };   // per InputTopology, adjacency vertices included (trb_data words per primitive)

    static void HandleGeometry(JsonObject rq)
    {
        string variant = (string)rq["variant"];
        if (Get(rq, "geometry_topology") is not JsonValue tv || !tv.TryGetValue(out int topology) || topology < 0 || topology > 4)
            throw new RuntimeError("geometry shader without a geometry_topology of 0..4 (the draw's InputTopology)");
        byte[] code = File.ReadAllBytes((string)rq["code"]);
        JsonObject layout = ShaderGeometry.OutputLayout(code, topology);   // validated before the translator runs
        bool depthMinusW = Truthy(Get(rq, "depth_minus_w"));
        JsonObject job = MakeJob(rq, variant, "gs");
        job["geometry_topology"] = topology;
        // The kernel's output vertices keep the guest's clip space (the vertex shader's capture does too); the reader, the
        // last stage before the rasterizer, converts a -w..w depth mode to Metal's 0..w (ShaderGeometry.ReaderDepthMinusW).
        job["depth_minus_w"] = false;
        string spv = Path.Combine(outDir, variant + ".spv"), readerSpv = Path.Combine(outDir, variant + ".reader.spv");
        string msl = Path.Combine(outDir, variant + "_b.metal"), readerMsl = Path.Combine(outDir, variant + "_r.metal");
        Translate(job);
        string src = KernelMsl(spv, msl, "spirv-cross (geometry): ");
        bool softfloat64 = Fp64.IsMatch(src);
        if (softfloat64)
        {
            job["softfloat64"] = true;   // Metal has no FP64: Ryujinx's DoubleToFloat lowering, as for the other stages
            Translate(job);
            src = KernelMsl(spv, msl, "spirv-cross (geometry) FP64 fallback: ");
            if (Fp64.IsMatch(src)) throw new RuntimeError("Ryujinx FP64 fallback still emitted Metal-unsupported double");
        }
        JsonObject res = ReadRes(variant);
        // the SPH's bounds must be the translator's (ShaderDefinitions with patch 0007): the host sizes its buffers by them
        if (Long(res["geometry_invocations"]) != (int)layout["invocations"] || Long(res["geometry_max_output_vertices"]) != (int)layout["vertices"])
            throw new RuntimeError($"geometry bounds: SPH {layout.ToJsonString()}, translator {res["geometry_invocations"]} invocations / {res["geometry_max_output_vertices"]} vertices");
        JsonObject tex = GetObj(rq, "tex"), img = GetObj(rq, "img");
        HashSet<long> depthSlots = DepthSlots(tex);
        var compareSlots = CompareSlots(src);
        string plain = ShaderGeometry.TopologyPointer(PostProcess(src, depthSlots));
        src = ShaderGeometry.CaptureInput(plain);
        File.WriteAllText(msl, src);
        Match entry = Regex.Match(src, @"(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(");
        if (!entry.Success || entry.Groups[1].Value != "kernel") throw new RuntimeError("geometry: the translation is not a compute kernel");
        JsonArray localSize = ShaderGeometry.LocalSize(File.ReadAllBytes(spv)) ?? throw new RuntimeError("geometry: no LocalSize in the kernel");
        JsonObject dr = DrvReads(src, "gs");
        layout["stride"] = res["geometry_stride"].DeepClone();
        layout["input_topology"] = topology;
        layout["input_vertices"] = InputVertexCount[topology];
        layout["input_stride"] = ShaderGeometry.Words;
        layout["passthrough"] = ShaderGeometry.Passthrough(code);
        layout["reader"] = variant + "_r";
        layout["depth_minus_w"] = depthMinusW;
        var refl = new JsonObject
        {
            ["key"] = variant + "_b", ["route"] = "B", ["chain"] = chain, ["stage"] = "gs",
            ["entry"] = entry.Groups[2].Value, ["msl_entry_kind"] = "kernel",
            ["threadgroup_size"] = localSize, ["c1"] = GetStr(rq, "c1", ""), ["notes"] = res["notes"].DeepClone(),
            ["drv_reads"] = dr, ["drv_unfillable"] = Unfillable(dr, "gs"), ["bindings"] = Reflect("gs", src, res),
            ["vertex_inputs"] = new JsonArray(), ["raster"] = new JsonObject(), ["deps"] = TextureDeps(src, res, tex, img, depthSlots),
            ["softfloat64"] = softfloat64, ["compare_slots"] = new JsonArray(compareSlots.Select(s => (JsonNode)s).ToArray()),
            ["geometry"] = layout,
        };
        WriteReader(readerSpv, readerMsl, variant, (int)layout["output"], depthMinusW);
        // Passthrough input without adjacency, one invocation: the lean kernel the host draws with the game's own vertex shader
        // (GeometryLean.cs). Named in the full kernel's layout ("lean"); a shape it does not handle stays a note.
        if ((bool)layout["passthrough"] && (topology == 0 || topology == 1 || topology == 3) && (int)layout["invocations"] == 1)
        {
            try
            {
                (string lean, int[] reads) = ShaderGeometryLean.Lean(plain, (int)res["geometry_stride"]);
                File.WriteAllText(Path.Combine(outDir, variant + "_l.metal"), lean);
                var leanLayout = (JsonObject)layout.DeepClone();
                leanLayout["stride"] = 1; leanLayout["lean"] = true;
                leanLayout["capture_reads"] = new JsonArray(reads.Select(k => (JsonNode)k).ToArray());
                var leanRefl = (JsonObject)refl.DeepClone();
                leanRefl["key"] = variant + "_l"; leanRefl["geometry"] = leanLayout;
                leanRefl["drv_reads"] = DrvReads(lean, "gs"); leanRefl["bindings"] = Reflect("gs", lean, res);
                WriteReflection(variant + "_l", leanRefl);
                layout["lean"] = variant + "_l";
            }
            catch (ShaderGeometry.GeometryError e) { ((JsonArray)refl["notes"]).Add((JsonNode)JsonValue.Create(e.Message)); }
        }
        WriteReflection(variant + "_b", refl);   // last: the host takes a variant once its _b.json is there
    }

    static string KernelMsl(string spv, string msl, string what)
    {
        ShaderGeometry.FlattenSets(spv);
        SpirvCross(spv, msl, what);
        return File.ReadAllText(msl);
    }

    // The raster vertex shader that reads the kernel's output vertices (TranslatorContext.GenerateVertexPassthroughForCompute)
    static void WriteReader(string spv, string msl, string variant, int outputTopology, bool depthMinusW)
    {
        ShaderGeometry.FlattenSets(spv);
        SpirvCross(spv, msl, "spirv-cross (geometry reader): ");
        string src = PreciseMinMax(File.ReadAllText(msl));
        if (depthMinusW) src = ShaderGeometry.ReaderDepthMinusW(src);
        src = ShaderGeometry.Reader(src);   // after the depth conversion: a culled vertex keeps its position beyond the X clip plane
        File.WriteAllText(msl, src);
        var refl = new JsonObject
        {
            ["key"] = variant + "_r", ["route"] = "B", ["chain"] = chain, ["stage"] = "vs", ["entry"] = "main0", ["msl_entry_kind"] = "vertex",
            ["geometry_reader"] = true, ["geometry_output"] = outputTopology, ["depth_minus_w"] = depthMinusW,
            ["threadgroup_size"] = null, ["c1"] = "", ["notes"] = new JsonArray(), ["drv_reads"] = new JsonObject(), ["drv_unfillable"] = new JsonArray(),
            ["bindings"] = Reflect("vs", src, new JsonObject { ["sbufs"] = new JsonArray() }), ["vertex_inputs"] = new JsonArray(), ["raster"] = new JsonObject(),
        };
        WriteReflection(variant + "_r", refl);
    }

    /// A vertex shader's capture entry point main0_gs, appended to its MSL file. It stores the guest's clip-space position:
    /// with a -w..w depth mode the translator converts z in main0, so the capture body comes from a second translation
    /// without that conversion, taken only when the declarations before its entry point match main0's source
    /// (SameDeclarations). -> the reflection's "geometry_capture"
    static JsonObject AppendCapture(JsonObject rq, JsonObject job, string variant, string src, string msl)
    {
        string captureSrc = src;
        if (Truthy(Get(job, "depth_minus_w")))
        {
            string key = variant + "_gsz";
            var plain = (JsonObject)job.DeepClone();
            plain["key"] = key; plain["depth_minus_w"] = false;
            Translate(plain);
            string spv = Path.Combine(outDir, key + ".spv"), plainMsl = Path.Combine(outDir, key + ".metal");
            try
            {
                SpirvCross(spv, plainMsl, "spirv-cross (geometry capture): ");
                captureSrc = PostProcess(File.ReadAllText(plainMsl), DepthSlots(GetObj(rq, "tex")));
            }
            finally { foreach (string f in new[] { spv, plainMsl, Path.Combine(outDir, key + ".res.json") }) File.Delete(f); }
            if (!SameDeclarations(src, captureSrc))
                throw new RuntimeError("geometry capture: the translation without the depth conversion declares other types or helpers than main0's");
        }
        File.WriteAllText(msl, src + ShaderGeometry.Capture(captureSrc));
        return new JsonObject
        {
            ["entry"] = "main0_gs", ["buffer"] = ShaderGeometry.CaptureSlot, ["record_words"] = ShaderGeometry.Words,
            ["written_words"] = new JsonArray(ShaderGeometry.OutputWords(captureSrc).Keys.Select(k => (JsonNode)k).ToArray()),
            ["depth_converted"] = false,
        };
    }

    static readonly Regex SsaName = new(@"\b_\d+\b");

    /// Whether the capture body of `plain` (everything from its vertex entry on) may be appended to `converted`: the text
    /// before the two entry points is the same up to a consistent renaming of SPIR-V ids (SPIRV-Cross numbers a helper
    /// function's locals after the ids the depth conversion took), and every such id the plain entry names keeps its name.
    static bool SameDeclarations(string converted, string plain)
    {
        const string main = "vertex main0_out main0(";
        int a = converted.IndexOf(main, StringComparison.Ordinal), b = plain.IndexOf(main, StringComparison.Ordinal);
        if (a < 0 || b < 0) return false;
        string ca = converted.Substring(0, a), pb = plain.Substring(0, b);
        if (SsaName.Replace(ca, "_") != SsaName.Replace(pb, "_")) return false;
        var map = new Dictionary<string, string>(); var taken = new Dictionary<string, string>();
        foreach (var (p, c) in SsaName.Matches(pb).Zip(SsaName.Matches(ca), (p, c) => (p.Value, c.Value)))
        {
            if (map.TryGetValue(p, out string m) ? m != c : taken.TryGetValue(c, out string q) && q != p) return false;
            map[p] = c; taken[c] = p;
        }
        return SsaName.Matches(plain.Substring(b)).All(t => !map.TryGetValue(t.Value, out string c) || c == t.Value);
    }
}

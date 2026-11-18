// Original Switch shaders (Maxwell machine code) → SPIR-V (the Ryujinx.Graphics.Shader translator), for route B's offline metallib build.
// Difference from Ryujinx.ShaderTools: the translator's "queries" are answered truthfully rather than always with defaults -
//   texture type / image format / vertex attribute type / fragment output type / compute thread group size come from the job file (the game profile's answer table shader_facts.tsv, by the slot numbers of the reflection);
//   window origin upper-left; depth is not rewritten (z goes out unchanged, NVN's near=0 convention). A game's depth convention is runtime state (nvnDeviceSetDepthMode: Mario / Metroid etc. keep the default near=-w, the GL convention;
//   Link's Awakening / 13 Sentinels / Engage set near=0), so dmsh_build.py adds the GL rewrite z' = z/2 + w/2 in the MSL as two entry points, main0 (rewritten, GL) and main0_z (unchanged, near=0), and the host picks by the game's convention.
//   This used to rewrite for GL always: a near=0 game's depth was squeezed into 0.5..1, and effects that turn depth into distance (Link's Awakening's outdoor depth of field blurring the whole screen, occlusion) were all wrong.
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions),
//           "arch": "sm86" for Ampere code (NVN2, Switch 2: a 0x80-byte SPH and 128-bit instructions; absent = Maxwell),
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z] }]
//   Output: <key>.spv and <key>.res.json (offsets of storage buffer slots in c0, etc.; decoding texture handle → slot number is done in the script that builds the metallib;
//           "fetch1d": the texture slots read by 1D integer fetches translated as wrapped 2D reads, see RecordTexelFetch1D)
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    // NVN's handle tables in c[2], byte offsets, 8 bytes per slot (one layout in every game, see dm/gfx/tools/dmsh_build.py): combined textures 0x20 + 8 r, images 0x120 + 8 r (8 slots),
    // separate textures 0x168 + 8 r (slot 128 + r), separate samplers 0x568 + 8 r
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SepTexBase = 0x168, SepTexSlot0 = 128, SampBase = 0x568;
    // Handle words in c[2]: one every 8 bytes up to the end of the separate sampler table (128 slots), 301 in all. An array of c[2] handles the shader indexes at run time (Ryujinx's
    // texture array from a buffer, BindlessToArray: the index is the handle's byte offset ÷ 8) has one element per word, so any word the shader computes is in it
    const int C2HandleWords = (SampBase + 8 * 128) / 8;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job, Dictionary<int, SamplerType> imageTypes = null) { if (job["geometry_topology"] != null) _binding = 40; _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); _imageTypes = imageTypes; }
        // A texture query on an image handle (TXQ on c2 0x120 + 8 r: Unreal's GetDimensions on the RWTexture3D a compute shader writes) asks the type of that handle, which a table built
        // without the game's PC shaders does not answer, and the 2D default took a 3D image's depth from its width. The shader's own surface instructions give the image's type: the
        // first translation records that a query went unanswered (ImageQueryGuessed), and Main translates again with the types of the images it found, by c2 handle (ImageTypesOf)
        readonly Dictionary<int, SamplerType> _imageTypes; public bool ImageQueryGuessed;
        public static Dictionary<int, SamplerType> ImageTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            foreach (var im in p.Info.Images) if (im.CbufSlot == 2 || im.CbufSlot < 0) types[im.HandleIndex] = im.Type & (SamplerType.Mask | SamplerType.Array);
            return types;
        }
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public int QueryConstantBuffer1Size() => _c1.Length;
        public int[] QueryAmpereComputeC0Layout() => _job["nvn2_cs"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        public int[] QueryAmpereMeshLayout() => _job["mesh_layout"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;   // a task / mesh shader as compute (dmsh_build.py job_for)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 read out of bounds +{offset:X} (c1 length {_c1.Length})"); return 0; }
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        // The translator's own messages (a bindless read it resolved, or could not and removed, its result 0; an invalid encoding) go into the notes, marked, which dmsh_build.py counts apart
        public void Log(string message) => Notes.Add("translator: " + message);
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        // A texture read whose handle the translator cannot trace to one slot indexes the texture and sampler pools by the handle's IDs (20 and 12 bits); dmsh_build.py turns these
        // arrays into pointers to the pools the replay keeps (pool_arrays), with a run-time bound, so the lengths are the whole ID ranges
        public int QuerySamplerArrayLengthFromPool() => 1 << 12;
        // c[2]: every handle word (C2HandleWords). EA SPORTS FC 27's lights index separate texture i as element 45 + i (c2 byte 0x168 + 8 i), which 32 elements clamped to element 31.
        // Other buffers (the game's own uniform buffers, read through the handles' byte offsets as well) keep 32
        public int QueryTextureArrayLengthFromBuffer(int slot) => slot == 2 ? C2HandleWords : 32;
        public int QueryTextureArrayLengthFromPool() => 1 << 20;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        // A handle's texture or image slot, numbered as dmsh_build.py decode_tex numbers the reflection's (so an answer table row names the slot the reflection shows): the handle ORs two
        // c[2] words in no fixed order, so both are tried (only the first for a combined handle or a constant sampler); -1 when neither is in the table
        static int Slot(int handle, int cbufSlot, bool image)
        {
            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;
            (int w0, int w1, TextureHandleType ty) = TextureHandle.UnpackOffsets(handle);
            foreach (int w in ty == TextureHandleType.CombinedSampler || ty == TextureHandleType.SeparateConstantSamplerHandle ? new[] { w0 } : new[] { w0, w1 })
            {
                int b = w * 4;
                if (b % 8 != 0) continue;
                if (image && b >= ImgBase && b < ImgBase + 8 * ImgSlots) return (b - ImgBase) / 8;
                if (!image && b >= TexBase && b < SepTexBase) return (b - TexBase) / 8;
                if (!image && b >= SepTexBase && b < SampBase) return SepTexSlot0 + (b - SepTexBase) / 8;
            }
            return -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, false), islot = Slot(handle, cbufSlot, true);
            string t = (slot >= 0 ? Get("tex", slot) : null) ?? (islot >= 0 ? Get("img", islot) : null);
            if (t == null && islot >= 0 && cbufSlot < 0)
            {
                if (_imageTypes != null && _imageTypes.TryGetValue(handle, out SamplerType it)) { Notes.Add($"texture handle {handle:X} is image slot {islot}: type {it} from the shader's image instructions"); return it; }
                ImageQueryGuessed = true;
            }
            if (t == null) { Notes.Add($"texture handle {handle:X} (constant buffer {cbufSlot}, slot {slot}) has no type in the answer table, using 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        // Texture slots read by a 1D integer fetch that the translator made a wrapped 2D read (x & 0x3FFF, x >> 14) for want of a type answer: they may hold a 1D texture or a texture buffer,
        // which only the bound descriptor tells (res "fetch1d"; dmsh_build.py flags their bindings, and the host binds a texture buffer there as 16384-texel rows, nvn_mtl.m tbuf_as_2d)
        public readonly SortedSet<int> Fetch1D = new();
        public void RecordTexelFetch1D(int handle) { int slot = Slot(handle, -1, false); if (slot >= 0) Fetch1D.Add(slot); }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, true);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"image handle {handle:X} slot {slot} has no format in the answer table, using R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // the shared memory size in the compute shader's control section (shader_split.py)
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public bool QueryGeometryInputFixed() => _job["geometry_topology"] != null;
        // Compute lowering must emit passthrough vertices explicitly: Metal
        // cannot supply the native NV geometry-passthrough stage afterwards.
        public bool QueryHostSupportsGeometryShaderPassthrough() => _job["geometry_topology"] == null;
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            bool earlyZ = _job["early_fragment_tests"] is JsonNode ez && (bool)ez;   // control +0x718 (dmsh_build.py job_for): [[early_fragment_tests]] in the MSL
            return new GpuGraphicsState(earlyZ, _job["geometry_topology"] is JsonNode gt ? (InputTopology)(int)gt : InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* depth not rewritten: dmsh_build.py adds the GL entry point (see the file header) */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsDepthClipControl() => false;   // Metal has no -1..1 clip control; the depth convention is not left to the translator, the rewrite is added per entry point in the MSL (see the file header)
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Job softfloat64 = true: the host has no FP64, so Ryujinx's DoubleToFloat pass lowers double operations to float (Metal has no double type). Set only on the retry of a
        // shader whose first translation declared a 64-bit float (see Main): answering "no" for every shader would also drop the Float64 capability from all the others
        public bool QueryHostSupportsShaderFloat64() => !(_job["softfloat64"] is JsonNode n && (bool)n);
    }

    // True when the SPIR-V declares a 64-bit float type (OpTypeFloat 64): SPIRV-Cross then writes double, which the Metal compiler rejects
    static bool UsesFloat64(byte[] spirv)
    {
        for (int i = 20; i + 12 <= spirv.Length; )
        {
            uint word = BitConverter.ToUInt32(spirv, i);
            if ((word & 0xffff) == 22 && (word >> 16) == 3 && BitConverter.ToUInt32(spirv, i + 8) == 64) return true;
            i += Math.Max(1, (int)(word >> 16)) * 4;
        }
        return false;
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // first 16 digits of the sha256 of the embedded Program.cs + csproj (same convention as dmsh_chain.py)
        {
            var asm = typeof(SwshXlate).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = TranslationFlags.MetalImageAtomics | (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0) | ((string)job["arch"] == "sm86" ? TranslationFlags.Ampere : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                bool geometry = job["geometry_topology"] != null;
                ShaderProgram p = ctx.Translate(geometry);
                Dictionary<int, SamplerType> imageTypes = null;
                if (acc.ImageQueryGuessed && (imageTypes = Accessor.ImageTypesOf(p)).Count > 0)   // a query on an image handle went unanswered: again with the images' types (see Accessor)
                {
                    acc = new Accessor(code, job, imageTypes);
                    ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    p = ctx.Translate(geometry);
                }
                if (UsesFloat64(p.BinaryCode))   // Tears of the Kingdom 1.4.3: 30 shaders used double and failed in the Metal compiler
                {
                    job["softfloat64"] = true; acc = new Accessor(code, job, imageTypes);
                    ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    p = ctx.Translate(geometry); acc.Notes.Add("FP64 lowered to FP32 (DoubleToFloat)");
                }
                if (geometry)
                {
                    // A fresh context: translation mutates the decoded program.
                    var reader = Translator.CreateContext(0, new Accessor(code, job, imageTypes), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    File.WriteAllBytes(Path.Combine(outDir, key + ".reader.spv"), reader.GenerateVertexPassthroughForCompute().BinaryCode);
                }
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), VertexCounterLowering.Apply(code, (string)job["stage"], p.BinaryCode, acc.Notes));
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["geometry_stride"] = geometry ? ctx.GetResourceReservations().OutputSizePerInvocation : 0,
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                if (acc.Fetch1D.Count > 0) { var f1 = new JsonArray(); foreach (int h in acc.Fetch1D) f1.Add(h); res["fetch1d"] = f1; }
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"[RED] {key}: {e.GetType().Name}: {e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"translated: {ok} ok, {bad} failed");
        return bad == 0 ? 0 : 1;
    }
}

// Tears of the Kingdom 1.4.3 visibility counter (one vertex shader, exact code SHA-256 below): each invocation that passes its test joins a subgroup ballot, and the
// highest active lane adds popcount(ballot) to s1[0]. SPIRV-Cross cannot give a Metal vertex function the subgroup builtins ("Subgroup builtins are not available in
// this type of function"), so the shader never built and its draws were skipped. Lowered to singleton groups: lane id 0 and ballot(true) = (1, 0, 0, 0), so every passing
// invocation adds one; the atomic's result is unused, so the final sum (mod 2^32) is the same. Not a general subgroup fallback: it runs only on these analysed bytes.
// Adapted from MacNativeGames (games/totk/tools/totk_xlate/VertexCounterLowering.cs, https://github.com/ly05010419/MacNativeGames), MIT License: dm/third_party/macnativegames/LICENSE.
static class VertexCounterLowering
{
    const string InstanceCodeHash = "F5F4D23149565ACD852F6B7091F64CC8CB3AA019CCB33BA3F88CED62C6D1FF7B";

    internal static byte[] Apply(byte[] code, string stage, byte[] spirv, List<string> notes)
    {
        if (stage != "vs" || Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(code)) != InstanceCodeHash) return spirv;
        uint[] w = new uint[spirv.Length / 4]; Buffer.BlockCopy(spirv, 0, w, 0, spirv.Length);
        uint lane = 0, uintType = 0, vectorType = 0, trueId = 0; int ballots = 0, loads = 0;
        for (int i = 5; i < w.Length; i += (int)(w[i] >> 16))
        {
            uint op = w[i] & 65535;
            if (op == 71 && w[i + 2] == 11 && w[i + 3] == 41) lane = w[i + 1];   // OpDecorate BuiltIn SubgroupLocalInvocationId
            if (op == 21 && w[i + 2] == 32 && w[i + 3] == 0) uintType = w[i + 1];   // OpTypeInt 32 unsigned
            if (op == 41) trueId = w[i + 2];   // OpConstantTrue
            if (op == 339) { vectorType = w[i + 1]; ballots++; if (w[i + 4] != trueId) throw new InvalidOperationException("visibility counter: ballot predicate changed"); }   // OpGroupNonUniformBallot
        }
        if (lane == 0 || uintType == 0 || vectorType == 0 || ballots != 1) throw new InvalidOperationException("visibility counter: SPIR-V shape changed");
        uint zero = w[3]++, one = w[3]++, mask = w[3]++;   // new ids past the bound
        var result = new List<uint>(w.Length + 16); result.AddRange(w.AsSpan(0, 5).ToArray());
        bool constants = false;
        for (int i = 5; i < w.Length; i += (int)(w[i] >> 16))
        {
            uint op = w[i] & 65535;
            if (op == 54 && !constants)   // before the first OpFunction: uint 0, uint 1, uvec4(1, 0, 0, 0)
            {
                result.AddRange(new uint[] { (4u << 16) | 43, uintType, zero, 0, (4u << 16) | 43, uintType, one, 1, (7u << 16) | 44, vectorType, mask, one, zero, zero, zero });
                constants = true;
            }
            if (op == 61 && w[i + 3] == lane) { result.AddRange(new uint[] { (4u << 16) | 83, w[i + 1], w[i + 2], zero }); loads++; }   // OpLoad lane id -> OpCopyObject 0
            else if (op == 339) result.AddRange(new uint[] { (4u << 16) | 83, w[i + 1], w[i + 2], mask });   // ballot(true) -> OpCopyObject (1, 0, 0, 0)
            else result.AddRange(w.AsSpan(i, (int)(w[i] >> 16)).ToArray());
        }
        if (loads != 1 || !constants) throw new InvalidOperationException("visibility counter: lane id use changed");
        notes.Add("visibility counter lowered to singleton groups (one atomic add per passing vertex)");
        byte[] output = new byte[result.Count * 4]; Buffer.BlockCopy(result.ToArray(), 0, output, 0, output.Length);
        return output;
    }
}

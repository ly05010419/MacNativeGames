// TOTK adaptation (2026-10-12): adds a --serve mode (one job per stdin line) and records which texture/image
// slots the translator queried, so the host can key runtime translations by the answers actually used.
// Based on D2R Mobile dm/gfx/tools/dmsh_xlate/Program.cs (MIT, see LICENSE).
// Switch 原版着色器（Maxwell 机器码）→ SPIR-V（Ryujinx.Graphics.Shader 翻译器），供 B 路线离线出 metallib。
// 与 Ryujinx.ShaderTools 的区别：翻译器的「查询」按真实情况作答，而不是一律缺省值——
//   贴图类型 / 图像格式 / 顶点属性类型 / 片元输出类型 / 计算线程组大小取自作业文件（由映射表配对的 PC 反射给出，同槽号 = 同资源）；
//   窗口原点左上、深度 0..1（游戏实测 nvnDeviceSetWindowOriginMode / SetDepthMode）。
// 用法：dmsh_xlate <作业.json> <输出目录>
//   作业：[{ "key", "stage": "vs|ps|cs", "code": 机器码文件（图形 = 0x50 字节 SPH + 指令，计算 = 指令），
//            "tex": { "槽": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", … }, "img": { "槽": { "type", "fmt" } },
//            "attr": { "位置": "float|sint|uint" }, "fout": { "目标": "float|sint|uint" }, "local": [x, y, z] }]
//   输出：<key>.spv 与 <key>.res.json（存储缓冲槽在 c0 的偏移等，贴图句柄 → 槽号的解码在出 metallib 的脚本里做）
// Original Switch shaders (Maxwell machine code) → SPIR-V (the Ryujinx.Graphics.Shader translator), for route B's offline metallib build.
// Difference from Ryujinx.ShaderTools: the translator's "queries" are answered truthfully rather than always with defaults -
//   texture type / image format / vertex attribute type / fragment output type / compute thread group size come from the job file (given by the PC reflection paired via the mapping table; same slot number = same resource);
//   window origin upper-left, depth 0..1 (measured from the game: nvnDeviceSetWindowOriginMode / SetDepthMode).
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions),
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z] }]
//   Output: <key>.spv and <key>.res.json (offsets of storage buffer slots in c0, etc.; decoding texture handle → slot number is done in the script that builds the metallib)
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    const int TexBase = 0x168, SampBase = 0x568;   // NVN 分离贴图 / 采样器句柄在 c[2] 的字节基址（每槽 8 字节；dm/gfx/tools/maxwell_refs.py 全量实测） | byte base of NVN separate texture / sampler handles in c[2] (8 bytes per slot; full measurement in dm/gfx/tools/maxwell_refs.py)

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new(); public readonly SortedSet<int> TexQueried = new(), ImgQueried = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) TexQueried.Add(slot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            if (t.EndsWith("_depth")) t = t.Substring(0, t.Length - 6);   // depth-format texture: same sampler type here; the MSL declaration is rewritten to depth2d by shader_service.py
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // TOTK (2026-10-13): the game calls nvnDeviceSetDepthMode(NEAR_IS_MINUS_W) (GL clip range -w..w); a frame-800 uniform dump
            // showed the UI projection producing clip z = -1, which Metal (0..w) clips entirely, so no draw ever produced a fragment.
            // The job says "depth_minus_w": true for that mode and the translator then emits z = z * 0.5 + w * 0.5 (Metal has no
            // depth clip control, see QueryHostSupportsDepthClipControl below).
            bool depthMinusW = _job["depth_minus_w"] is JsonValue dm && (dm.TryGetValue<bool>(out bool db) ? db : dm.TryGetValue<int>(out int di) && di != 0);
            // Super Smash Bros. Ultimate (Q61): "alpha": "<NVN alpha function>:<reference>". Metal has no fixed-function alpha
            // test; with it Ryujinx discards fragments whose colour 0 alpha fails the compare. NVN numbers the functions as
            // AlphaTestOp does (1 never .. 8 always).
            AlphaTestOp alphaOp = AlphaTestOp.Always; float alphaRef = 0f;
            if (_job["alpha"] is JsonValue av && av.TryGetValue<string>(out string at))
            {
                string[] p = at.Split(':');
                if (p.Length != 2 || !int.TryParse(p[0], out int fn) || fn < 1 || fn > 8
                    || !float.TryParse(p[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out alphaRef))
                    throw new InvalidDataException($"alpha test answer \"{at}\" is not <function 1-8>:<reference>");
                alphaOp = (AlphaTestOp)fn;
            }
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, depthMinusW, false, 1f, alphaOp, alphaRef, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsDepthClipControl() => false;   // Metal clips z to 0..w only: a -w..w depth mode must be converted in the shader
        // Ryujinx's DoubleToFloat pass lowers FP64 operations and pack/unpack
        // helpers for Metal, which has no shader double type. Enabled only on
        // a retry for shaders that actually emit FP64 in the first pass.
        public bool QueryHostSupportsShaderFloat64() => _job["softfloat64"] is not JsonValue v || !(v.TryGetValue<bool>(out bool b) && b);
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Super Smash Bros. Ultimate (23.10.): a pixel shader gathers with four offsets computed at run time (TLD4 PTP). With
        // Ryujinx's default answers the SPIR-V got a constant composite of loaded values, which no SPIR-V reader accepts
        // ("ID has not been defined"; spirv-cross: nullptr). Metal has no gather with four offsets and takes only constant
        // offsets, so the answer is no for both: Ryujinx's TexturePass then splits the gather and moves the coordinates.
        public bool QueryHostSupportsTextureGatherOffsets() => false;
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
    }


    internal static string Translate(JsonObject job, string outDir)   // also called by the --service mode (Service.cs)
    {
        string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                ShaderProgram p = ctx.Translate();
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"),
                    VertexCounterLowering.Apply(code, (string)job["stage"], p.BinaryCode, acc.Notes));
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                    ["tex_queried"] = new JsonArray(acc.TexQueried.Select(x => (JsonNode)x).ToArray()), ["img_queried"] = new JsonArray(acc.ImgQueried.Select(x => (JsonNode)x).ToArray()),
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                return null;
            }
            catch (Exception e) { if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); return $"{e.GetType().Name}: {e.Message.Split('\n')[0]}"; }
    }

    // --disasm <code.bin> [vs|ps|cs]: print the decoded Maxwell instruction stream (address, name, raw opcode) with the
    // constant-buffer slot / offset of LDC and of cbuf-operand ALU forms, and the address register of global loads.
    static int Disasm(string codePath, string stage)
    {
        // reads the translator's private decoder state by reflection, which a NativeAOT build does not keep
        if (!System.Runtime.CompilerServices.RuntimeFeature.IsDynamicCodeSupported) { Console.Error.WriteLine("--disasm needs the JIT build: tooling/dotnet/dotnet build/totk_xlate/totk_xlate.dll --disasm ..."); return 2; }
        byte[] code = File.ReadAllBytes(codePath);
        var job = new JsonObject { ["key"] = "disasm", ["stage"] = stage, ["c1"] = "", ["debug"] = true, ["reduced"] = true };
        var acc = new Accessor(code, job);
        var flags = TranslationFlags.DebugMode | (stage == "cs" ? TranslationFlags.Compute : 0);
        var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
        var f = typeof(TranslatorContext).GetField("_program", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        var program = (System.Collections.IEnumerable)f.GetValue(ctx);
        foreach (object func in program)
        {
            var blocks = (Array)func.GetType().GetProperty("Blocks").GetValue(func);
            foreach (object block in blocks)
            {
                var ops = (System.Collections.IEnumerable)block.GetType().GetProperty("OpCodes").GetValue(block);
                foreach (object op in ops)
                {
                    ulong addr = (ulong)op.GetType().GetField("Address").GetValue(op);
                    ulong raw = (ulong)op.GetType().GetField("RawOpCode").GetValue(op);
                    string name = op.GetType().GetField("Name").GetValue(op).ToString();
                    string extra = "";
                    if (name == "Ldc") extra = $" c[{(raw >> 36) & 0x1f}][{(raw >> 20) & 0xffff:x}] + R{(raw >> 8) & 0xff} -> R{raw & 0xff} size={(raw >> 48) & 7} mode={(raw >> 44) & 3}";
                    else if (name == "Ldg" || name == "Ld" || name == "Stg" || name == "St") extra = $" [R{(raw >> 8) & 0xff} + {(raw >> 20) & 0xffffff:x}] R{raw & 0xff} size={(raw >> 48) & 7} e={(raw >> 45) & 1}";
                    else if (name.EndsWith("C")) extra = $" c[{(raw >> 34) & 0x1f}][{((raw >> 20) & 0x3fff) * 4:x}] rA=R{(raw >> 8) & 0xff} -> R{raw & 0xff}";
                    else if (name.EndsWith("R") || name.EndsWith("I")) extra = $" rA=R{(raw >> 8) & 0xff} rB=R{(raw >> 20) & 0xff} -> R{raw & 0xff}";
                    Console.WriteLine($"{addr:x4}: {name,-12} {raw:x16}{extra}");
                }
            }
        }
        return 0;
    }

    static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "--disasm") return Disasm(args[1], args.Length > 2 ? args[2] : "vs");
        if (args.Length >= 1 && args[0] == "--service") return ShaderService.Run(args);   // the runtime's translation service (Service.cs)
        if (args.Length == 1 && args[0] == "--serve")
        {
            // One request per line: {"job": {...}, "out": "<dir>"}; one reply line "ok <key>" / "fail <key> <message>".
            string line;
            while ((line = Console.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                var req = JsonNode.Parse(line).AsObject(); var job = req["job"].AsObject(); string o = (string)req["out"];
                Directory.CreateDirectory(o);
                string err = Translate(job, o);
                Console.WriteLine(err == null ? $"ok {(string)job["key"]}" : $"fail {(string)job["key"]} {err}");
                Console.Out.Flush();
            }
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string err = Translate(job, outDir);
            if (err == null) ok++; else { bad++; Console.Error.WriteLine($"【红】{(string)job["key"]}：{err}"); }
        }
        Console.WriteLine($"翻译 {ok} 成功，{bad} 失败");
        return bad == 0 ? 0 : 1;
    }
}

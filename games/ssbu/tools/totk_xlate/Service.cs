// TOTK (2026-10-17): the runtime shader translation service as one native program: totk_xlate --service.
// Same request / reply lines and the same cache files as scripts/shader_service.py, which it replaces when
// build/shader-service/totk_xlate exists (totk_shader_rt.m); the translation runs in this process instead of a second
// "dotnet totk_xlate.dll --serve" process, and published with NativeAOT (make shader-service) it needs neither Python nor
// a .NET runtime. This is a line-by-line port of shader_service.py: keep the two in step, and check a change with
// scripts/diag/retranslate_cache.py --service build/shader-service/totk_xlate --compare <cache> (every MSL byte for byte,
// every reflection file as JSON).
// Request: {"variant", "stage": "vs|ps|cs", "code": path, "c1": hex, "tex": {slot: type}, "img": {slot: {type, fmt}},
//           "attr": {loc: float|sint|uint}, "fout": {idx: float|sint|uint}, "local": [x, y, z], "depth_minus_w": bool,
//           "alpha": "<NVN alpha function>:<reference>" (ps with an alpha test only)}
// Reply:   {"ok": true} or {"ok": false, "error": "..."}; outputs <out>/<variant>_b.metal and <variant>_b.json.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static class ShaderService
{
    sealed class RuntimeError : Exception { public RuntimeError(string message) : base(message) { } }

    const int TexBase = 0x168, SampBase = 0x568, ImgBase = 0x120;
    // TOTK (NVN 55): combined texture handles bound with nvnCommandBufferBindTexture live at c2+0x20, 32 slots of 8 bytes
    // (measured over 300 pixel and 400 vertex shaders of the game); separate textures stay at 0x168, samplers at 0x568.
    const int CombinedBase = 0x20, CombinedSlots = 32;
    static readonly string[] Xc = { "--msl", "--msl-version", "20300", "--msl-argument-buffers", "--msl-texture-buffer-native", "--msl-pad-fragment-output" };
    // Driver-segment words the replay fills, per stage (nvn_mtl.m drv_fillable / encode_args): c0 storage-buffer table of the
    // stage, compute thread-group counts (0x200) and uniform-buffer table (0x210), vertex point-size limit (0x610); c2 handles.
    static readonly Dictionary<string, (long lo, long hi)[]> DrvFillC0 = new()
    {
        ["vs"] = new[] { (0x110L, 0x210L), (0x610L, 0x620L) }, ["ps"] = new[] { (0x510L, 0x610L) }, ["cs"] = new[] { (0x200L, 0x410L) },
    };
    static readonly (long lo, long hi)[] DrvFillC2 = { (0x20, 0x120), (0x168, 0x568), (0x568, 0x968) };
    static readonly Dictionary<string, string> StagePrefix = new() { ["vs"] = "vp", ["ps"] = "fp", ["cs"] = "cp" };
    // Depth-format textures sampled through plain texture slots: Metal returns nothing for a depth texture read as texture2d,
    // so the declaration becomes depth2d / depth2d_array / depthcube (same for _ms) and every .sample / .read on it is wrapped
    // so the scalar depth becomes (d, 0, 0, 1) like a depth texture fetch on NVN. .gather already returns float4 on depth.
    static readonly Dictionary<string, string> DepthTypes = new()
    {
        ["texture2d"] = "depth2d", ["texture2d_array"] = "depth2d_array", ["texturecube"] = "depthcube",
        ["texturecube_array"] = "depthcube_array", ["texture2d_ms"] = "depth2d_ms", ["texture2d_ms_array"] = "depth2d_ms_array",
    };
    static readonly Regex Fp64 = new(@"\bdouble\b|unsupported_GLSLstd450(?:Pack|Unpack)Double2x32");

    static string spirvCross, outDir, chain;
    static int debugMode;

    // ---- small helpers with Python's meaning ----
    static long ParseInt(string s) => long.Parse(s, NumberStyles.None, CultureInfo.InvariantCulture);
    static long ParseHex(string s) => long.Parse(s, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture);
    static long FloorDiv(long a, long b) => a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);   // Python //
    static string Slice(string s, int start, int stop)   // Python s[start:stop] for 0 <= start
    {
        stop = Math.Min(stop, s.Length); start = Math.Min(start, s.Length);
        return stop <= start ? "" : s.Substring(start, stop - start);
    }
    static string Cut(string s, int n) => s.Length > n ? s.Substring(0, n) : s;
    static bool Truthy(JsonNode n) => n switch
    {
        null => false,
        JsonObject o => o.Count > 0,
        JsonArray a => a.Count > 0,
        JsonValue v => v.GetValueKind() switch
        {
            JsonValueKind.True => true, JsonValueKind.False => false,
            JsonValueKind.String => v.GetValue<string>().Length > 0,
            JsonValueKind.Number => v.TryGetValue(out long l) ? l != 0 : v.GetValue<double>() != 0,
            _ => false,
        },
        _ => false,
    };
    static JsonNode Get(JsonObject o, string key) => o.TryGetPropertyValue(key, out JsonNode v) ? v : null;
    static JsonObject GetObj(JsonObject o, string key) => Get(o, key) as JsonObject ?? new JsonObject();   // rq.get(key, {})
    static string GetStr(JsonObject o, string key, string fallback) => o.TryGetPropertyValue(key, out JsonNode v) ? (string)v : fallback;
    static JsonNode GetOr(JsonObject o, string key, JsonNode fallback) => o.TryGetPropertyValue(key, out JsonNode v) ? v?.DeepClone() : fallback;
    static JsonNode Num(long? v) => v is long l ? JsonValue.Create(l) : null;
    static long Long(JsonNode n) => n.AsValue().TryGetValue(out long l) ? l : (long)n.GetValue<double>();

    // ---- binding names -> NVN slots (shader_service.py decode_tex / tex_slot_of_name) ----
    static long? Slot(long word, long @base, long cb) => cb == 2 && word * 4 >= @base && (word * 4 - @base) % 8 == 0 ? (word * 4 - @base) / 8 : null;

    static (long? t, long? s, long ty)? DecodeTex(string name)
    {
        Match m = Regex.Match(name, @"^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)");
        if (!m.Success) return null;
        long slots = m.Groups[1].Success ? ParseInt(m.Groups[1].Value) : -1;
        long h = ParseHex(m.Groups[2].Value);
        long tb = slots >= 0 ? (slots & 0xffff) : 2;
        long sb = slots >= 0 && (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
        long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
        long[] words = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
        if (Regex.IsMatch(name, @"^[a-z]+_i"))
            return (words.Where(x => ImgBase <= x * 4 && x * 4 < TexBase && Slot(x, ImgBase, tb) != null).Select(x => Slot(x, ImgBase, tb)).FirstOrDefault(), null, ty);
        long? t = words.Where(x => Slot(x, TexBase, tb) != null && x * 4 < SampBase).Select(x => Slot(x, TexBase, tb)).FirstOrDefault();
        if (t == null)
            t = words.Where(x => CombinedBase <= x * 4 && x * 4 < CombinedBase + 8 * CombinedSlots && Slot(x, CombinedBase, tb) != null).Select(x => Slot(x, CombinedBase, tb)).FirstOrDefault();
        long? s = words.Where(x => x * 4 >= SampBase && Slot(x, SampBase, sb) != null).Select(x => Slot(x, SampBase, sb)).FirstOrDefault();
        return (t, s, ty);
    }

    static long TexSlotOfName(string name)
    {
        Match m = Regex.Match(name, @"^\w+_t_tcb_([0-9A-Fa-f]+)$");
        return m.Success ? FloorDiv(ParseHex(m.Groups[1].Value) - 8, 2) : -1;
    }

    static readonly Regex TexDecl = new(@"\b(?:texture\w+|depth\w+)<float>\s+(\w+)\s*\[\[id\(\d+\)\]\]");

    /// NVN texture slots of the textures the MSL samples with a depth compare (sample_compare / gather_compare): shadow
    /// maps. The game binds depth textures to all of a shader's compare slots or to none of them (1,248 and 126 of the
    /// variants it translated on the Mac by 2026-10-22, never some), so the seed prepares both (prepare_shader_seed.py).
    static SortedSet<long> CompareSlots(string src)
    {
        var slots = new SortedSet<long>();
        string[] lines = src.Split('\n');
        foreach (Match m in TexDecl.Matches(src))
        {
            string name = m.Groups[1].Value;
            long slot = TexSlotOfName(name);
            if (slot < 0) continue;
            var use = new Regex(@"\b" + Regex.Escape(name) + @"\b");
            if (lines.Any(line => use.IsMatch(line) && !line.Contains("[[id(", StringComparison.Ordinal) && line.Contains("_compare(", StringComparison.Ordinal)))
                slots.Add(slot);
        }
        return slots;
    }

    static readonly Regex AttrDecl = new(@"\b\w+\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]");
    static readonly Regex Ssa = new(@"\b_\d+\b");

    /// location -> "uint" | "float" for every vertex input: "uint" when every value loaded from it is only ever bitcast to
    /// an integer, converted with int() / uint(), or used as an index (the game binds an integer vertex format there:
    /// 834 of 837 integer attributes the Mac translated by 2026-10-22, the other 3 mixed in a float use; no integer
    /// attribute was ever used as float only). A value with any float use, or an input read any other way, is "float".
    static Dictionary<string, string> InferAttributeTypes(string src)
    {
        var result = new Dictionary<string, string>();
        string[] lines = src.Split('\n');
        // where each SSA value is used (its defining line excluded)
        var usedIn = new Dictionary<string, List<string>>();
        foreach (string line in lines)
        {
            Match def = Regex.Match(line, @"^\s*\w+\s+(_\d+)\s*=");
            string defined = def.Success ? def.Groups[1].Value : null;
            foreach (Match t in Ssa.Matches(line))
            {
                if (t.Value == defined) continue;
                if (!usedIn.TryGetValue(t.Value, out var l)) usedIn[t.Value] = l = new List<string>();
                l.Add(line);
            }
        }
        foreach (Match m in AttrDecl.Matches(src))
        {
            string field = m.Groups[1].Value, loc = m.Groups[2].Value;
            var load = new Regex(@"^\s*\w+\s+(_\d+)\s*=\s*(?:as_type<\w+>\()?in\." + field + @"\.[xyzw]\)?;\s*$");
            var any = new Regex(@"\bin\." + field + @"\b");
            bool intUse = false, floatUse = false;
            foreach (string line in lines)
            {
                Match l = load.Match(line);
                if (!l.Success) { if (any.IsMatch(line) && !line.Contains("[[attribute(", StringComparison.Ordinal)) floatUse = true; continue; }
                string v = l.Groups[1].Value;
                var intPat = new Regex(@"as_type<u?int>\(" + v + @"\)|\bu?int\(" + v + @"\)|\[" + v + @"\]");
                if (usedIn.TryGetValue(v, out var uses))
                    foreach (string u in uses) { if (intPat.IsMatch(u)) intUse = true; else floatUse = true; }
            }
            result[loc] = intUse && !floatUse ? "uint" : "float";
        }
        return result;
    }

    static int CloseParen(string src, int i)   // index just past the ')' closing a call whose '(' ends at i (or src.Length)
    {
        int depth = 1;
        while (i < src.Length && depth != 0) { depth += src[i] == '(' ? 1 : src[i] == ')' ? -1 : 0; i++; }
        return depth == 0 ? i : -i - 1;
    }

    static string RewriteDepthTextures(string src, HashSet<long> depthSlots)
    {
        var names = new List<(string ty, string name)>();
        foreach (Match m in Regex.Matches(src, @"\b(texture\w+)<float>\s+(\w+)\s*\[\[id\((\d+)\)\]\]"))
        {
            string ty = m.Groups[1].Value, name = m.Groups[2].Value;
            if (!depthSlots.Contains(TexSlotOfName(name))) continue;
            // Q92 (Smash, 2026-10-25): a depth texture bound where the shader samples a type with no depth counterpart (ps
            // 9dcc392f…, a 3D colour table) keeps the shader's own declaration; the runtime binds the empty texture of that type
            // there (nvn_mtl.m), as for an unbound slot. This threw, and the game ended.
            if (!DepthTypes.ContainsKey(ty)) continue;
            names.Add((ty, name));
        }
        foreach (var (ty, name) in names)
        {
            string decl = DepthTypes[ty] + "<float> " + name;
            src = new Regex(@"\b" + ty + @"<float>\s+" + name + @"\b").Replace(src, _ => decl, 1);
            // compare sampling: spirv-cross casts the texture2d to depth2d itself; the declaration is now depth2d, drop the cast
            src = Regex.Replace(src, @"spvDepthCast\(((?:\w+\.)*" + name + @")\)", m => m.Groups[1].Value);
            // depth2d.gather takes no component argument (only the depth component exists)
            var gpat = new Regex(@"((?:\w+\.)*" + name + @")\.gather\(");
            int pos = 0;
            while (pos <= src.Length)
            {
                Match m = gpat.Match(src, pos);
                if (!m.Success) break;
                int end = m.Index + m.Length, c = CloseParen(src, end), i = c >= 0 ? c : -c - 1;
                string call = Slice(src, end, i - 1);
                if (Regex.IsMatch(call, @"component::[yzw]")) throw new RuntimeError($"gather of a non-x component on depth texture {name}");
                string call2 = Regex.Replace(call, @",\s*component::x", "");
                src = src.Substring(0, end) + call2 + src.Substring(i - 1);
                pos = end + call2.Length + 1;
            }
            var pat = new Regex(@"((?:\w+\.)*" + name + @")\.(sample|read)\(");
            pos = 0;
            while (pos <= src.Length)
            {
                Match m = pat.Match(src, pos);
                if (!m.Success) break;
                int start = m.Groups[1].Index, c = CloseParen(src, m.Index + m.Length);
                if (c < 0) throw new RuntimeError($"unbalanced call on depth texture {name}");
                src = src.Substring(0, start) + "totk_depth_fetch(" + src.Substring(start, c - start) + ")" + src.Substring(c);
                pos = c + "totk_depth_fetch()".Length;
            }
        }
        if (names.Count > 0)
        {
            const string ns = "using namespace metal;";
            int at = src.IndexOf(ns, StringComparison.Ordinal);
            if (at >= 0) src = src.Substring(0, at) + ns + "\nstatic inline float4 totk_depth_fetch(float d) { return float4(d, 0.0, 0.0, 1.0); }" + src.Substring(at + ns.Length);
        }
        return src;
    }

    // Two depth sampling differences between Maxwell and Metal (PR #104, issue #89: dark patches on ground and walls, a dark
    // halo around Link on the wall behind him).
    //  - Shadow compares on texture arrays (the game's cascaded shadow maps, Depth16Unorm): Maxwell rounds the reference to
    //    the texture's precision before comparing, Metal compares the full float, so a lit surface fails its own compare by
    //    a fraction of a 16-bit step. The reference is rounded to 1/65535 like the depth write. The 2D depth copies are D32F
    //    and never compare-sampled.
    //  - Non-compare reads of a depth texture (totk_depth_fetch above): the game binds a trilinear sampler, and on the Switch
    //    the read lands on one texel; Metal blends neighbouring depths, which puts reconstructed positions in mid-air at
    //    object edges. These reads are point-sampled.
    static readonly Regex D16Compare = new(@"(\.sample_compare\(spvDescriptorSet0\.[A-Za-z0-9_]+Smplr, [^,]+\.xy, uint\(rint\([^)]+\)\), )([^,]+)(, level)");
    static readonly Regex DepthFetchSample = new(@"totk_depth_fetch\((spvDescriptorSet0\.[A-Za-z0-9_]+)\.sample\(spvDescriptorSet0\.[A-Za-z0-9_]+Smplr, ");

    static string MatchSwitchDepthSampling(string src)
    {
        string s = D16Compare.Replace(src, "${1}totk_d16ref(${2})${3}");
        s = DepthFetchSample.Replace(s, "totk_depth_fetch(${1}.sample(totk_nearest, ");
        if (s == src) return src;
        const string ns = "using namespace metal;";
        int at = s.IndexOf(ns, StringComparison.Ordinal);
        if (at < 0) throw new RuntimeError("depth sampling rewrite: no 'using namespace metal;'");
        return s.Substring(0, at) + ns +
            "\nstatic inline float totk_d16ref(float r) { return floor(fast::clamp(r, 0.0f, 1.0f) * 65535.0f + 0.5f) * (1.0f / 65535.0f); }" +
            "\nconstexpr sampler totk_nearest(filter::nearest, mip_filter::none, address::clamp_to_edge);" + s.Substring(at + ns.Length);
    }

    /// True when the MSL stores or does atomics through storage binding `name` (same rule as nvn_mtl.m msl_stores_through).
    static bool MslStoresThrough(string msl, string name)
    {
        if (msl.Contains($"&(*spvDescriptorSet0.{name})", StringComparison.Ordinal)) return true;
        return Regex.IsMatch(msl, @"\(\*spvDescriptorSet0\." + Regex.Escape(name) + @"\)\._m0\[(?:[^\[\]]|\[[^\[\]]*\])*\](?:\.[xyzw]+)?\s*=(?!=)");
    }

    static JsonArray Reflect(string stage, string msl, JsonObject res)
    {
        Match body = Regex.Match(msl, @"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", RegexOptions.Singleline);
        var binds = new List<JsonObject>();
        foreach (Match m in Regex.Matches(body.Success ? body.Groups[1].Value : "", @"^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];", RegexOptions.Multiline))
        {
            string ty = m.Groups[1].Value.Trim(), nm = m.Groups[2].Value;
            var e = new JsonObject { ["msl_name"] = nm, ["msl_type"] = ty, ["arg_id"] = ParseInt(m.Groups[3].Value) };
            Match fm;
            if (nm == "support_buffer") e["kind"] = "support";
            else if ((fm = Regex.Match(nm, @"^[a-z]+_c(\d+)\z")).Success)
            {
                long n = ParseInt(fm.Groups[1].Value);
                if (3 <= n && n <= 16) { e["kind"] = "cbuf"; e["reg"] = n - 3; }
                else { e["kind"] = n == 1 ? "c1" : "drvcb"; e["bank"] = n; }
            }
            else if ((fm = Regex.Match(nm, @"^[a-z]+_s(\d+)\z")).Success) { e["kind"] = "sbuf"; e["index"] = ParseInt(fm.Groups[1].Value); }
            else if (nm.EndsWith("Smplr", StringComparison.Ordinal))
            {
                var d = DecodeTex(nm.Substring(0, nm.Length - 5)) ?? throw new RuntimeError($"sampler {nm}: no texture handle in the name");
                if (d.ty != 0) { e["kind"] = "samp"; e["reg"] = Num(d.s); } else { e["kind"] = "samp_tex"; e["reg"] = Num(d.t); }
            }
            else
            {
                var d = DecodeTex(nm);
                if (d == null) e["kind"] = "?";
                else { e["kind"] = Regex.IsMatch(nm, @"^[a-z]+_i") ? "img" : (ty.Contains("texture_buffer") ? "tbuf" : "tex"); e["reg"] = Num(d.Value.t); }
            }
            binds.Add(e);
        }
        long? @base = stage switch { "vs" => 0x110, "cs" => 0x310, "ps" => 0x510, _ => null };
        List<JsonObject> sbSorted = res["sbufs"].AsArray().Select(b => b.AsObject()).OrderBy(b => Long(b["binding"])).ToList();   // stable, as sorted()
        foreach (JsonObject e in binds)
        {
            if ((string)e["kind"] != "sbuf") continue;
            long index = Long(e["index"]);
            JsonObject b = index < sbSorted.Count ? sbSorted[(int)index] : null;
            long cbSlot = b != null ? Long(b["cb_slot"]) : 0, cbOffset = b != null ? Long(b["cb_offset"]) : 0;
            long? off = b != null && cbSlot == 0 ? cbOffset * 4 : null;
            if (stage == "cs" && off != null && 0x210 <= off && off < 0x310 && (off - 0x210) % 16 == 0) { e["kind"] = "ubo_ptr"; e["reg"] = (off - 0x210) / 16; }
            else if (b != null && cbSlot != 0 && 3 <= cbSlot && cbSlot <= 16)
            {
                // the shader itself loads this storage buffer's address from a user uniform block (NVN uniform slot cb_slot-3,
                // byte offset cb_offset*4); the replay reads that pointer at bind time
                e["kind"] = "sbuf_ubo"; e["reg"] = cbSlot - 3; e["off"] = cbOffset * 4;
            }
            else e["reg"] = off != null && @base != null && 0 <= off - @base && off - @base < 0x100 && (off - @base) % 16 == 0 ? Num((off - @base) / 16) : null;
            // Ryujinx BufferUsageFlags.Write is 1 << 0; the MSL is the other witness: a store or an atomic through the binding
            e["rw"] = (b != null && (Long(b["flags"]) & 1) != 0) || MslStoresThrough(msl, (string)e["msl_name"]);
        }
        return new JsonArray(binds.Select(x => (JsonNode)x).ToArray());
    }

    static JsonObject DrvReads(string src, string stage)
    {
        var dr = new JsonObject();
        foreach (int bank in new[] { 0, 2 })
        {
            var offs = new SortedSet<long>(); bool dyn = false;
            foreach (Match m in Regex.Matches(src, StagePrefix[stage] + "_c" + bank.ToString(CultureInfo.InvariantCulture) + @"\)\._m0\[([^\]]+)\]"))
            {
                string idx = m.Groups[1].Value.TrimEnd('u');
                if (idx.Length > 0 && idx.All(c => c >= '0' && c <= '9')) offs.Add(ParseInt(idx) * 16); else dyn = true;
            }
            if (offs.Count > 0 || dyn)
                dr[bank.ToString(CultureInfo.InvariantCulture)] = new JsonObject { ["vec4_offsets"] = new JsonArray(offs.Select(o => (JsonNode)o).ToArray()), ["dynamic"] = dyn };
        }
        return dr;
    }

    static JsonArray Unfillable(JsonObject dr, string stage)
    {
        var bad = new JsonArray();
        foreach (var kv in dr)
        {
            JsonObject d = kv.Value.AsObject();
            if ((bool)d["dynamic"]) bad.Add((JsonNode)$"c{kv.Key} dynamic index");
            long bank = ParseInt(kv.Key);
            var ranges = bank == 2 ? DrvFillC2 : bank == 0 ? DrvFillC0[stage] : Array.Empty<(long lo, long hi)>();
            foreach (JsonNode o in d["vec4_offsets"].AsArray())
            {
                long ov = Long(o);
                if (!ranges.Any(r => r.lo <= ov && ov < r.hi)) bad.Add((JsonNode)$"c{kv.Key}+0x{ov.ToString("x", CultureInfo.InvariantCulture)}");
            }
        }
        return bad;
    }

    static void SpirvCross(string spv, string msl, string what)
    {
        var psi = new ProcessStartInfo(spirvCross) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string a in Xc) psi.ArgumentList.Add(a);
        psi.ArgumentList.Add(spv); psi.ArgumentList.Add("--output"); psi.ArgumentList.Add(msl);
        using Process p = Process.Start(psi) ?? throw new RuntimeError(what + "could not start " + spirvCross);
        var err = p.StandardError.ReadToEndAsync(); var outp = p.StandardOutput.ReadToEndAsync();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new RuntimeError(what + Cut(err.Result.Trim(), 300));
        _ = outp.Result;
    }

    static void Translate(JsonObject job)
    {
        string err = SwshXlate.Translate(job, outDir);
        if (err != null) throw new RuntimeError(Cut($"fail {(string)job["key"]} {err}".Trim(), 300));
    }

    static JsonObject ReadRes(string variant) => JsonNode.Parse(File.ReadAllText(Path.Combine(outDir, variant + ".res.json"))).AsObject();

    static void Handle(JsonObject rq)
    {
        string variant = (string)rq["variant"], stage = (string)rq["stage"];
        if (stage != "vs" && stage != "ps" && stage != "cs") throw new RuntimeError($"stage {stage} not supported by the translation chain");
        if (stage == "cs" && !Truthy(Get(rq, "local"))) throw new RuntimeError("compute shader without thread group size");
        // DebugMode (AST optimizer off) since 2026-10-13: --debug-mode / TOTK_SHADER_DEBUGMODE, as in shader_service.py
        var job = new JsonObject
        {
            ["key"] = variant, ["stage"] = stage, ["code"] = (string)rq["code"], ["c1"] = GetStr(rq, "c1", ""), ["debug"] = debugMode != 0, ["reduced"] = true,
            ["tex"] = GetObj(rq, "tex").DeepClone(), ["img"] = GetObj(rq, "img").DeepClone(), ["attr"] = GetObj(rq, "attr").DeepClone(), ["fout"] = GetObj(rq, "fout").DeepClone(),
        };
        if (Truthy(Get(rq, "local"))) job["local"] = rq["local"].DeepClone();
        job["depth_minus_w"] = Truthy(Get(rq, "depth_minus_w"));   // nvnDeviceSetDepthMode(NEAR_IS_MINUS_W): clip z converted to Metal's 0..w
        // Super Smash Bros. Ultimate (Q61): the alpha test "<function>:<reference>" (nvn_mtl.m alpha_answer), done by the
        // translator in the fragment shader; Metal has no fixed-function alpha test.
        string alpha = stage == "ps" ? GetStr(rq, "alpha", null) : null;
        if (alpha != null) job["alpha"] = alpha;
        Translate(job);
        JsonObject res = ReadRes(variant);
        string spv = Path.Combine(outDir, variant + ".spv"), msl = Path.Combine(outDir, variant + "_b.metal");
        SpirvCross(spv, msl, "spirv-cross: ");
        string src = File.ReadAllText(msl);
        JsonObject tex = GetObj(rq, "tex"), img = GetObj(rq, "img"), attr = GetObj(rq, "attr"), fout = GetObj(rq, "fout");
        // Vertex inputs the request leaves unanswered (the prepared seed answers none) are typed by their use: an input
        // the code only ever bitcasts to an integer or indexes with is an integer attribute (shader_variants.md). The
        // translation is redone with those answers, so the result is the one an answered request gives.
        if (stage == "vs")
        {
            bool again = false;
            foreach (var kv in InferAttributeTypes(src))
                if (!attr.ContainsKey(kv.Key) && kv.Value != "float") { job["attr"][kv.Key] = kv.Value; again = true; }
            if (again)
            {
                Translate(job);
                res = ReadRes(variant);
                SpirvCross(spv, msl, "spirv-cross (inferred attributes): ");
                src = File.ReadAllText(msl);
            }
        }
        bool softfloat64 = Fp64.IsMatch(src);
        if (softfloat64)
        {
            // Metal has no FP64 in shaders: Ryujinx's DoubleToFloat lowering for this variant only
            job["softfloat64"] = true;
            Translate(job);
            res = ReadRes(variant);
            SpirvCross(spv, msl, "spirv-cross FP64 fallback: ");
            src = File.ReadAllText(msl);
            if (Fp64.IsMatch(src)) throw new RuntimeError("Ryujinx FP64 fallback still emitted Metal-unsupported double");
        }
        var depthSlots = new HashSet<long>(tex.Where(kv => kv.Value is JsonValue v && v.GetValueKind() == JsonValueKind.String && v.GetValue<string>().EndsWith("_depth", StringComparison.Ordinal))
                                            .Select(kv => ParseInt(kv.Key)));
        var compareSlots = CompareSlots(src);   // before the depth rewrite: it drops spvDepthCast, not the compare
        if (depthSlots.Count > 0)
            src = RewriteDepthTextures(src, depthSlots);
        src = MatchSwitchDepthSampling(src);
        if (depthSlots.Count > 0 || src != File.ReadAllText(msl))
            File.WriteAllText(msl, src);
        Match entry = Regex.Match(src, @"(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(");
        JsonObject dr = DrvReads(src, stage);
        JsonArray unfillable = Unfillable(dr, stage);
        var vertexInputs = new JsonArray(Regex.Matches(src, @"(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]")
            .Select(m => (JsonNode)new JsonObject { ["location"] = ParseInt(m.Groups[2].Value), ["msl_type"] = m.Groups[1].Value }).ToArray());
        Match outStruct = Regex.Match(src, @"struct main0_out\s*\{(.*?)\};", RegexOptions.Singleline);
        var colors = new SortedSet<long>(Regex.Matches(outStruct.Success ? outStruct.Groups[1].Value : "", @"\[\[color\((\d+)\)\]\]").Select(m => ParseInt(m.Groups[1].Value)));
        // Answers this translation depends on: queried texture / image slots, consumed vertex inputs, written colour outputs.
        // A texture slot the MSL binds without having queried its kind takes that kind from the sampling instruction, so
        // only whether a depth-format texture sits there changes the MSL: "*" (any colour texture) or "*_depth"
        // (docs/shader-variants.md).
        var usedSlots = new HashSet<long>(Regex.Matches(src, @"\b(texture\w+|depth\w+)<float>\s+(\w+)\s*\[\[id\(\d+\)\]\]").Select(m => TexSlotOfName(m.Groups[2].Value)));
        usedSlots.Remove(-1);
        var deps = new JsonObject { ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject() };
        IEnumerable<long> Ints(string key) => res.TryGetPropertyValue(key, out JsonNode a) && a is JsonArray arr ? arr.Select(Long) : Enumerable.Empty<long>();
        var queried = new HashSet<long>(Ints("tex_queried"));
        foreach (long s in new SortedSet<long>(queried.Concat(usedSlots)))
        {
            string slot = s.ToString(CultureInfo.InvariantCulture);
            deps["tex"][slot] = queried.Contains(s) ? GetOr(tex, slot, "2d") : depthSlots.Contains(s) ? "*_depth" : "*";
        }
        foreach (long s in Ints("img_queried"))
            deps["img"][s.ToString(CultureInfo.InvariantCulture)] = GetOr(img, s.ToString(CultureInfo.InvariantCulture), new JsonObject());
        if (stage == "vs")   // the answered type, or the one inferred from the code (job["attr"] holds both)
            foreach (JsonNode v in vertexInputs) { string loc = Long(v["location"]).ToString(CultureInfo.InvariantCulture); deps["attr"][loc] = GetOr(job["attr"].AsObject(), loc, "float"); }
        if (stage == "ps")
            foreach (long c in colors) { string k = c.ToString(CultureInfo.InvariantCulture); deps["fout"][k] = GetOr(fout, k, "float"); }
        if (alpha != null) deps["alpha"] = alpha;   // a variant with an alpha test serves only that test (totk_shader_rt.m deps_match)
        var refl = new JsonObject
        {
            ["key"] = variant + "_b", ["route"] = "B", ["chain"] = chain, ["stage"] = stage,
            ["entry"] = entry.Success ? entry.Groups[2].Value : "main0", ["msl_entry_kind"] = entry.Success ? entry.Groups[1].Value : null,
            ["threadgroup_size"] = GetOr(rq, "local", null), ["c1"] = GetStr(rq, "c1", ""), ["notes"] = res["notes"].DeepClone(),
            ["drv_reads"] = dr, ["drv_unfillable"] = unfillable, ["bindings"] = Reflect(stage, src, res),
            ["vertex_inputs"] = vertexInputs, ["raster"] = new JsonObject(), ["deps"] = deps, ["softfloat64"] = softfloat64,
            ["compare_slots"] = new JsonArray(compareSlots.Select(s => (JsonNode)s).ToArray()),
        };
        string tmp = Path.Combine(outDir, variant + "_b.json.tmp");
        File.WriteAllText(tmp, PyJson(refl, 1, false));
        File.Move(tmp, Path.Combine(outDir, variant + "_b.json"), true);
    }

    // ---- JSON text as Python's json.dump writes it (indent >= 0: indented, < 0: one line; ascii: ensure_ascii) ----
    static string PyJson(JsonNode n, int indent, bool ascii) { var sb = new StringBuilder(); PyJson(sb, n, indent, ascii, 0); return sb.ToString(); }
    static void PyJson(StringBuilder sb, JsonNode n, int indent, bool ascii, int level)
    {
        switch (n)
        {
            case null: sb.Append("null"); return;
            case JsonObject o:
                if (o.Count == 0) { sb.Append("{}"); return; }
                sb.Append('{');
                int i = 0;
                foreach (var kv in o)
                {
                    if (i++ > 0) sb.Append(indent < 0 ? ", " : ",");
                    if (indent >= 0) sb.Append('\n').Append(' ', indent * (level + 1));
                    PyStr(sb, kv.Key, ascii); sb.Append(": "); PyJson(sb, kv.Value, indent, ascii, level + 1);
                }
                if (indent >= 0) sb.Append('\n').Append(' ', indent * level);
                sb.Append('}'); return;
            case JsonArray a:
                if (a.Count == 0) { sb.Append("[]"); return; }
                sb.Append('[');
                for (int j = 0; j < a.Count; j++)
                {
                    if (j > 0) sb.Append(indent < 0 ? ", " : ",");
                    if (indent >= 0) sb.Append('\n').Append(' ', indent * (level + 1));
                    PyJson(sb, a[j], indent, ascii, level + 1);
                }
                if (indent >= 0) sb.Append('\n').Append(' ', indent * level);
                sb.Append(']'); return;
            case JsonValue v:
                switch (v.GetValueKind())
                {
                    case JsonValueKind.String: PyStr(sb, v.GetValue<string>(), ascii); return;
                    case JsonValueKind.True: sb.Append("true"); return;
                    case JsonValueKind.False: sb.Append("false"); return;
                    case JsonValueKind.Number:
                        if (v.TryGetValue(out int iv)) sb.Append(iv.ToString(CultureInfo.InvariantCulture));
                        else if (v.TryGetValue(out long lv)) sb.Append(lv.ToString(CultureInfo.InvariantCulture));
                        else if (v.TryGetValue(out ulong uv)) sb.Append(uv.ToString(CultureInfo.InvariantCulture));
                        else sb.Append(v.ToJsonString());
                        return;
                    default: sb.Append("null"); return;
                }
        }
    }
    static void PyStr(StringBuilder sb, string s, bool ascii)
    {
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                default:
                    if (c < 0x20 || (ascii && c > 0x7e)) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }

    // totk_xlate --service --spirv-cross <path> --out <dir> --chain <name> [--debug-mode 0|1]
    public static int Run(string[] args)
    {
        string dm = Environment.GetEnvironmentVariable("TOTK_SHADER_DEBUGMODE");
        debugMode = string.IsNullOrEmpty(dm) ? 1 : (int)ParseInt(dm.Trim());
        for (int i = 1; i < args.Length; i++)
        {
            string v = i + 1 < args.Length ? args[i + 1] : null;
            switch (args[i])
            {
                case "--spirv-cross": spirvCross = v; i++; break;
                case "--out": outDir = v; i++; break;
                case "--chain": chain = v; i++; break;
                case "--debug-mode": debugMode = (int)ParseInt(v); i++; break;
                default: Console.Error.WriteLine($"totk_xlate --service: unknown argument {args[i]}"); return 2;
            }
        }
        if (spirvCross == null || outDir == null || chain == null)
        {
            Console.Error.WriteLine("usage: totk_xlate --service --spirv-cross <path> --out <dir> --chain <name> [--debug-mode 0|1]");
            return 2;
        }
        Directory.CreateDirectory(outDir);
        var stdout = Console.Out;
        string line;
        while ((line = Console.In.ReadLine()) != null)
        {
            if (line.Trim().Length == 0) continue;
            string reply;
            try
            {
                Handle(JsonNode.Parse(line).AsObject());
                reply = "{\"ok\": true}";
            }
            catch (Exception e)   // reported to the host, which stops with the message
            {
                string kind = e is RuntimeError ? "RuntimeError" : e.GetType().Name;
                reply = PyJson(new JsonObject { ["ok"] = false, ["error"] = Cut($"{kind}: {e.Message}", 500) }, -1, true);
            }
            stdout.Write(reply + "\n");
            stdout.Flush();
        }
        return 0;
    }
}

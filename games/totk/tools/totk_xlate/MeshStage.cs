// Copyright (c) 2026 seedds. SPDX-License-Identifier: MIT
// C# adaptation of nxnative PR #983 (b124b24), dmsh_meshstage.py / dmsh_build.py,
// as vendored by games/mkw/tools/{dmsh_meshstage,prepare_ampere_jobs}.py. See LICENSE.
// Stage memory is zeroed before the body; patch 0008 omits racing zero-count stores.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static partial class ShaderService
{
    static JsonArray MeshComponents(byte[] sph) {
        var a = new JsonArray();
        for (int c = 0; c < 4; c++) if ((sph[0x35] & (1 << (4 + c))) != 0) a.Add((JsonNode)new JsonArray(null, JsonValue.Create(c)));
        for (int n = 0; n < 32; n++) for (int c = 0; c < 4; c++)
            if (((sph[0x36 + n / 2] >> (4 * (n % 2))) & (1 << c)) != 0) a.Add((JsonNode)new JsonArray(n, c));
        return a;
    }

    // Native request code is a path to the WHOLE code area, just like MKW's host request.
    static JsonObject PrepareMesh(JsonObject request) {
        var rq = request.DeepClone().AsObject();
        byte[] ctl = Convert.FromHexString(GetStr(rq, "mesh_control", ""));
        string kind = (string)rq["stage"];
        if (GetStr(rq, "arch", "") != "sm86") throw new RuntimeError("task/mesh requires arch sm86");
        if (ctl.Length < 0xc0 || BitConverter.ToUInt32(ctl, 0) != 0x98761234 || BitConverter.ToUInt32(ctl, 4) != 0xc9)
            throw new RuntimeError("not a complete NVN2 mesh control section");
        if (ctl[0x56] != (kind == "ts" ? 6 : 7)) throw new RuntimeError("request and control mesh stage differ");
        byte[] area = File.ReadAllBytes((string)rq["code"]);
        uint inst = BitConverter.ToUInt32(ctl, 0x38), c1 = BitConverter.ToUInt32(ctl, 0x40), end = BitConverter.ToUInt32(ctl, 0x44);
        if (inst < 0x90 || inst % 16 != 0 || inst > c1 || c1 > end || end != area.Length)
            throw new RuntimeError("invalid mesh instruction/constant segment lengths");
        int threads = BitConverter.ToUInt16(ctl, 0x58), payload = BitConverter.ToUInt16(ctl, 0x5a);
        if (threads < 1 || threads > 1024) throw new RuntimeError("invalid mesh workgroup size");
        for (int at = 0x80; at < inst; at += 16)
            if ((BitConverter.ToUInt16(area, at) & 0xfff) is 0x984 or 0x388 or 0x988 or 0x38c or 0x98c or 0xf8c or 0x38d)
                throw new RuntimeError("task/mesh shader with its own shared memory is unsupported");
        var info = new JsonObject { ["kind"] = kind, ["threads"] = threads, ["payload"] = payload };
        int output = Math.Max(0x900, 0x40 + ((payload + 15) & ~15)), prim, words;
        if (kind == "ts") { prim = output + payload; words = (prim + 15) / 4 + 4; }
        else {
            int mv = BitConverter.ToUInt16(ctl, 0x60), mp = BitConverter.ToUInt16(ctl, 0x62);
            if (mv < 1 || mv > 256 || mp < 1 || mp > 256) throw new RuntimeError("mesh output counts exceed supported Metal limits");
            JsonArray comps = MeshComponents(area), pcomps = new();
            if ((ctl[0x68] & 0x80) != 0) {
                uint off = BitConverter.ToUInt32(ctl, 0x64);
                if (off == 0 || (ulong)off + 0x80 > (ulong)area.Length) throw new RuntimeError("invalid per-primitive SPH offset");
                pcomps = new JsonArray(MeshComponents(area.AsSpan((int)off, 0x80).ToArray()).Where(c => c[0] != null).Select(c => c.DeepClone()).ToArray());
            }
            info["max_v"] = mv; info["max_p"] = mp; info["comps"] = comps; info["pcomps"] = pcomps; info["task"] = ctl[0x5e] != 0;
            int blocks = Math.Max(Math.Max((mv + 31) / 32, (mp + 31) / 32), 1);
            prim = output + blocks * Math.Max(comps.Count + pcomps.Count, 1) * 0x80;
            words = (prim + 4 + 3 * Math.Max(mp, 1) + 15) / 4 + 4;
        }
        if (words * 4 > 32768) throw new RuntimeError("mesh stage memory exceeds Metal threadgroup limit");
        info["mesh_layout"] = new JsonArray(0, 0x40, output, prim);
        string code = Path.Combine(outDir, (string)rq["variant"] + ".stage.code");
        File.WriteAllBytes(code, area.AsSpan(0x80, (int)inst - 0x80).ToArray());
        rq["code"] = code; rq["c1"] = Convert.ToHexString(area.AsSpan((int)c1, (int)(end - c1))).ToLowerInvariant();
        rq["stage"] = "cs"; rq["local"] = new JsonArray(threads, 1, 1); rq["shared"] = words * 4;
        rq["mesh_stage"] = info; rq["mesh_layout"] = info["mesh_layout"].DeepClone();
        return rq;
    }

    static List<string> MeshParams(string s) {
        var result = new List<string>(); int start = 0, depth = 0;
        for (int i = 0; i < s.Length; i++) {
            char c = s[i];
            if (c == ',' && depth == 0) { result.Add(s[start..i].Trim()); start = i + 1; }
            if (c is '(' or '[' or '<') depth++; if (c is ')' or ']' or '>') depth--;
        }
        if (s[start..].Trim().Length > 0) result.Add(s[start..].Trim());
        return result;
    }

    static string WrapMesh(string msl, JsonObject info) {
        string kind = (string)info["kind"];
        // the thread variables spirv-cross declares ahead of the stage memory (GlobalToStorage's last raw entry, uint _N = 0u;) stay in the body: one call a thread, as the kernel ran (nxnative dmsh_meshstage.py, MIT)
        Match m = Regex.Match(msl, @"kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{\s*((?:(?!threadgroup\b)[^\n{};]+;\r?\n\s*)*)threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\r?\n");
        if (!m.Success) throw new RuntimeError(kind + ": no kernel main0 with its threadgroup array");
        List<string> pars = MeshParams(m.Groups[1].Value);
        string local = m.Groups[2].Value.Trim(), words = m.Groups[3].Value, arr = m.Groups[4].Value;
        var plain = pars.Select(p => Regex.Replace(p, @"\s*\[\[.*\]\]$", "")).ToList();
        var names = pars.Select(p => Regex.Match(p, @".*?(\w+)\s*(\[\[.*\]\])?$").Groups[1].Value).ToList();
        string body = $"static inline __attribute__((always_inline)) void dm_stage_body({string.Join(", ", plain.Append($"threadgroup spvUnsafeArray<uint, {words}>& {arr}"))})\n{{\n" + (local.Length > 0 ? "    " + local + "\n" : "");
        string src = msl[..m.Index] + body + msl[(m.Index + m.Length)..];
        string call = $"dm_stage_body({string.Join(", ", names.Append(arr))});";
        int thr = (int)info["threads"], payw = ((int)info["payload"] + 3) / 4;
        JsonArray lay = info["mesh_layout"].AsArray();
        int sys = (int)lay[0] / 4, input = (int)lay[1] / 4, output = (int)lay[2] / 4, prim = (int)lay[3];
        var own = new Dictionary<string, string>();
        foreach (string p in pars) {
            Match a = Regex.Match(p, @"^(uint3?)\s+(\w+)\s*\[\[(thread_position_in_threadgroup|threadgroup_position_in_grid)\]\]$");
            if (a.Success) own[a.Groups[3].Value] = a.Groups[2].Value + (a.Groups[1].Value == "uint3" ? ".x" : "");
        }
        var extra = new List<string>(); var alias = new StringBuilder();
        foreach (var (attr, name) in new[] { ("threadgroup_position_in_grid", "dm_tg"), ("thread_position_in_threadgroup", "dm_lid") })
            if (own.TryGetValue(attr, out string expr)) alias.Append($"    uint {name} = {expr};\n"); else extra.Add($"uint {name} [[{attr}]]");
        string start = alias + $"    threadgroup spvUnsafeArray<uint, {words}> {arr};\n    for (uint i = dm_lid; i < {words}u; i += {thr}u) {arr}[i] = 0u;\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        string barrier = "    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        string head = $"struct dm_payload_t {{ uint w[{Math.Max(payw, 1)}]; }};\n";
        if (kind == "ts") {
            var args = pars.Concat(new[] { "object_data dm_payload_t& dm_pl [[payload]]", "mesh_grid_properties dm_mgp" }).Concat(extra).Append("constant uint* dm_first [[buffer(29)]]");
            return src + "\n" + head + $"[[object]] void main0({string.Join(", ", args)})\n{{\n" + start +
                $"    if (dm_lid == 0u) {arr}[{sys}] = dm_first[0] + dm_tg;\n" + barrier + $"    {call}\n" + barrier +
                $"    for (uint i = dm_lid; i < {payw}u; i += {thr}u) dm_pl.w[i] = {arr}[{output}u + i];\n    if (dm_lid == 0u) dm_mgp.set_threadgroups_per_grid(uint3(min({arr}[{output + 1}], 1024u), 1u, 1u));\n}}\n";
        }
        var comps = info["comps"].AsArray().Select(c => (n: c[0] == null ? (int?)null : (int)c[0], c: (int)c[1])).ToList();
        var pcomps = info["pcomps"].AsArray().Select(c => (n: (int?)c[0], c: (int)c[1])).ToList();
        var gens = comps.Where(c => c.n != null).Select(c => c.n.Value).Distinct().OrderBy(n => n).ToList();
        var pgens = pcomps.Select(c => c.n.Value).Distinct().OrderBy(n => n).ToList();
        int mv = (int)info["max_v"], mp = (int)info["max_p"], stride = Math.Max(comps.Count + pcomps.Count, 1) * 32;
        bool task = (bool)info["task"];
        string Lanes(List<(int? n, int c)> cs, int? n, int off) => string.Join(", ", Enumerable.Range(0, 4).Select(c => cs.Contains((n, c)) ? $"as_type<float>({arr}[b + {(off + cs.IndexOf((n, c))) * 32}u])" : c == 3 ? "1.0" : "0.0"));
        string sets = string.Concat(new int?[] { null }.Concat(gens.Select(n => (int?)n)).Select(n => $"        o.{(n == null ? "position" : "a" + n)} = float4({Lanes(comps, n, 0)});\n"));
        string psets = string.Concat(pgens.Select(n => $"        q.p{n} = float4({Lanes(pcomps, n, comps.Count)});\n"));
        head += "struct dm_vertex_t {\n    float4 position [[position]];\n" + string.Concat(gens.Select(n => $"    float4 a{n} [[user(locn{n})]];\n")) + "};\n";
        if (pgens.Count > 0) head += "struct dm_prim_t {\n" + string.Concat(pgens.Select(n => $"    float4 p{n} [[user(locn{n})]];\n")) + "};\n";
        head += $"using dm_mesh_t = metal::mesh<dm_vertex_t, {(pgens.Count > 0 ? "dm_prim_t" : "void")}, {mv}, {mp}, metal::topology::triangle>;\n";
        string Entry(string name, bool gl) {
            var args = pars.Append("dm_mesh_t dm_out").ToList();
            if (task) args.Add("const object_data dm_payload_t& dm_pl [[payload]]");
            args.AddRange(extra); args.Add("constant uint* dm_first [[buffer(29)]]");
            return $"[[mesh]] void {name}({string.Join(", ", args)})\n{{\n" + start +
                (task ? $"    for (uint i = dm_lid; i < {payw}u; i += {thr}u) {arr}[{input}u + i] = dm_pl.w[i];\n" + barrier + $"    if (dm_lid == 0u) {{ {arr}[{sys}] = dm_tg; {arr}[{input}] = dm_tg; }}\n" : $"    if (dm_lid == 0u) {arr}[{sys}] = dm_first[0] + dm_tg;\n") + barrier + $"    {call}\n" + barrier +
                $"    uint np = min(({arr}[{prim / 4}] >> 24) & 0xffu, {mp}u);\n    if (dm_lid == 0u) dm_out.set_primitive_count(np);\n    for (uint v = dm_lid; v < {mv}u; v += {thr}u)\n    {{\n        uint b = {output}u + (v / 32u) * {stride}u + (v % 32u);\n        dm_vertex_t o;\n" + sets +
                (gl ? "        o.position.z = fma(o.position.z, 0.5, o.position.w * 0.5);\n" : "") + "        dm_out.set_vertex(v, o);\n    }\n" +
                (pgens.Count > 0 ? $"    for (uint p = dm_lid; p < np; p += {thr}u)\n    {{\n        uint b = {output}u + (p / 32u) * {stride}u + (p % 32u);\n        dm_prim_t q;\n" + psets + "        dm_out.set_primitive(p, q);\n    }\n" : "") +
                $"    for (uint i = dm_lid; i < np * 3u; i += {thr}u) {{ uint a = {prim + 4}u + i; dm_out.set_index(i, ({arr}[a / 4u] >> ((a % 4u) * 8u)) & 0xffu); }}\n}}\n";
        }
        return src + "\n" + head + Entry("main0", true) + "\n" + Entry("main0_z", false);
    }
}

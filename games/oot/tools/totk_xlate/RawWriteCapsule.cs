using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

static class RawWriteCapsule
{
    [DllImport("oot_spirv_tools", EntryPoint = "oot_spirv_ssa")]
    static extern unsafe int Normalize(uint* words, nuint count, out IntPtr result, out nuint bytes);
    [DllImport("oot_spirv_tools", EntryPoint = "oot_spirv_ssa_free")]
    static extern void Free(IntPtr p);

    public static unsafe JsonObject Create(string path, string source, JsonObject reflection)
    {
        byte[] input = File.ReadAllBytes(path);
        // This is only a quick absence check. The analyzer separately accounts
        // for every physical store/atomic and rejects an unattested writer.
        if (input.Length > 2_097_152 || !Encoding.UTF8.GetString(input).Contains("dmsh_raw_write_", StringComparison.Ordinal)) return null;
        IntPtr result = IntPtr.Zero;
        try
        {
            nuint count;
            fixed (byte* p = input) if (Normalize((uint*)p, (nuint)(input.Length / 4), out result, out count) != 1) return null;
            if (result == IntPtr.Zero || count == 0 || count > 2_097_152) return null;
            byte[] normalized = new byte[(int)count]; Marshal.Copy(result, normalized, 0, normalized.Length);
            var bindings = new JsonArray();
            foreach (JsonObject b in reflection["bindings"].AsArray())
            {
                string kind = (string)b["kind"];
                if (kind is "cbuf" or "c1") bindings.Add((JsonNode)new JsonObject { ["name"] = (string)b["msl_name"], ["arg_id"] = (long)b["arg_id"], ["reg"] = kind == "c1" ? -1 : (long)b["reg"] });
            }
            return new JsonObject { ["abi"] = 1, ["normalizer"] = "spirv-tools-ef96ed763b43-ssa-rewrite",
                ["spirv"] = Convert.ToBase64String(normalized), ["msl_sha256"] = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant(),
                ["c1"] = (string)reflection["c1"], ["bindings"] = bindings };
        }
        catch (DllNotFoundException) { return null; }
        catch (EntryPointNotFoundException) { return null; }
        finally { if (result != IntPtr.Zero) Free(result); }
    }
}

using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Text.Json.Nodes;

// Bridge to the vendored nxnative SM86 frontend (third_party/nxnative). Keep diagnostics off the protocol's stdout.
static class SwshXlate
{
    static int Main(string[] args) => ShaderService.Run(args);
    public static string Translate(JsonObject job, string output)
    {
        string executable = ShaderService.Xlate;   // build/dmsh_xlate/dmsh_xlate (third_party/nxnative)
        if ((string)job["arch"] != "sm86") return "Ampere service requires arch sm86";
        if ((string)job["stage"] == "cs" && job["nvn2_cs"] is JsonArray layout && layout.Count == 5)
        {
            byte[] code = File.ReadAllBytes((string)job["code"]); int reductions = 0;
            int images = 0x10 + (((int)layout[4] + 15) & ~15) + 16 * ((int)layout[0] + (int)layout[1]) + 8 * (int)layout[2];
            for (int at = 0; at + 16 <= code.Length; at += 16)
            {
                ulong lo = BinaryPrimitives.ReadUInt64LittleEndian(code.AsSpan(at, 8));
                ulong hi = BinaryPrimitives.ReadUInt64LittleEndian(code.AsSpan(at + 8, 8));
                if ((lo & 0xfff) != 0x59f) continue;
                int descriptor = (int)((lo >> 40) & 0x3fff) * 4;
                // The encountered direct, 2D, ADD.U32, STRONG.GPU form. Other modes stay unsupported.
                if ((uint)hi != 0x0010e000 || ((lo >> 61) & 7) != 3 || ((lo >> 54) & 31) != 0 ||
                    ((lo >> 59) & 3) != 0 || ((lo >> 24) & 255) > 253 ||
                    descriptor < images || descriptor + 8 > (int)layout[3] || (descriptor - images) % 8 != 0) continue;
                // SUATOM.ADD.U32 with RZ destination and PT predicate-result selector uses the frontend's
                // AtomicDiscardResult path. Descriptor, coordinates, data, predicate and scheduling are retained.
                lo = (lo & ~0xff0ffful) | 0xff0000ul | 0x593ul;
                hi = (hi & ~(7ul << 17)) | (7ul << 17);
                BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan(at, 8), lo);
                BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan(at + 8, 8), hi);
                reductions++;
            }
            if (reductions != 0)
            {
                string lowered = Path.Combine(output, (string)job["key"] + ".sured.code");
                File.WriteAllBytes(lowered, code); job["code"] = lowered;
            }
        }
        if ((string)job["stage"] == "vs")
        {
            // Mesa NAK b5b73e59: attribute 0x5c is VPRS_TABLE_INDEX (per-primitive shading rate).
            // The Metal path shades at full rate. Remove only scalar static stores to that output;
            // wider/physical/per-patch stores retain the frontend's unsupported-attribute failure.
            byte[] code = File.ReadAllBytes((string)job["code"]); int removed = 0;
            for (int at = 0x80; at + 16 <= code.Length; at += 16)
            {
                ulong lo = BinaryPrimitives.ReadUInt64LittleEndian(code.AsSpan(at, 8));
                ulong hi = BinaryPrimitives.ReadUInt64LittleEndian(code.AsSpan(at + 8, 8));
                if ((lo & 0xfff) != 0x322 || ((lo >> 40) & 0x3ff) != 0x5c || ((lo >> 24) & 255) != 255 || ((hi >> 10) & 15) != 0) continue;
                // NOP retains the predicate and scheduling word, so addresses and branch targets stay fixed.
                BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan(at, 8), 0x918ul | (lo & 0xf000));
                BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan(at + 8, 8), hi & 0xffffffff00000000ul);
                removed++;
            }
            if (removed != 0)
            {
                string lowered = Path.Combine(output, (string)job["key"] + ".fullrate.code");
                File.WriteAllBytes(lowered, code); job["code"] = lowered;
            }
        }
        string path = Path.Combine(output, (string)job["key"] + ".job.json");
        File.WriteAllText(path, new JsonArray(job.DeepClone()).ToJsonString());
        var start = new ProcessStartInfo(executable) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        start.ArgumentList.Add(path); start.ArgumentList.Add(output);
        using var process = Process.Start(start);
        var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        if (process.ExitCode != 0) return stderr.Result + stdout.Result;
        return null;
    }
}

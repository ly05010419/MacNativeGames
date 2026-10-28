using System;
using System.Collections.Generic;
using System.Security.Cryptography;

// TOTK 1.1.2 / 1.4.3 visibility counter: each invocation passing its input test
// participates in a ballot; the highest active lane adds popcount(ballot) to s1[0].
// The atomic's return value is unused and no other invocation observes it here.
// Singleton groups therefore preserve the final modulo-2^32 sum exactly. Metal's
// vertex stage cannot expose the subgroup builtins through our SPIRV-Cross path.
// This is deliberately restricted to the analysed Maxwell bytes, NOT a general
// subgroup fallback (shuffle, vote-dependent output, and atomic allocation differ).
static class VertexCounterLowering
{
    const string VertexCodeHash = "EABE9FDD6FC737B8414E94CC3E14EC0031AB3A156511953ABE5E3A04128524AD";
    // 1.4.3 reads s0[114 + (instance_id - base_instance)*4] instead of
    // 1.1.2's s0[8304 + vertex_id*4]. The guarded ballot/popcount/atomic tail
    // has the same reduction: one increment for every passing invocation.
    const string InstanceCodeHash = "F5F4D23149565ACD852F6B7091F64CC8CB3AA019CCB33BA3F88CED62C6D1FF7B";
    internal static byte[] Apply(byte[] code, string stage, byte[] spirv, List<string> notes)
    {
        if (stage != "vs") return spirv;
        string codeHash=Convert.ToHexString(SHA256.HashData(code));
        if(codeHash!=VertexCodeHash&&codeHash!=InstanceCodeHash)return spirv;
        uint[] w = new uint[spirv.Length / 4]; Buffer.BlockCopy(spirv, 0, w, 0, spirv.Length);
        uint lane = 0, uintType = 0, vectorType = 0, trueId = 0;
        int ballots = 0, loads = 0;
        for (int i = 5; i < w.Length; i += (int)(w[i] >> 16))
        {
            uint op = w[i] & 65535;
            if (op == 71 && w[i + 2] == 11 && w[i + 3] == 41) lane = w[i + 1]; // BuiltIn SubgroupLocalInvocationId
            if (op == 21 && w[i + 2] == 32 && w[i + 3] == 0) uintType = w[i + 1];
            if (op == 41) trueId = w[i + 2]; // OpConstantTrue
            if (op == 339) { vectorType = w[i + 1]; ballots++; if (w[i + 4] != trueId) throw new InvalidOperationException("counter ballot predicate changed"); }
        }
        if (lane == 0 || uintType == 0 || vectorType == 0 || ballots != 1)
            throw new InvalidOperationException("analysed vertex counter SPIR-V shape changed");
        uint zero = w[3]++, one = w[3]++, mask = w[3]++;
        var result = new List<uint>(w.Length + 16); result.AddRange(w.AsSpan(0, 5).ToArray());
        bool constants = false;
        for (int i = 5; i < w.Length; i += (int)(w[i] >> 16))
        {
            uint op = w[i] & 65535;
            if (op == 54 && !constants)
            {
                result.AddRange(new uint[] { (4u << 16) | 43, uintType, zero, 0,
                    (4u << 16) | 43, uintType, one, 1,
                    (7u << 16) | 44, vectorType, mask, one, zero, zero, zero });
                constants = true;
            }
            if (op == 61 && w[i + 3] == lane) // load lane -> 0
            { result.AddRange(new uint[] { (4u << 16) | 83, w[i + 1], w[i + 2], zero }); loads++; }
            else if (op == 339) // ballot(true) -> (1,0,0,0)
                result.AddRange(new uint[] { (4u << 16) | 83, w[i + 1], w[i + 2], mask });
            else result.AddRange(w.AsSpan(i, (int)(w[i] >> 16)).ToArray());
        }
        if (loads != 1 || !constants) throw new InvalidOperationException("counter lane use changed");
        notes.Add("verified vertex visibility counter: singleton reduction (one atomic per passing vertex)");
        byte[] output = new byte[result.Count * 4]; Buffer.BlockCopy(result.ToArray(), 0, output, 0, output.Length);
        return output;
    }
}

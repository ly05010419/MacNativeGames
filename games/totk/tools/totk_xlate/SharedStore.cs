// Ampere shared memory byte and half-word stores as one Metal store.
// From seedds/nxnative dm/gfx/tools/dmsh_build.py shared_store_msl (MIT), ffebfd59.
// The translator's helper is a compare-and-swap loop on the word, as SPIR-V's shared memory is an array of words. A mesh workgroup's
// threads write neighbouring index bytes, so they contend for each word and retry. Metal's threadgroup memory takes byte stores and
// each byte is its own memory location; two threads storing the same byte leave one of the two values, as the console does.
using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

static class ShaderSharedStore
{
    static readonly Regex Definition = new(@"\nvoid (dmsh_shared_store(8|16)_\d+)\(([^)]*)\)\n\{");
    const string Alias = "typedef ushort __attribute__((may_alias)) dm_ushort_alias;\n";   // a half-word that may alias the shared memory's words
    const string Namespace = "using namespace metal;\n";

    static List<string> SplitParameters(string text)
    {
        var parameters = new List<string>();
        var current = new StringBuilder(); int depth = 0;
        foreach (char c in text + ",")
        {
            if (c == '<') depth++; else if (c == '>') depth--;
            if (c == ',' && depth == 0) { parameters.Add(current.ToString().Trim()); current.Clear(); }
            else current.Append(c);
        }
        return parameters;
    }

    public static string Rewrite(string src)
    {
        if (!src.Contains("dmsh_shared_store", StringComparison.Ordinal)) return src;
        MatchCollection found = Definition.Matches(src);
        if (found.Count == 0) throw new InvalidOperationException("shared store helper: no definition of the expected form");
        var output = new StringBuilder(); int at = 0; bool half = false;
        foreach (Match m in found)
        {
            List<string> parameters = SplitParameters(m.Groups[3].Value);
            Match offset = parameters.Count == 3 ? Regex.Match(parameters[0], @"^thread const u?int& (\w+)$") : Match.Empty;
            Match value = parameters.Count == 3 ? Regex.Match(parameters[1], @"^thread const u?int& (\w+)$") : Match.Empty;
            Match array = parameters.Count == 3 ? Regex.Match(parameters[2], @"^(threadgroup|device) spvUnsafeArray<uint, \d+>& (\w+)$") : Match.Empty;
            if (!offset.Success || !value.Success || !array.Success) throw new InvalidOperationException("shared store helper: unexpected parameters " + m.Groups[3].Value);
            string space = array.Groups[1].Value, memory = array.Groups[2].Value, off = offset.Groups[1].Value, val = value.Groups[1].Value;
            string store = m.Groups[2].Value == "8"
                ? $"(({space} uchar*)&{memory}[0])[uint({off})] = uchar(uint({val}));"
                : $"(({space} dm_ushort_alias*)&{memory}[0])[uint({off}) >> 1u] = ushort(uint({val}));";   // a half-word store is aligned
            half |= m.Groups[2].Value == "16";
            int end = m.Index + m.Length - 1, depth = 0;
            for (; end < src.Length; end++) { if (src[end] == '{') depth++; else if (src[end] == '}' && --depth == 0) break; }
            if (end == src.Length) throw new InvalidOperationException("shared store helper body is not closed");
            output.Append(src, at, m.Index + m.Length - at).Append("\n    ").Append(store).Append('\n');
            at = end;
        }
        output.Append(src, at, src.Length - at);
        string result = output.ToString();
        if (half)
        {
            int u = result.IndexOf(Namespace, StringComparison.Ordinal);
            if (u < 0) throw new InvalidOperationException("shared store helper: no using namespace metal");
            result = result.Insert(u + Namespace.Length, Alias);
        }
        return result;
    }
}

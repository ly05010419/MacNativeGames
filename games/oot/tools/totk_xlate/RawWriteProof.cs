// Conservative address analysis for the translator's raw global-write ABI.
// This interprets shader value ranges, never guest CPU code. Unsupported types,
// mutable inputs, missing writers and budget exhaustion reject the proof.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using static Spv.Specification;

static class RawWriteProof
{
    const ulong Mask = uint.MaxValue;
    const long Mod = 1L << 32;
    sealed record Range(ulong Lo, ulong Hi);
    sealed record Value(Range[] R = null, Value[] P = null);
    static readonly Value Full = new(new[] { new Range(0, Mask) });
    static readonly Value Bool = new(new[] { new Range(0, 1) });
    static readonly Value Unsupported = new();
    static Value Const(ulong n) => new(new[] { new Range(n & Mask, n & Mask) });
    static Value Normal(IEnumerable<Range> input)
    {
        var ranges = new List<Range>();
        foreach (Range r in input.Where(r => r.Lo <= r.Hi).OrderBy(r => r.Lo))
        {
            if (r.Hi > Mask) return Full;
            if (ranges.Count != 0 && r.Lo <= ranges[^1].Hi + 1)
                ranges[^1] = new(ranges[^1].Lo, Math.Max(ranges[^1].Hi, r.Hi));
            else ranges.Add(r);
        }
        return ranges.Count > 64 ? Full : new(ranges.ToArray());
    }
    static Value Union(IEnumerable<Value> input)
    {
        Value[] ds = input.ToArray();
        if (ds.Length == 0) return Unsupported;
        if (ds.Any(d => d == null || d.R == null && d.P == null)) return Unsupported;
        if (ds[0].P != null)
        {
            int n = ds[0].P.Length;
            if (ds.Any(d => d.P == null || d.P.Length != n)) return Unsupported;
            return new(P: Enumerable.Range(0, n).Select(i => Union(ds.Select(d => d.P[i]))).ToArray());
        }
        return ds.Any(d => d.R == null) ? Unsupported : Normal(ds.SelectMany(d => d.R));
    }
    static ulong[] Values(Value d, int limit = 64)
    {
        if (d?.R == null || d.R.Sum(r => (double)(r.Hi - r.Lo + 1)) > limit) return null;
        return d.R.SelectMany(r => Enumerable.Range(0, checked((int)(r.Hi - r.Lo + 1))).Select(i => r.Lo + (ulong)i)).ToArray();
    }
    static bool Contains(Value d, ulong n) => d.R == null || d.R.Any(r => r.Lo <= n && n <= r.Hi);
    static Value Intersect(Value a, Value b) => a.R == null || b.R == null ? Unsupported :
        Normal(from x in a.R from y in b.R where Math.Max(x.Lo, y.Lo) <= Math.Min(x.Hi, y.Hi) select new Range(Math.Max(x.Lo, y.Lo), Math.Min(x.Hi, y.Hi)));
    static IEnumerable<Range> Modulo(Int128 lo, Int128 hi)
    {
        if (hi - lo >= Mod) return Full.R;
        Int128 Q(Int128 n) => n / Mod - (n < 0 && n % Mod != 0 ? 1 : 0);
        ulong A = (ulong)(lo & Mask), B = (ulong)(hi & Mask);
        return Q(lo) == Q(hi) ? new[] { new Range(A, B) } : new[] { new Range(0, B), new Range(A, Mask) };
    }
    static IEnumerable<(long Lo, long Hi)> Signed(Value d)
    {
        foreach (Range r in d.R)
        {
            if (r.Lo < 1UL << 31) yield return ((long)r.Lo, (long)Math.Min(r.Hi, (1UL << 31) - 1));
            if (r.Hi >= 1UL << 31) yield return ((long)Math.Max(r.Lo, 1UL << 31) - Mod, (long)r.Hi - Mod);
        }
    }
    static Value Binary(Op op, Value a, Value b)
    {
        if (a.R == null || b.R == null) return Unsupported;
        ulong[] av = Values(a), bv = Values(b);
        bool shift = op is Op.OpShiftLeftLogical or Op.OpShiftRightLogical or Op.OpShiftRightArithmetic;
        if (shift && b.R.Any(r => r.Hi >= 32)) return Full; // undefined shifts cannot authorize pruning
        if (av != null && bv != null && av.Length * bv.Length <= 64)
        {
            ulong Eval(ulong x, ulong y) => op switch
            {
                Op.OpIAdd => x + y, Op.OpISub => unchecked(x - y), Op.OpIMul => x * y,
                Op.OpBitwiseAnd => x & y, Op.OpBitwiseOr => x | y, Op.OpBitwiseXor => x ^ y,
                Op.OpShiftLeftLogical => x << (int)y, Op.OpShiftRightLogical => x >> (int)y,
                Op.OpShiftRightArithmetic => unchecked((ulong)(unchecked((int)x) >> (int)y)), _ => throw new InvalidOperationException("binary operation")
            };
            return Normal(from x in av from y in bv let z = Eval(x, y) & Mask select new Range(z, z));
        }
        var result = new List<Range>();
        foreach (Range x in a.R) foreach (Range y in b.R)
        {
            switch (op)
            {
                case Op.OpIAdd: result.AddRange(Modulo((Int128)x.Lo + y.Lo, (Int128)x.Hi + y.Hi)); break;
                case Op.OpISub: result.AddRange(Modulo((Int128)x.Lo - y.Hi, (Int128)x.Hi - y.Lo)); break;
                case Op.OpIMul: result.AddRange(Modulo((Int128)x.Lo * y.Lo, (Int128)x.Hi * y.Hi)); break;
                case Op.OpBitwiseAnd: result.Add(new(0, Math.Min(x.Hi, y.Hi))); break;
                case Op.OpBitwiseOr: case Op.OpBitwiseXor:
                    ulong bits = Math.Max(x.Hi, y.Hi), top = 0;
                    while (bits != 0) { top = (top << 1) | 1; bits >>= 1; }
                    result.Add(new(0, top)); break;
                case Op.OpShiftLeftLogical: case Op.OpShiftRightLogical: case Op.OpShiftRightArithmetic:
                    for (ulong s = y.Lo; s <= y.Hi; s++)
                        if (op == Op.OpShiftLeftLogical) result.AddRange(Modulo((Int128)x.Lo << (int)s, (Int128)x.Hi << (int)s));
                        else if (op == Op.OpShiftRightLogical) result.Add(new(x.Lo >> (int)s, x.Hi >> (int)s));
                        else foreach (var v in Signed(new(new[] { x }))) result.AddRange(Modulo(v.Lo >> (int)s, v.Hi >> (int)s));
                    break;
                default: return Full;
            }
        }
        return Normal(result);
    }
    sealed record Inst(uint Id, uint Type, Op Op, uint[] A, int Index, uint Fn, uint Block);
    sealed class Function
    {
        public List<uint> Params = new();
        public Dictionary<uint, List<Inst>> Blocks = new();
        public Dictionary<uint, uint[]> Succ = new();
        public HashSet<uint> Reach = new();
        public Dictionary<uint, HashSet<uint>> Dom = new();
        public uint First;
    }
    sealed class Analyzer
    {
        readonly List<Inst> lines = new();
        readonly Dictionary<uint, Inst> defs = new();
        readonly Dictionary<uint, Function> functions = new();
        readonly Dictionary<uint, string> names = new();
        readonly Dictionary<(uint, uint), uint[]> decs = new();
        readonly Dictionary<(uint, uint, uint), uint> members = new();
        readonly Dictionary<uint, uint> constants = new();
        readonly Dictionary<uint, List<(uint Value, Inst At)>> stores = new();
        readonly HashSet<uint> bad = new(), active = new(), cyclic = new();
        readonly Dictionary<uint, Value> memo = new(), constraints = new();
        readonly Dictionary<uint, uint> facts = new();
        readonly Dictionary<uint, byte[]> buffers = new();
        readonly Dictionary<uint, int> regs = new(), aids = new();
        readonly Dictionary<(uint, uint), uint> reads = new();
        readonly uint[] grid, local;
        readonly uint simd;
        readonly Dictionary<uint, Dictionary<uint, uint[]>> baseCfg = new();
        readonly Dictionary<uint, HashSet<uint>> baseReach = new();
        readonly Dictionary<uint, Dictionary<uint, HashSet<uint>>> baseDom = new();
        uint entry;
        int budget = 2_000_000;
        void Tick(int n = 1) { if ((budget -= n) < 0) throw new InvalidOperationException("analysis budget exhausted"); }
        Inst Def(uint id) => defs.TryGetValue(id, out Inst x) ? x : throw new InvalidOperationException("undefined SPIR-V id");
        static uint[] Triplet(JsonNode n)
        {
            if (n is not JsonArray a || a.Count != 3) throw new InvalidOperationException("dispatch shape missing");
            uint[] r = a.Select(v => v.GetValue<uint>()).ToArray();
            if (r.Any(v => v == 0 || v > 1_048_576)) throw new InvalidOperationException("dispatch shape unsupported");
            return r;
        }
        public Analyzer(JsonObject request)
        {
            if ((int?)request["abi"] != 1) throw new InvalidOperationException("raw write ABI missing");
            grid = Triplet(request["grid"]); local = Triplet(request["local"]);
            simd = (uint?)request["simd_width"] ?? 0;
            if (simd != 32 || local.Aggregate(1UL, (a, b) => a * b) > 1024) throw new InvalidOperationException("execution width unsupported");
            byte[] bytes = Convert.FromBase64String((string)request["spirv"]);
            Parse(bytes);
            if (request["bindings"] is JsonArray bindings)
                foreach (JsonObject b in bindings)
                {
                    byte[] data = Convert.FromBase64String((string)b["data"]);
                    if (data.Length > 65536) throw new InvalidOperationException("uniform too large");
                    int reg = (int)b["reg"];
                    int aid = (int?)b["arg_id"] ?? 0;
                    if (aid < 0 || aid >= 64) throw new InvalidOperationException("uniform argument id unsupported");
                    if (reg < -1 || reg >= 16 || !(bool)b["immutable"]) throw new InvalidOperationException("uniform is not immutable");
                    foreach (uint id in names.Where(v => v.Value == (string)b["name"]).Select(v => v.Key))
                    { buffers.Add(id, data); regs.Add(id, reg); aids.Add(id, (int?)b["arg_id"] ?? 0); }
                }
            BuildCfg(); TrackStores();
        }
        void Parse(byte[] bytes)
        {
            if (bytes.Length < 20 || bytes.Length > 2_097_152 || bytes.Length % 4 != 0 || BitConverter.ToUInt32(bytes) != MagicNumber)
                throw new InvalidOperationException("invalid SPIR-V");
            uint bound = BitConverter.ToUInt32(bytes, 12), fn = 0, block = 0;
            if (bound > 500_000) throw new InvalidOperationException("SPIR-V id budget");
            for (int pos = 20; pos < bytes.Length;)
            {
                Tick(); uint header = BitConverter.ToUInt32(bytes, pos);
                int count = (int)(header >> 16); Op op = (Op)(header & 65535);
                if (count == 0 || (long)pos + count * 4 > bytes.Length) throw new InvalidOperationException("truncated SPIR-V");
                uint[] a = new uint[count - 1];
                for (int k = 0; k < a.Length; k++) a[k] = BitConverter.ToUInt32(bytes, pos + 4 + 4 * k);
                int layout = SpirvResultLayout.Get(op), skip = layout == 2 ? 2 : layout;
                if (a.Length < skip) throw new InvalidOperationException("SPIR-V result missing");
                uint id = layout == 0 ? 0 : a[skip - 1], type = layout == 2 ? a[0] : 0;
                if (layout != 0 && (id == 0 || id >= bound)) throw new InvalidOperationException("invalid SPIR-V result");
                a = a.Skip(skip).ToArray();
                if (op == Op.OpFunction) { fn = id; block = 0; functions.Add(fn, new()); }
                if (op == Op.OpLabel) { block = id; functions[fn].Blocks.Add(block, new()); if (functions[fn].First == 0) functions[fn].First = block; }
                var x = new Inst(id, type, op, a, lines.Count, fn, block); lines.Add(x);
                if (id != 0) defs.Add(id, x);
                if (block != 0 && op != Op.OpFunctionEnd) functions[fn].Blocks[block].Add(x);
                if (op == Op.OpFunctionParameter) functions[fn].Params.Add(id);
                if (op == Op.OpFunctionEnd) { fn = 0; block = 0; }
                if (op == Op.OpName)
                {
                    byte[] text = new byte[(a.Length - 1) * 4];
                    for (int k = 1; k < a.Length; k++) BitConverter.TryWriteBytes(text.AsSpan((k - 1) * 4, 4), a[k]);
                    int end = Array.IndexOf(text, (byte)0); if (end < 0) throw new InvalidOperationException("unterminated SPIR-V name");
                    names.Add(a[0], Encoding.UTF8.GetString(text, 0, end));
                }
                if (op == Op.OpDecorate) decs[(a[0], a[1])] = a.Skip(2).ToArray();
                if (op == Op.OpMemberDecorate && a.Length == 4) members[(a[0], a[1], a[2])] = a[3];
                if (op == Op.OpEntryPoint)
                {
                    if (entry != 0 || a[0] != (uint)ExecutionModel.GLCompute) throw new InvalidOperationException("only one compute entry supported");
                    entry = a[1];
                }
                if (op == Op.OpConstant && a.Length == 1 && Bits32(type)) constants[id] = a[0];
                if (op is Op.OpConstantTrue or Op.OpConstantFalse) constants[id] = op == Op.OpConstantTrue ? 1u : 0;
                pos += count * 4;
            }
            if (entry == 0) throw new InvalidOperationException("compute entry missing");
            // Specialization constants and alternate local sizes require a separate ABI.
            foreach (Inst x in lines)
                if (x.Op.ToString().StartsWith("OpSpecConstant", StringComparison.Ordinal) || x.Op == Op.OpExecutionModeId)
                    throw new InvalidOperationException("specialized execution unsupported");
            var localMode = lines.Where(x => x.Op == Op.OpExecutionMode && x.A[0] == entry && x.A[1] == (uint)ExecutionMode.LocalSize).ToArray();
            if (localMode.Length != 1 || !localMode[0].A.Skip(2).SequenceEqual(local)) throw new InvalidOperationException("local size differs from shader");
        }
        bool Bits32(uint t) => t != 0 && defs.TryGetValue(t, out Inst x) && (x.Op == Op.OpTypeBool || (x.Op is Op.OpTypeInt or Op.OpTypeFloat) && x.A[0] == 32);
        bool Int32(uint t) => t != 0 && defs.TryGetValue(t, out Inst x) && x.Op == Op.OpTypeInt && x.A[0] == 32;
        Value Shape(uint t, int depth = 0)
        {
            if (depth > 16 || t == 0) return Unsupported;
            Inst x = Def(t);
            if (x.Op is Op.OpTypePointer) return Shape(x.A[1], depth + 1);
            if (x.Op is Op.OpTypeVector or Op.OpTypeMatrix)
                return x.A[1] > 16 ? Unsupported : new(P: Enumerable.Range(0, (int)x.A[1]).Select(_ => Shape(x.A[0], depth + 1)).ToArray());
            if (x.Op == Op.OpTypeStruct)
                return x.A.Length > 16 ? Unsupported : new(P: x.A.Select(v => Shape(v, depth + 1)).ToArray());
            return x.Op == Op.OpTypeBool ? Bool : Bits32(t) ? Full : Unsupported;
        }
        void BuildCfg()
        {
            foreach (Function f in functions.Values)
            {
                foreach (var (b, ins) in f.Blocks)
                {
                    Inst x = ins[^1];
                    f.Succ[b] = x.Op switch
                    {
                        Op.OpBranch => new[] { x.A[0] }, Op.OpBranchConditional => x.A.Skip(1).Take(2).ToArray(),
                        Op.OpSwitch => new[] { x.A[1] }.Concat(x.A.Skip(3).Where((_, i) => i % 2 == 0)).ToArray(),
                        Op.OpReturn or Op.OpReturnValue or Op.OpUnreachable or Op.OpKill or Op.OpTerminateInvocation => Array.Empty<uint>(),
                        _ => throw new InvalidOperationException("unsupported block terminator")
                    };
                    if (f.Succ[b].Any(v => !f.Blocks.ContainsKey(v))) throw new InvalidOperationException("invalid branch target");
                }
                Recompute(f, true);
            }
        }
        void Recompute(Function f, bool dom)
        {
            f.Reach = new() { f.First }; var todo = new Stack<uint>(); todo.Push(f.First);
            while (todo.TryPop(out uint b)) foreach (uint s in f.Succ[b]) { Tick(); if (f.Reach.Add(s)) todo.Push(s); }
            if (!dom) return;
            f.Dom = f.Reach.ToDictionary(b => b, b => b == f.First ? new HashSet<uint> { b } : new HashSet<uint>(f.Reach));
            var pred = f.Reach.ToDictionary(b => b, b => f.Reach.Where(p => f.Succ[p].Contains(b)).ToArray());
            bool change;
            do
            {
                change = false;
                foreach (uint b in f.Reach.Where(b => b != f.First))
                {
                    Tick(); var next = new HashSet<uint>(f.Reach);
                    foreach (uint p in pred[b]) next.IntersectWith(f.Dom[p]);
                    next.Add(b);
                    if (!next.SetEquals(f.Dom[b])) { f.Dom[b] = next; change = true; }
                }
            } while (change);
        }
        uint Root(uint p)
        {
            var seen = new HashSet<uint>();
            while (seen.Add(p))
            {
                Tick(); Inst x = Def(p);
                if (x.Op is Op.OpAccessChain or Op.OpInBoundsAccessChain or Op.OpPtrAccessChain) p = x.A[0];
                else if (x.Op == Op.OpCopyObject) p = x.A[0];
                else return p;
            }
            throw new InvalidOperationException("cyclic pointer");
        }
        void TrackStores()
        {
            var mayWrite = functions.ToDictionary(v => v.Key, _ => new HashSet<uint>());
            foreach (Inst x in lines)
            {
                if (x.Op == Op.OpStore)
                {
                    uint p = x.A[0], root = Root(p);
                    if (p == root) { if (!stores.TryGetValue(p, out var ss)) stores[p] = ss = new(); ss.Add((x.A[1], x)); }
                    else if (Def(root).Op == Op.OpVariable && Def(root).A[0] == (uint)StorageClass.Function) bad.Add(root);
                    if (x.Fn != 0 && functions[x.Fn].Params.Contains(root)) mayWrite[x.Fn].Add(root);
                }
                if (x.Op is Op.OpCopyMemory or Op.OpCopyMemorySized) bad.Add(Root(x.A[0]));
                if (x.Op == Op.OpVariable && x.A.Length > 1) bad.Add(x.Id); // explicit initializers are not analyzed
            }
            bool change;
            do
            {
                change = false;
                foreach (Inst x in lines.Where(x => x.Op == Op.OpFunctionCall))
                {
                    Function f = functions[x.A[0]];
                    if (x.A.Length != f.Params.Count + 1) throw new InvalidOperationException("call arity");
                    for (int k = 0; k < f.Params.Count; k++) if (mayWrite[x.A[0]].Contains(f.Params[k]))
                    {
                        uint r = Root(x.A[k + 1]);
                        if (functions[x.Fn].Params.Contains(r)) change |= mayWrite[x.Fn].Add(r);
                        else if (Def(r).Op == Op.OpVariable) bad.Add(r);
                    }
                }
            } while (change);
        }
        (uint Base, List<uint> Path) Access(uint p)
        {
            var path = new List<uint>(); var seen = new HashSet<uint>();
            while (seen.Add(p))
            {
                Inst x = Def(p);
                if (x.Op is not (Op.OpAccessChain or Op.OpInBoundsAccessChain)) return (p, path);
                path.InsertRange(0, x.A.Skip(1)); p = x.A[0];
            }
            throw new InvalidOperationException("cyclic access chain");
        }
        Value SelectPart(Value d, IEnumerable<uint> path)
        {
            foreach (uint index in path)
            {
                ulong[] iv = Values(Eval(index));
                if (iv == null || iv.Length == 0 || d.P == null || iv.Any(i => i >= (ulong)d.P.Length)) return Unsupported;
                d = Union(iv.Select(i => d.P[i]));
            }
            return d;
        }
        Value LocalLoad(uint p, List<uint> path, Inst at)
        {
            Inst v = Def(p);
            if (bad.Contains(p) || !stores.TryGetValue(p, out var ss)) return Shape(v.Type);
            var reachable = ss.Where(s => functions[s.At.Fn].Reach.Contains(s.At.Block)).ToArray();
            if (!reachable.Any(s => s.At.Fn == at.Fn && functions[at.Fn].Dom.TryGetValue(at.Block, out var ds) && ds.Contains(s.At.Block) && (s.At.Block != at.Block || s.At.Index < at.Index))) return Shape(v.Type);
            return Union(reachable.Select(s => SelectPart(Eval(s.Value), path)));
        }
        Value UniformLoad(uint p, List<uint> path)
        {
            if (!buffers.TryGetValue(p, out byte[] data)) return Unsupported;
            Inst ptr = Def(Def(p).Type); uint t = ptr.A[1]; var offsets = new HashSet<ulong> { 0 };
            foreach (uint idx in path)
            {
                Inst v = Def(t); ulong[] iv = Values(Eval(idx));
                if (iv == null || iv.Length == 0) return Unsupported;
                if (v.Op == Op.OpTypeStruct)
                {
                    if (iv.Length != 1 || iv[0] >= (ulong)v.A.Length || !members.TryGetValue((t, (uint)iv[0], (uint)Decoration.Offset), out uint delta)) return Unsupported;
                    offsets = offsets.Select(o => o + delta).ToHashSet(); t = v.A[iv[0]];
                }
                else if (v.Op == Op.OpTypeArray)
                {
                    if (!decs.TryGetValue((t, (uint)Decoration.ArrayStride), out uint[] stride) || !constants.TryGetValue(v.A[1], out uint size) || iv.Any(i => i >= size)) return Unsupported;
                    offsets = (from o in offsets from i in iv select o + i * stride[0]).ToHashSet(); t = v.A[0];
                }
                else if (v.Op == Op.OpTypeVector && Bits32(v.A[0]))
                {
                    if (iv.Any(i => i >= v.A[1])) return Unsupported;
                    offsets = (from o in offsets from i in iv select o + i * 4).ToHashSet(); t = v.A[0];
                }
                else return Unsupported;
                if (offsets.Count > 256) return Unsupported;
            }
            if (!Bits32(t) || Def(t).Op == Op.OpTypeBool || offsets.Any(o => o > (ulong)data.Length || (ulong)data.Length - o < 4)) return Unsupported;
            var result = new List<Value>();
            foreach (ulong off in offsets)
            {
                uint word = BitConverter.ToUInt32(data, (int)off);
                reads[(p, (uint)off)] = word; result.Add(Const(word));
            }
            if (reads.Count > 256) throw new InvalidOperationException("uniform guard budget");
            return Union(result);
        }
        Value Eval(uint id)
        {
            Tick();
            if (facts.TryGetValue(id, out uint fact)) return Const(fact);
            if (constants.TryGetValue(id, out uint c)) return Const(c);
            if (memo.TryGetValue(id, out Value cached)) return cached;
            Inst x = Def(id);
            if (active.Count > 512 || !active.Add(id)) return Shape(x.Type);
            try
            {
                Value d = Evaluate(x);
                if (constraints.TryGetValue(id, out Value constraint)) d = Intersect(d, constraint);
                return memo[id] = d;
            }
            finally { active.Remove(id); }
        }
        Value Evaluate(Inst x)
        {
            uint[] a = x.A; Op op = x.Op;
            if (op == Op.OpLoad)
            {
                var (p, path) = Access(a[0]); Inst v = Def(p);
                if (v.Op == Op.OpVariable && v.A[0] == (uint)StorageClass.Function) return LocalLoad(p, path, x);
                if (decs.TryGetValue((p, (uint)Decoration.BuiltIn), out uint[] bi))
                {
                    Value d = (BuiltIn)bi[0] switch
                    {
                        BuiltIn.LocalInvocationId => new(P: local.Select(n => new Value(new[] { new Range(0, n - 1) })).ToArray()),
                        BuiltIn.WorkgroupId => new(P: grid.Select(n => new Value(new[] { new Range(0, n - 1) })).ToArray()),
                        BuiltIn.SubgroupLocalInvocationId => new(new[] { new Range(0, simd - 1) }),
                        BuiltIn.SubgroupLtMask => new(P: new[] { Full, Const(0), Const(0), Const(0) }),
                        _ => Shape(x.Type)
                    };
                    return SelectPart(d, path);
                }
                return v.Op == Op.OpVariable && v.A[0] == (uint)StorageClass.Uniform ? UniformLoad(p, path) : Shape(x.Type);
            }
            if (op == Op.OpVariable) return LocalLoad(x.Id, new(), x);
            if (op is Op.OpCopyObject or Op.OpBitcast or Op.OpUConvert or Op.OpSConvert)
                return Bits32(x.Type) && Bits32(Def(a[0]).Type) ? Eval(a[0]) : Shape(x.Type);
            if (op is Op.OpCompositeConstruct or Op.OpConstantComposite) return new(P: a.Select(Eval).ToArray());
            if (op == Op.OpCompositeExtract)
            {
                Value d = Eval(a[0]);
                foreach (uint i in a.Skip(1)) { if (d.P == null || i >= d.P.Length) return Shape(x.Type); d = d.P[i]; }
                return d;
            }
            if (op is Op.OpIAdd or Op.OpISub or Op.OpIMul or Op.OpBitwiseAnd or Op.OpBitwiseOr or Op.OpBitwiseXor or Op.OpShiftLeftLogical or Op.OpShiftRightLogical or Op.OpShiftRightArithmetic)
                return Int32(x.Type) && Int32(Def(a[0]).Type) && Int32(Def(a[1]).Type) ? Binary(op, Eval(a[0]), Eval(a[1])) : Shape(x.Type);
            if (op == Op.OpUMulExtended && Int32(Def(a[0]).Type) && Int32(Def(a[1]).Type))
            {
                Value aa = Eval(a[0]), bb = Eval(a[1]);
                if (aa.R == null || bb.R == null) return Shape(x.Type);
                return new(P: new[] { Binary(Op.OpIMul, aa, bb), Normal(from p in aa.R from q in bb.R select new Range(p.Lo * q.Lo >> 32, p.Hi * q.Hi >> 32)) });
            }
            if (op == Op.OpPhi)
            {
                var leaves = new List<Value>(); var seen = new HashSet<uint>();
                var pending = new Stack<uint>(); pending.Push(x.Id);
                while (pending.TryPop(out uint id))
                {
                    Tick(); if (!seen.Add(id)) continue;
                    Inst v = Def(id);
                    if (v.Op == Op.OpPhi)
                    {
                        Function f = functions[v.Fn];
                        for (int k = 0; k < v.A.Length; k += 2)
                            if (f.Reach.Contains(v.A[k + 1]) && f.Succ[v.A[k + 1]].Contains(v.Block)) pending.Push(v.A[k]);
                    }
                    else if ((v.Op is Op.OpCopyObject or Op.OpBitcast) && Bits32(v.Type) && Bits32(Def(v.A[0]).Type)) pending.Push(v.A[0]);
                    else leaves.Add(Eval(id));
                }
                return leaves.Count == 0 ? Shape(x.Type) : Union(leaves);
            }
            if (op == Op.OpSelect)
            {
                // Both literal Boolean arms bound a carry without reading its operands.
                if (constants.TryGetValue(a[1], out uint t) && constants.TryGetValue(a[2], out uint f) && t <= 1 && f <= 1)
                    return Union(new[] { Const(t), Const(f) });
                Value cond = Eval(a[0]); var ds = new List<Value>();
                if (Contains(cond, 1)) ds.Add(Eval(a[1]));
                if (Contains(cond, 0)) ds.Add(Eval(a[2]));
                return ds.Count == 0 ? Shape(x.Type) : Union(ds);
            }
            if (op is Op.OpULessThan or Op.OpULessThanEqual or Op.OpUGreaterThan or Op.OpUGreaterThanEqual or Op.OpSLessThan or Op.OpSLessThanEqual or Op.OpSGreaterThan or Op.OpSGreaterThanEqual or Op.OpIEqual or Op.OpINotEqual)
            {
                if (!Int32(Def(a[0]).Type) || !Int32(Def(a[1]).Type)) return Bool;
                Value aa = Eval(a[0]), bb = Eval(a[1]);
                if (aa.R == null || bb.R == null || aa.R.Length == 0 || bb.R.Length == 0) return Bool;
                bool signed = op is Op.OpSLessThan or Op.OpSLessThanEqual or Op.OpSGreaterThan or Op.OpSGreaterThanEqual;
                var ar = signed ? Signed(aa).ToArray() : aa.R.Select(r => ((long)r.Lo, (long)r.Hi)).ToArray();
                var br = signed ? Signed(bb).ToArray() : bb.R.Select(r => ((long)r.Lo, (long)r.Hi)).ToArray();
                long la = ar.Min(r => r.Item1), ha = ar.Max(r => r.Item2), lb = br.Min(r => r.Item1), hb = br.Max(r => r.Item2);
                if (op is Op.OpULessThan or Op.OpSLessThan) { if (ha < lb) return Const(1); if (la >= hb) return Const(0); }
                if (op is Op.OpULessThanEqual or Op.OpSLessThanEqual) { if (ha <= lb) return Const(1); if (la > hb) return Const(0); }
                if (op is Op.OpUGreaterThan or Op.OpSGreaterThan) { if (la > hb) return Const(1); if (ha <= lb) return Const(0); }
                if (op is Op.OpUGreaterThanEqual or Op.OpSGreaterThanEqual) { if (la >= hb) return Const(1); if (ha < lb) return Const(0); }
                if (op is Op.OpIEqual or Op.OpINotEqual)
                {
                    bool? equal = aa.R.All(p => bb.R.All(q => p.Hi < q.Lo || q.Hi < p.Lo)) ? false : la == ha && lb == hb && la == lb ? true : null;
                    if (equal.HasValue) return Const((equal.Value == (op == Op.OpIEqual)) ? 1u : 0);
                }
                return Bool;
            }
            if (op is Op.OpLogicalAnd or Op.OpLogicalOr or Op.OpLogicalEqual or Op.OpLogicalNotEqual)
            {
                ulong[] av = Values(Eval(a[0])), bv = Values(Eval(a[1]));
                if (av == null || bv == null || av.Any(v => v > 1) || bv.Any(v => v > 1)) return Bool;
                return Normal(from p in av from q in bv let z = op switch { Op.OpLogicalAnd => p != 0 && q != 0, Op.OpLogicalOr => p != 0 || q != 0, Op.OpLogicalEqual => p == q, _ => p != q } select new Range(z ? 1u : 0, z ? 1u : 0));
            }
            if (op == Op.OpLogicalNot)
            {
                ulong[] vs = Values(Eval(a[0]));
                return vs == null || vs.Any(v => v > 1) ? Bool : Normal(vs.Select(v => new Range(v == 0 ? 1u : 0, v == 0 ? 1u : 0)));
            }
            if (op == Op.OpBitCount && Int32(Def(a[0]).Type)) return new(new[] { new Range(0, 32) });
            if (op == Op.OpNot && Int32(x.Type))
            { Value d = Eval(a[0]); return d.R == null ? Full : Normal(d.R.Select(r => new Range(Mask ^ r.Hi, Mask ^ r.Lo))); }
            if (op == Op.OpSNegate && Int32(x.Type)) return Binary(Op.OpISub, Const(0), Eval(a[0]));
            return Shape(x.Type);
        }
        void Clear() { memo.Clear(); active.Clear(); }
        void Prune(bool dom, int iterations)
        {
            for (int i = 0; i < iterations; i++)
            {
                int changes = 0; Clear();
                foreach (Function f in functions.Values)
                {
                    foreach (uint b in f.Reach.ToArray())
                    {
                        Inst x = f.Blocks[b][^1];
                        if (x.Op != Op.OpBranchConditional) continue;
                        ulong[] vs = Values(Eval(x.A[0]));
                        if (vs == null || vs.Length != 1 || vs[0] > 1) continue;
                        uint keep = x.A[vs[0] == 0 ? 2 : 1];
                        uint[] next = f.Succ[b].Where(v => v == keep).ToArray();
                        if (!next.SequenceEqual(f.Succ[b])) { f.Succ[b] = next; changes++; }
                    }
                    Recompute(f, dom);
                }
                if (changes == 0) break;
            }
            Clear();
        }
        bool Reaches(Dictionary<uint, uint[]> cfg, uint from, uint to)
        {
            var seen = new HashSet<uint>(); var todo = new Stack<uint>(); todo.Push(from);
            while (todo.TryPop(out uint b)) { Tick(); if (b == to) return true; if (seen.Add(b)) foreach (uint s in cfg[b]) todo.Push(s); }
            return false;
        }
        uint CopySource(Inst x)
        {
            if ((x.Op is Op.OpCopyObject or Op.OpBitcast) && Bits32(x.Type) && Bits32(Def(x.A[0]).Type)) return x.A[0];
            return 0;
        }
        bool Stable(uint id) => !cyclic.Contains(Def(id).Block);
        void Context(Inst call)
        {
            facts.Clear(); constraints.Clear(); Clear();
            foreach (var (id, f) in functions)
            { f.Succ = baseCfg[id].ToDictionary(v => v.Key, v => v.Value.ToArray()); f.Reach = new(baseReach[id]); f.Dom = baseDom[id]; }
            Function caller = functions[call.Fn]; var cfg = baseCfg[call.Fn];
            foreach (uint b in baseReach[call.Fn])
            {
                Inst x = caller.Blocks[b][^1];
                if (x.Op != Op.OpBranchConditional || !Stable(x.A[0]) || !baseDom[call.Fn][call.Block].Contains(b)) continue;
                bool t = Reaches(cfg, x.A[1], call.Block), f = Reaches(cfg, x.A[2], call.Block);
                if (t != f) facts[x.A[0]] = t ? 1u : 0;
            }
            var todo = new Stack<(uint, uint)>(facts.Select(v => (v.Key, v.Value)));
            while (todo.TryPop(out var pair))
            {
                Tick(); var (q, value) = pair; Inst x = Def(q); var pairs = new List<(uint, uint)>();
                uint copied = CopySource(x);
                if (copied != 0 && Def(x.Type).Op == Op.OpTypeBool) pairs.Add((copied, value));
                if (x.Op == Op.OpLogicalNot) pairs.Add((x.A[0], 1 - value));
                if (x.Op == Op.OpLogicalAnd && value == 1) pairs.AddRange(x.A.Select(v => (v, 1u)));
                if (x.Op == Op.OpLogicalOr && value == 0) pairs.AddRange(x.A.Select(v => (v, 0u)));
                foreach (var (n, v) in pairs) if (Stable(n) && facts.TryAdd(n, v)) todo.Push((n, v));
            }
            foreach (var (q, value) in facts.ToArray())
            {
                Inst x = Def(q);
                if (x.Op is not (Op.OpULessThan or Op.OpULessThanEqual or Op.OpUGreaterThan or Op.OpUGreaterThanEqual)) continue;
                uint lhs = x.A[0], rhs = x.A[1];
                if (!Stable(lhs) || !Int32(Def(lhs).Type) || !Int32(Def(rhs).Type)) continue;
                Value d = Eval(rhs);
                if (d.R == null || d.R.Length == 0) continue;
                long lo = (long)d.R[0].Lo, hi = (long)d.R[^1].Hi, a = 0, b = (long)Mask;
                if (x.Op == Op.OpULessThan) { if (value != 0) b = hi - 1; else a = lo; }
                if (x.Op == Op.OpULessThanEqual) { if (value != 0) b = hi; else a = lo + 1; }
                if (x.Op == Op.OpUGreaterThan) { if (value != 0) a = lo + 1; else b = hi; }
                if (x.Op == Op.OpUGreaterThanEqual) { if (value != 0) a = lo; else b = hi - 1; }
                Value bound = a > b || a > (long)Mask || b < 0 ? new(Array.Empty<Range>()) : new(new[] { new Range((ulong)Math.Max(0, a), (ulong)Math.Min((long)Mask, b)) });
                constraints[lhs] = Intersect(constraints.GetValueOrDefault(lhs, Full), bound);
            }
            var pending = new Stack<uint>(constraints.Keys);
            while (pending.TryPop(out uint id))
            {
                Tick(); Inst x = Def(id);
                uint src = CopySource(x);
                if (src == 0 || !Stable(src)) continue;
                Value old = constraints.GetValueOrDefault(src, Full), next = Intersect(old, constraints[id]);
                if (!old.R.SequenceEqual(next.R)) { constraints[src] = next; pending.Push(src); }
            }
            Clear(); Prune(false, 12);
        }
        public JsonObject Run()
        {
            var writers = new HashSet<uint>();
            foreach (Inst x in lines)
            {
                uint ptr;
                if (x.Op == Op.OpStore || x.Op == Op.OpAtomicStore) ptr = x.A[0];
                else if (x.Op.ToString().StartsWith("OpAtomic", StringComparison.Ordinal) && x.Op != Op.OpAtomicLoad) ptr = x.A[0];
                else if (x.Op is Op.OpCopyMemory or Op.OpCopyMemorySized) ptr = x.A[0];
                else continue;
                Inst pt = Def(Def(ptr).Type);
                if (pt.Op != Op.OpTypePointer || pt.A[0] != (uint)StorageClass.PhysicalStorageBuffer) continue;
                Inst storedType = Def(pt.A[1]);
                if ((storedType.Op is not (Op.OpTypeInt or Op.OpTypeFloat)) || storedType.A[0] > 32 || storedType.A[0] == 0)
                    throw new InvalidOperationException("physical write width unsupported");
                if (x.Fn == 0 || !names.TryGetValue(x.Fn, out string name) || !name.StartsWith("dmsh_raw_write_", StringComparison.Ordinal))
                    throw new InvalidOperationException("unattested physical writer");
                if (x.Op is Op.OpCopyMemory or Op.OpCopyMemorySized) throw new InvalidOperationException("physical copy unsupported");
                writers.Add(x.Fn);
            }
            if (writers.Count == 0) throw new InvalidOperationException("no physical writes");
            foreach (uint id in writers)
            {
                Function f = functions[id];
                if (f.Params.Count < 2) throw new InvalidOperationException("writer ABI arity");
                foreach (uint p in f.Params.Take(2))
                { Inst pt = Def(Def(p).Type); if (pt.Op != Op.OpTypePointer || pt.A[0] != (uint)StorageClass.Function || !Int32(pt.A[1])) throw new InvalidOperationException("writer ABI type"); }
            }
            var calls = lines.Where(x => x.Op == Op.OpFunctionCall && writers.Contains(x.A[0])).ToArray();
            if (calls.Length == 0 || calls.Length > 256 || calls.Any(x => x.Fn != entry)) throw new InvalidOperationException("nested raw writer unsupported");
            Prune(true, 16);
            foreach (var (id, f) in functions)
            {
                baseCfg[id] = f.Succ.ToDictionary(v => v.Key, v => v.Value.ToArray()); baseReach[id] = new(f.Reach); baseDom[id] = f.Dom;
                foreach (uint b in f.Reach)
                    if (f.Succ[b].Any(s => Reaches(f.Succ, s, b))) cyclic.Add(b);
            }
            var results = new List<Value>(); var callResults = new JsonArray();
            foreach (Inst call in calls.Where(x => baseReach[x.Fn].Contains(x.Block)))
            {
                Context(call); uint p = call.A[2]; Inst var = Def(p);
                if (var.Op != Op.OpVariable || var.A[0] != (uint)StorageClass.Function || Root(p) != p) throw new InvalidOperationException("writer high argument unsupported");
                Value d = LocalLoad(p, new(), call);
                if (d.R == null || d.R.Length == 0 || d.R.Any(r => r.Hi == Mask)) throw new InvalidOperationException("unbounded write address at call " + call.Id + " high=" + p);
                // A scalar store may cross the 4-GiB boundary. The ABI promises a
                // maximum 4-byte access per helper, so include its successor stripe.
                d = Normal(d.R.SelectMany(r => new[] { r, new Range(r.Lo + 1, r.Hi + 1) }));
                results.Add(d); callResults.Add((JsonNode)new JsonObject { ["call"] = call.Id, ["helper"] = call.A[0], ["high_intervals"] = JsonRanges(d) });
            }
            Value domain = Union(results);
            if (domain.R == null || domain.R.Length == 0 || domain.R.Any(r => r.Hi == Mask)) throw new InvalidOperationException("empty or unbounded proof");
            var highReads = new Dictionary<(uint, uint), uint>(reads);
            var proof = new JsonObject { ["ok"] = true, ["abi"] = 1, ["grid"] = JsonTriplet(grid), ["local"] = JsonTriplet(local), ["simd_width"] = simd,
                ["guards"] = Guards(highReads), ["high_intervals"] = JsonRanges(domain), ["calls"] = callResults, ["analysis_steps"] = 2_000_000 - budget };
            // Refinement (2026-11-08): the helper writes the 4-byte word at (high << 32) + low,
            // so the low word's range narrows each 4-GiB stripe to exact bytes. Its extra
            // uniform reads become separate guards: when they do not match at a dispatch,
            // the runtime keeps the stripes above, so the refinement can never widen waits.
            int highSteps = 2_000_000 - budget;
            budget = 2_000_000; // the refinement has its own bounded budget; failure keeps the stripes
            var bytes = ByteIntervals(calls.Where(x => baseReach[x.Fn].Contains(x.Block)).ToArray(), out string why);
            if (bytes == null) proof["byte_reject"] = why;
            if (bytes != null)
            {
                proof["byte_intervals"] = new JsonArray(bytes.Select(r => (JsonNode)new JsonArray(JsonValue.Create(r.Lo), JsonValue.Create(r.Hi))).ToArray());
                proof["byte_guards"] = Guards(reads);
            }
            proof["analysis_steps"] = highSteps; proof["refine_steps"] = 2_000_000 - budget;
            return proof;
        }
        JsonArray Guards(Dictionary<(uint, uint), uint> words)
        {
            var guards = new JsonArray();
            foreach (var (key, word) in words.OrderBy(v => v.Key.Item1).ThenBy(v => v.Key.Item2))
            {
                var (p, off) = key; int reg = regs[p];
                if (reg < 0) continue; // c1 is part of the source-identity capsule
                if (off % 4 != 0) throw new InvalidOperationException("unaligned uniform guard");
                guards.Add((JsonNode)new JsonObject { ["reg"] = reg, ["arg_id"] = aids[p], ["offset"] = off, ["word"] = word });
            }
            return guards;
        }
        // Exact byte ranges [Lo, Hi) of every reachable writer call, or null when any
        // call's low or high word is not a small bounded set (the stripes then stand).
        List<(ulong Lo, ulong Hi)> ByteIntervals(Inst[] calls, out string why)
        {
            var spans = new List<(ulong Lo, ulong Hi)>(); why = null;
            try
            {
                foreach (Inst call in calls)
                {
                    Context(call);
                    uint pl = call.A[1], ph = call.A[2];
                    Inst lv = Def(pl);
                    if (lv.Op != Op.OpVariable || lv.A[0] != (uint)StorageClass.Function || Root(pl) != pl) { why = "low argument at call " + call.Id; return null; }
                    Value high = LocalLoad(ph, new(), call), low = LocalLoad(pl, new(), call);
                    ulong[] hv = Values(high, 64);
                    if (hv == null || hv.Length == 0) { why = "high not a small set at call " + call.Id; return null; }
                    if (low.R == null || low.R.Length == 0) { why = "low unsupported at call " + call.Id; return null; }
                    foreach (ulong h in hv) foreach (Range r in low.R)
                        spans.Add(((h << 32) + r.Lo, (h << 32) + r.Hi + 4)); // a 4-byte word may carry into the next stripe
                }
            }
            catch (InvalidOperationException e) { why = e.Message; return null; } // budget exhausted: keep the stripe proof
            spans.Sort();
            var merged = new List<(ulong Lo, ulong Hi)>();
            foreach (var s in spans)
                if (merged.Count != 0 && s.Lo <= merged[^1].Hi) merged[^1] = (merged[^1].Lo, Math.Max(merged[^1].Hi, s.Hi));
                else merged.Add(s);
            while (merged.Count > 64)
            {   // widening is always sound: join the two neighbours with the smallest gap
                int best = 0; ulong gap = ulong.MaxValue;
                for (int i = 0; i + 1 < merged.Count; i++) if (merged[i + 1].Lo - merged[i].Hi < gap) { gap = merged[i + 1].Lo - merged[i].Hi; best = i; }
                merged[best] = (merged[best].Lo, merged[best + 1].Hi); merged.RemoveAt(best + 1);
            }
            return merged;
        }
    }
    static JsonArray JsonTriplet(uint[] a) => new(a.Select(v => (JsonNode)JsonValue.Create(v)).ToArray());
    static JsonArray JsonRanges(Value d) => new(d.R.Select(r => (JsonNode)new JsonArray(JsonValue.Create(r.Lo), JsonValue.Create(r.Hi))).ToArray());
    public static string Handle(string json)
    {
        try
        {
            if (json == null || json.Length > 4_194_304) throw new InvalidOperationException("proof request too large");
            return new Analyzer(JsonNode.Parse(json).AsObject()).Run().ToJsonString();
        }
        catch (Exception e) when (e is not OutOfMemoryException && e is not StackOverflowException)
        { return new JsonObject { ["ok"] = false, ["error"] = e.GetBaseException().Message }.ToJsonString(); }
    }
}

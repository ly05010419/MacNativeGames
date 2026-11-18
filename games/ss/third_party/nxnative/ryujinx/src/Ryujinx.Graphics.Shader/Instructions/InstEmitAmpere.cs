using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static class AmpereOpcodes
    {
        public const int Bra = 0x947;
        public const int Brx = 0x949;
        public const int Brxu = 0x958;   // BRX on a uniform register
        public const int Exit = 0x94d;
        public const int Kill = 0x95b;
        public const int Ret = 0x950;
        public const int Call = 0x944;
        public const int Ipa = 0x326;
        public const int IpaIdx = 0x526;
        public const int Ald = 0x321;
        public const int Ast = 0x322;
        public const int Ldc = 0xb82;
        public const int Ldl = 0x983;
    }

    // Ampere opcode → emitter. ALU instructions come in operand forms (bits 9..11, AmpereOp.Form) and are registered by their 9-bit opcode; the others by the
    // whole 12-bit opcode, which is looked up first.
    static class AmpereTable
    {
        private readonly struct Entry
        {
            public readonly InstName Name;
            public readonly InstEmitter Emitter;
            public readonly InstProps Props;
            public readonly bool UniformGuard;   // a uniform-datapath instruction: its guard (12..15) names a uniform predicate
            public readonly bool WritesRd;

            public Entry(InstName name, InstEmitter emitter, InstProps props, bool uniformGuard, bool writesRd)
            {
                Name = name;
                Emitter = emitter;
                Props = props;
                UniformGuard = uniformGuard;
                WritesRd = writesRd;
            }
        }

        private static readonly Dictionary<int, Entry> _exact = new();
        private static readonly Dictionary<int, Entry> _alu = new();

        static AmpereTable()
        {
            const InstProps B = InstProps.Bra;

            // Control flow. Convergence barriers (BSSY 0x945, BSYNC 0x941, BREAK 0x942), WARPSYNC 0x948, DEPBAR 0x91a and NOP 0x918 only order the warp's
            // threads: per thread they do nothing.
            X(AmpereOpcodes.Bra, InstName.Bra, InstEmit.ABra, B, writesRd: false);
            X(AmpereOpcodes.Brx, InstName.Brx, InstEmit.ABrx, B, writesRd: false);
            X(AmpereOpcodes.Brxu, InstName.Brx, InstEmit.ABrx, B, writesRd: false);
            X(AmpereOpcodes.Exit, InstName.Exit, InstEmit.AExit, B, writesRd: false);
            X(AmpereOpcodes.Kill, InstName.Kil, InstEmit.AKill, B, writesRd: false);
            X(AmpereOpcodes.Ret, InstName.Ret, InstEmit.ARet, B, writesRd: false);
            X(AmpereOpcodes.Call, InstName.Cal, InstEmit.ACall, B, writesRd: false);
            foreach (int op in new[] { 0x918, 0x91a, 0x945, 0x941, 0x942, 0x948 })
            {
                X(op, InstName.Nop, InstEmit.ANop, InstProps.None, writesRd: false);
            }

            InstEmit.RegisterAmpereAlu(Alu, X);
            InstEmit.RegisterAmpereMemory(X);
            InstEmit.RegisterAmpereTensor(X);
            InstEmit.RegisterAmpereHalfCompare(Alu);
            InstEmit.RegisterAmpereGeometry(Alu, X);
        }

        private static void X(int opcode, InstName name, InstEmitter emitter, InstProps props, bool uniform = false, bool writesRd = true)
        {
            _exact[opcode] = new Entry(name, emitter, props, uniform, writesRd);
        }

        private static void Alu(int op9, InstName name, InstEmitter emitter, bool uniform = false, bool writesRd = true)
        {
            _alu[op9] = new Entry(name, emitter, InstProps.None, uniform, writesRd);
        }

        private static bool TryGet(int opcode, out Entry entry)
        {
            return _exact.TryGetValue(opcode, out entry) || _alu.TryGetValue(opcode & 0x1ff, out entry);
        }

        public static InstOp GetOp(ulong address, ulong lo, ulong hi)
        {
            int opcode = (int)(lo & 0xfff);

            if (TryGet(opcode, out Entry e))
            {
                return new InstOp(address, lo, hi, e.Name, e.Emitter, e.Props);
            }

            return new InstOp(address, lo, hi, InstName.Invalid, InstEmit.AUnsupported, InstProps.None);
        }

        public static bool HasUniformGuard(int opcode) => TryGet(opcode, out Entry e) && e.UniformGuard;

        public static bool WritesRd(int opcode) => !TryGet(opcode, out Entry e) || e.WritesRd;
    }

    // An ALU source operand: a register, a uniform register, a 32-bit immediate or a constant buffer word, with the float or integer modifiers it carries.
    enum AKind
    {
        Reg,
        UReg,
        Imm,
        Cbuf,
    }

    readonly struct ASrc
    {
        public readonly AKind Kind;
        public readonly int Value;   // register number, immediate, or constant buffer byte offset
        public readonly int Bank;
        public readonly bool Neg;
        public readonly bool Abs;

        public ASrc(AKind kind, int value, int bank = 0, bool neg = false, bool abs = false)
        {
            Kind = kind;
            Value = value;
            Bank = bank;
            Neg = neg;
            Abs = abs;
        }

        public ASrc WithMods(bool neg, bool abs) => new(Kind, Value, Bank, neg, abs);

        public bool IsZeroReg => (Kind == AKind.Reg && Value == RegisterConsts.RegisterZeroIndex) || (Kind == AKind.UReg && Value == RegisterConsts.UniformZeroIndex);
    }

    static partial class InstEmit
    {
        public static void AUnsupported(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            throw new NotSupportedException($"Ampere instruction {op.Opcode:x3} at 0x{context.CurrOp.Address:x} ({op.Hi:x16}{op.Lo:x16}) is not implemented");
        }

        public static void ANop(EmitterContext context)
        {
        }

        private static NotSupportedException AFail(EmitterContext context, string what)
        {
            AmpereOp op = new(context.CurrOp);

            return new NotSupportedException($"Ampere {op.Opcode:x3} at 0x{context.CurrOp.Address:x}: {what} ({op.Hi:x16}{op.Lo:x16})");
        }

        // ---- operands ----

        // A, B, C of an ALU instruction by its form. Uniform instructions name uniform registers (6 bits) in the register fields and take no constant buffer.
        // neg / abs: whether the operation has the float modifier bits (A 72 / 73, B 63 / 62, C 75 / 74); integer operations use some of them for other fields.
        private static (ASrc A, ASrc B, ASrc C) AAluSrcs(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            AKind rk = uniform ? AKind.UReg : AKind.Reg;
            int rbits = uniform ? 6 : 8;

            ASrc Reg(int lo, int negBit, int absBit) => new(rk, x.Bits(lo, lo + rbits), 0, neg && x.Bit(negBit), abs && x.Bit(absBit));

            ASrc a = Reg(24, 72, 73);
            ASrc c64 = Reg(64, 75, 74);

            ASrc Slot1(AKind kind)
            {
                return kind switch
                {
                    AKind.Reg => Reg(32, 63, 62),
                    AKind.Imm => new ASrc(AKind.Imm, (int)x.Imm32),
                    AKind.Cbuf => ACbufOperand(context, x, neg && x.Bit(63), abs && x.Bit(62)),
                    _ => new ASrc(AKind.UReg, x.Bits(32, 38), 0, neg && x.Bit(63), abs && x.Bit(62)),
                };
            }

            int form = x.Form;

            if (uniform)
            {
                return form switch
                {
                    1 => (a, Slot1(AKind.Reg), c64),
                    2 => (a, c64, Slot1(AKind.Imm)),
                    4 => (a, Slot1(AKind.Imm), c64),
                    _ => throw AFail(context, $"uniform ALU form {form}"),
                };
            }

            return form switch
            {
                1 => (a, Slot1(AKind.Reg), c64),
                2 => (a, c64, Slot1(AKind.Imm)),
                3 => (a, c64, Slot1(AKind.Cbuf)),
                4 => (a, Slot1(AKind.Imm), c64),
                5 => (a, Slot1(AKind.Cbuf), c64),
                6 => (a, Slot1(AKind.UReg), c64),
                7 => (a, c64, Slot1(AKind.UReg)),
                _ => throw AFail(context, $"ALU form {form}"),
            };
        }

        // The second source of a two-source ALU operation: B in forms 1, 4, 5, 6; C in forms 2, 3, 7.
        private static (ASrc A, ASrc B) ASecond(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, uniform, neg, abs);

            return (a, x.Form is 1 or 4 or 5 or 6 ? b : c);
        }

        // The only source of a one-source operation (MOV, MUFU, F2I, ...): the B slot.
        private static ASrc AOne(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            return AAluSrcs(context, x, uniform, neg, abs).B;
        }

        private static ASrc ACbufOperand(EmitterContext context, AmpereOp x, bool neg, bool abs)
        {
            if (x.Bit(91))
            {
                throw AFail(context, "constant buffer operand with its bank in a uniform register");
            }

            (int bank, int offset) = AMapC0(context, x.Bits(54, 59), x.Bits(38, 54));

            return new ASrc(AKind.Cbuf, offset, bank, neg, abs);
        }

        // NVN2 compute shaders lay out their driver constants in c[0] per shader (the control section counts them, QueryAmpereComputeC0Layout): from 0x10, the push
        // constants, then a 0x10-byte entry per uniform buffer slot, then one per storage buffer slot, then an 8-byte handle per texture slot, then per image slot (all
        // 205 compute shaders of Donkey Kong Bananza read their c[0] that way). They are mapped to the places the build's reflection and the host know: the push
        // constants to c[24] (where the graphics stages read theirs), uniform slot r to c[0] + 0x210 + 0x10 r and storage slot s to c[0] + 0x310 + 0x10 s (NVN's
        // compute tables), texture slot t to c[2] + 0x20 + 8 t and image slot i to c[2] + 0x120 + 8 i (NVN's handle tables). Other offsets stay.
        private static (int Bank, int Offset) AMapC0(EmitterContext context, int bank, int offset)
        {
            int[] layout = bank == 0 ? context.TranslatorContext.GpuAccessor.QueryAmpereComputeC0Layout() : null;

            if (layout == null || layout.Length < 5)
            {
                return (bank, offset);
            }

            int ubos = layout[0], ssbos = layout[1], textures = layout[2], size = layout[3], push = (layout[4] + 0xf) & ~0xf;
            int ub = 0x10 + push;
            int sb = ub + 0x10 * ubos;
            int hb = sb + 0x10 * ssbos;
            int ib = hb + 8 * textures;

            if (offset >= 0x10 && offset < ub)
            {
                return (24, offset - 0x10);
            }

            if (offset >= ub && offset < sb)
            {
                return (0, 0x210 + (offset - ub));
            }

            if (offset >= sb && offset < hb)
            {
                return (0, 0x310 + (offset - sb));
            }

            if (offset >= hb && offset < ib)
            {
                return (2, 0x20 + (offset - hb));
            }

            if (offset >= ib && offset < size)
            {
                return (2, 0x120 + (offset - ib));
            }

            return (bank, offset);
        }

        // The operand's 32-bit value, modifiers not applied.
        private static Operand AGet(EmitterContext context, ASrc s)
        {
            switch (s.Kind)
            {
                case AKind.Reg:
                    return Register(s.Value, RegisterType.Gpr);
                case AKind.UReg:
                    return Register(s.Value, RegisterType.Uniform);
                case AKind.Imm:
                    return Const(s.Value);
                default:
                    if ((s.Value & 3) != 0)
                    {
                        throw AFail(context, "unaligned constant buffer operand");
                    }

                    return Cbuf(s.Bank, s.Value >> 2);
            }
        }

        // The upper word of a 64-bit operand pair.
        private static Operand AGetHigh(EmitterContext context, ASrc s)
        {
            return s.Kind switch
            {
                AKind.Reg => s.Value == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(s.Value + 1, RegisterType.Gpr),
                AKind.UReg => s.Value == RegisterConsts.UniformZeroIndex ? Const(0) : Register(s.Value + 1, RegisterType.Uniform),
                AKind.Imm => Const(s.Value < 0 ? -1 : 0),
                _ => Cbuf(s.Bank, (s.Value >> 2) + 1),
            };
        }

        private static Operand AFloat(EmitterContext context, ASrc s, Instruction fpType = Instruction.FP32)
        {
            return context.FPAbsNeg(AGet(context, s), s.Abs, s.Neg, fpType);
        }

        private static Operand AInt(EmitterContext context, ASrc s)
        {
            if (s.Abs)
            {
                throw AFail(context, "integer absolute value modifier");
            }

            Operand v = AGet(context, s);

            return s.Neg ? context.INegate(v) : v;
        }

        private static Operand ADestReg(int rd, bool uniform)
        {
            return uniform ? Register(rd, RegisterType.Uniform) : Register(rd, RegisterType.Gpr);
        }

        // The destination register field: 16..24, or 16..22 on the uniform datapath.
        private static int ARd(AmpereOp x, bool uniform) => uniform ? x.Bits(16, 22) : x.Rd;

        private static void ASet(EmitterContext context, AmpereOp x, Operand value, bool uniform = false)
        {
            context.Copy(ADestReg(ARd(x, uniform), uniform), value);
        }

        private static Operand APredReg(int index, bool uniform)
        {
            return Register(index, uniform ? RegisterType.UniformPredicate : RegisterType.Predicate);
        }

        // A predicate source: 3 bits at lo, the not bit at notBit.
        private static Operand APred(EmitterContext context, AmpereOp x, int lo, int notBit, bool uniform = false)
        {
            Operand p = APredReg(x.Bits(lo, lo + 3), uniform);

            return x.Bit(notBit) ? context.BitwiseNot(p) : p;
        }

        private static void ASetPred(EmitterContext context, int index, Operand value, bool uniform = false)
        {
            if (index != RegisterConsts.PredicateTrueIndex)
            {
                context.Copy(APredReg(index, uniform), value);
            }
        }

        // ---- control flow ----

        // BRA [.U] target: taken where the guard (handled here, not by the caller) and the condition predicate (87..90, a uniform one with bit 91) both hold.
        public static void ABra(EmitterContext context)
        {
            InstOp currOp = context.CurrOp;
            AmpereOp op = new(currOp);
            ulong target = Decoder.AmpereTarget(currOp);

            if (target == currOp.Address + Decoder.AmpereInstSize)
            {
                return;
            }

            Operand label = context.GetLabel(target);
            Operand cond = AGuardAndCondition(context, op, op.Bit(91));

            if (cond == null)
            {
                context.Branch(label);
            }
            else
            {
                context.BranchIfTrue(label, cond);
            }
        }

        // The guard (12..15) and the condition predicate (87..90) of a branch-like instruction, ANDed; null when both are always true.
        private static Operand AGuardAndCondition(EmitterContext context, AmpereOp op, bool uniformCondition)
        {
            Operand result = null;

            if (op.Pred != RegisterConsts.PredicateTrueIndex || op.PredInv)
            {
                Operand g = APredReg(op.Pred, AmpereTable.HasUniformGuard(op.Opcode));
                result = op.PredInv ? context.BitwiseNot(g) : g;
            }

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                Operand c = APred(context, op, 87, 90, uniformCondition);
                result = result == null ? c : context.BitwiseAnd(result, c);
            }

            return result;
        }

        // BRX Ra, imm: to the next instruction + Ra + imm; the decoder found the targets in the c1 jump table. Each target but the last is taken where the
        // address matches; the last one unconditionally (Ra holds one of the table's values).
        public static void ABrx(EmitterContext context)
        {
            InstOp currOp = context.CurrOp;
            AmpereOp op = new(currOp);
            int startIndex = context.CurrBlock.HasNext() ? 1 : 0;

            if (context.CurrBlock.Successors.Count <= startIndex)
            {
                throw AFail(context, "BRX with no target found in the c1 jump table");
            }

            long next = (long)currOp.Address + Decoder.AmpereInstSize + op.BranchOffset;
            Operand address = context.IAdd(Register(op.Ra, op.Opcode == AmpereOpcodes.Brxu ? RegisterType.Uniform : RegisterType.Gpr), Const((int)next));
            Block[] targets = context.CurrBlock.Successors.Skip(startIndex).OrderBy(b => b.Address).ToArray();

            for (int i = 0; i < targets.Length; i++)
            {
                Operand label = context.GetLabel(targets[i].Address);

                if (i == targets.Length - 1)
                {
                    context.Branch(label);
                }
                else
                {
                    context.BranchIfTrue(label, context.ICompareEqual(address, Const((int)targets[i].Address)));
                }
            }
        }

        public static void AExit(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            if (context.IsNonMain)
            {
                throw AFail(context, "EXIT inside a called function");
            }

            Operand lblSkip = null;

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                lblSkip = Label();
                context.BranchIfFalse(lblSkip, APred(context, op, 87, 90));
            }

            if (context.PrepareForReturn())
            {
                context.Return();
            }

            if (lblSkip != null)
            {
                context.MarkLabel(lblSkip);
            }
        }

        public static void AKill(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                Operand lblSkip = Label();
                context.BranchIfFalse(lblSkip, APred(context, op, 87, 90));
                context.Discard();
                context.MarkLabel(lblSkip);
            }
            else
            {
                context.Discard();
            }
        }

        // CALL.REL.NOINC target: a function call; the callee returns to the next instruction (RET.REL.NODEC, through the return offset the caller left in a
        // register, which the call itself replaces).
        public static void ACall(EmitterContext context)
        {
            if (Decoder.IsAmpereJumpCall(context.Program, context.CurrOp))
            {
                context.Branch(context.GetLabel(Decoder.AmpereTarget(context.CurrOp)));
                return;
            }

            DecodedFunction function = context.Program.GetFunctionByAddress(Decoder.AmpereTarget(context.CurrOp));

            context.Call(function.Id, false);
        }

        public static void ARet(EmitterContext context)
        {
            if (!context.IsNonMain)
            {
                throw AFail(context, "RET in the main function");
            }

            context.Return();
        }
    }
}

using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using System.Numerics;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere attribute, memory and texture instructions. Field positions: Mesa NAK sm70_encode.rs (OpIpa, OpALd, OpASt, OpLd, OpSt, OpAtom, OpTex ...).
    static partial class InstEmit
    {
        public static void RegisterAmpereMemory(AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            x(AmpereOpcodes.Ipa, InstName.Ipa, AIpa, N);
            x(AmpereOpcodes.IpaIdx, InstName.Ipa, AIpa, N);
            x(AmpereOpcodes.Ald, InstName.Ald, AAld, N);
            x(AmpereOpcodes.Ast, InstName.Ast, AAst, N, writesRd: false);

            x(0x981, InstName.Ldg, ALdg, N);
            x(0x381, InstName.Ldg, ALdg, N);
            x(0x986, InstName.Stg, AStg, N, writesRd: false);
            x(0x386, InstName.Stg, AStg, N, writesRd: false);
            x(0x98e, InstName.Red, ARed, N, writesRd: false);
            x(0x9a8, InstName.Atom, AAtomg, N);
            x(0x3a8, InstName.Atom, AAtomg, N);
            x(0x38c, InstName.Atoms, AAtoms, N);
            x(0xf8c, InstName.Atoms, AAtoms, N);   // ATOMS.POPC.INC (Pokémon Pokopia's histogram e4ec13f79f9e0bd4)
            x(AmpereOpcodes.Ldl, InstName.Ldl, ALdLocalShared, N);
            x(0x984, InstName.Lds, ALdLocalShared, N);
            x(0x387, InstName.Stl, AStLocalShared, N, writesRd: false);
            x(0x987, InstName.Stl, AStLocalShared, N, writesRd: false);
            x(0x388, InstName.Sts, AStLocalShared, N, writesRd: false);
            x(0x988, InstName.Sts, AStLocalShared, N, writesRd: false);
            x(0x38d, InstName.Atoms, AAtomsCas, N);
            x(0x992, InstName.Membar, AMembar, N, writesRd: false);
            x(0x9ab, InstName.Membar, AErrbar, N, writesRd: false);

            x(0xb60, InstName.Tex, ATex, InstProps.Tex);
            x(0x361, InstName.Tex, ATex, InstProps.Tex);
            x(0xf60, InstName.Tex, ATex, InstProps.Tex);
            x(0xb66, InstName.Tld, ATex, InstProps.Tex);
            x(0xb63, InstName.Tld4, ATex, InstProps.Tex);
            x(0xb69, InstName.Tmml, ATex, InstProps.Tex);
            x(0xb6c, InstName.Txd, ATex, InstProps.Tex);
            x(0xb6f, InstName.Txq, ATxq, InstProps.Tex);
            x(0x370, InstName.Txq, ATxq, InstProps.Tex);
            x(0xb97, InstName.Suld, ASurface, N);
            x(0xb99, InstName.Suld, ASurface, N);
            x(0xb9b, InstName.Sust, ASurface, N, writesRd: false);
            x(0xb9d, InstName.Sust, ASurface, N, writesRd: false);
            x(0x593, InstName.Suatom, ASuatom, N);
            x(0x59f, InstName.Sured, ASured, N, writesRd: false);
            x(0xb1d, InstName.Bar, ABar, N, writesRd: false);
        }

        // ---- attributes ----

        // IPA Rd, a[addr] (0x326) or a[URa + addr] (0x526, the index a uniform register at 24..30): addr 64..72 in words, sample location 76..78 (default,
        // centroid, offset), frequency 78..80 (pass, constant, state). Like Maxwell's IPA.PASS, a perspective attribute is read divided by w: the shader
        // multiplies by 1 / a[0x7c] itself.
        public static void AIpa(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int addr = x.Bits(64, 72) << 2;

            if (x.Bits(76, 78) == 2)
            {
                throw AFail(context, "IPA at an offset");
            }

            Operand idxAddress = null;

            if (x.Opcode == AmpereOpcodes.IpaIdx)
            {
                idxAddress = context.IAdd(Register(x.Bits(24, 30), RegisterType.Uniform), Const(addr));
            }

            IpaOp ipaOp = x.Bits(78, 80) == 1 ? IpaOp.Constant : IpaOp.Pass;

            EmitIpa(context, idxAddress, addr, ipaOp, RegisterConsts.RegisterZeroIndex, false, x.Rd);
        }

        // Geometry and tessellation stages. OUT (opcode 0x124 in the ALU forms): Rd = the new output handle, Ra the old, the stream in the second source; 78..80 emit (1),
        // cut (2) or both, none being OUTFINAL. ISBERD Rd, [Ra]: the stage buffer entry of a vertex, which only ALD reads: Ra passes through, as on Maxwell. A geometry shader
        // that ends its output through an exit stub sets it up first: LEPC R, R + offset → 0x357 installs it as the address EXIT continues at, and the stub runs OUTFINAL
        // and 0x957 before its own EXIT. Per thread, with no output to finish, EXIT just ends the invocation: LEPC writes its own address (only the stub setup reads it),
        // and 0x357 and 0x957 do nothing (Donkey Kong Bananza's geometry shader 5d537f7630ec3671, which writes its primitives to memory with ATOMG / STG and emits none)
        public static void RegisterAmpereGeometry(AmpereAluRegistrar alu, AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            alu(0x124, InstName.Out, AOut);
            x(0x923, InstName.Isberd, AIsberd, N);
            x(0x927, InstName.Ast, AStageOut, N, writesRd: false);
            x(0x34e, InstName.Lepc, ALepc, N);
            x(0x357, InstName.Nop, ANop, N, writesRd: false);
            x(0x957, InstName.Nop, ANop, N, writesRd: false);
        }

        public static void AOut(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int type = x.Bits(78, 80);

            if (type != 0)
            {
                EmitOut(context, (type & 1) != 0, (type & 2) != 0);
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), AGpr(x.Ra));
            }
        }

        public static void AIsberd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int[] mesh = context.TranslatorContext.GpuAccessor.QueryAmpereMeshLayout();

            if (mesh != null)
            {
                // A task / mesh shader translated as compute (QueryAmpereMeshLayout): ISBERD reads its stage input, the workgroup's index with bit 78 (mode 0x78; the
                // task shader's draw-wide task number, the mesh shader's number among its task's launches) and otherwise a word of the payload its task wrote
                // (mode 0x38, [Ra + offset]). Mario Kart World's grass: the mesh shader reads its blade record at [index * 0x28 + 0x10..0x34]
                Operand addr = x.Bit(78) ? Const(mesh[0]) : context.IAdd(x.Ra == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Ra, RegisterType.Gpr), Const(mesh[1] + (int)x.SignedField(40, 64)));
                EmitLoad(context, StorageKind.SharedMemory, LsSize2.B32, addr, x.Rd, 0);
                return;
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), AGpr(x.Ra));
            }
        }

        // The stage output store of task and mesh shaders (0x927) [Ra + offset], Rb: in shared memory where the build reads it back into the Metal object / mesh
        // outputs (QueryAmpereMeshLayout). Mode 72..79: 0xb8 a task payload word, 0xf8 a mesh attribute word (vertex attributes component by component, 0x80 bytes
        // a component for 32 vertices, then the primitive attributes), 0x80 a byte of the primitive indices, 0x88 the primitive count byte (at 3, the indices from 4)
        public static void AStageOut(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int[] mesh = context.TranslatorContext.GpuAccessor.QueryAmpereMeshLayout();
            int mode = x.Bits(72, 80);

            if (mesh == null || (mode != 0xb8 && mode != 0xf8 && mode != 0x80 && mode != 0x88))
            {
                throw AFail(context, $"stage output store mode {mode:x2} outside a task or mesh shader");
            }

            bool word = mode == 0xb8 || mode == 0xf8;
            Operand addr = context.IAdd(x.Ra == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Ra, RegisterType.Gpr), Const((word ? mesh[2] : mesh[3]) + (int)x.SignedField(40, 64)));

            if (word)
            {
                EmitStore(context, StorageKind.SharedMemory, LsSize2.B32, addr, x.Rb, 0);
                return;
            }

            // A byte: the SPIR-V backend has no 8-bit shared memory, and neighbouring threads write neighbouring bytes of a word, so it goes into its word with
            // two atomics (clear the byte, then set it)
            Operand id = Const(context.ResourceManager.SharedMemoryId);
            Operand wordIndex = context.ShiftRightU32(addr, Const(2));
            Operand shift = context.ShiftLeft(context.BitwiseAnd(addr, Const(3)), Const(3));
            Operand value = context.ShiftLeft(context.BitwiseAnd(x.Rb == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr), Const(0xff)), shift);
            EmitAtomicOp(context, StorageKind.SharedMemory, AtomOp.And, AtomSize.U32, id, wordIndex, context.BitwiseNot(context.ShiftLeft(Const(0xff), shift)));
            EmitAtomicOp(context, StorageKind.SharedMemory, AtomOp.Or, AtomSize.U32, id, wordIndex, value);
        }

        public static void ALepc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), Const((int)context.CurrOp.Address));
            }

            if (x.Rd + 1 < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd + 1, RegisterType.Gpr), Const(0));
            }
        }

        // ALD Rd, a[Ra + addr], vtx: addr 40..50, vertex 32..40, count - 1 74..76, patch 76, physical (Ra the whole address) 77, output 79
        public static void AAld(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (!x.Bit(77) && x.Ra != RegisterConsts.RegisterZeroIndex)
            {
                throw AFail(context, "ALD with an offset register and an address");
            }

            EmitAld(context, x.Rd, x.Ra, x.Rb, x.Bits(40, 50), x.Bits(74, 76), x.Bit(76), x.Bit(79), x.Bit(77));
        }

        // AST a[Ra + addr], data (32..40), vtx (64..72)
        public static void AAst(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (!x.Bit(77) && x.Ra != RegisterConsts.RegisterZeroIndex)
            {
                throw AFail(context, "AST with an offset register and an address");
            }

            EmitAst(context, x.Ra, x.Rb, x.Bits(40, 50), x.Bits(74, 76), x.Bit(76), x.Bit(77));
        }

        // ---- global memory ----

        // The 64-bit address of a global access: Ra (a pair with bit raPairBit, else 32 bits) plus a uniform register pair at urLo (a pair with bit 72) in
        // the uniform form (bit 91), plus the signed offset (40..64). The low word stays (UR + Ra + offset), the form Ryujinx's global-to-storage pass
        // traces back to a storage buffer table entry in c[0].
        private static (Operand Low, Operand High) AGlobalAddress(EmitterContext context, AmpereOp x, int raPairBit, int urLo, int extraOffset)
        {
            bool uniformForm = x.Bit(91);
            bool raPair = uniformForm ? x.Bit(raPairBit) : x.Bit(72);
            int offset = (int)x.SignedField(40, 64) + extraOffset;

            Operand lo = Register(x.Ra, RegisterType.Gpr);
            Operand hi = raPair && x.Ra != RegisterConsts.RegisterZeroIndex ? Register(x.Ra + 1, RegisterType.Gpr) : Const(0);

            if (uniformForm)
            {
                int ur = x.Bits(urLo, urLo + 6);

                if (ur != RegisterConsts.UniformZeroIndex)
                {
                    Operand uLo = Register(ur, RegisterType.Uniform);
                    Operand uHi = x.Bit(72) ? Register(ur + 1, RegisterType.Uniform) : Const(0);

                    Operand sum = context.IAdd(uLo, lo);
                    Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sum, uLo), Const(1), Const(0));

                    hi = context.IAdd(context.IAdd(uHi, hi), carry);
                    lo = sum;
                }
            }

            if (offset != 0)
            {
                Operand sum = context.IAdd(lo, Const(offset));
                Operand carry = offset > 0
                    ? context.ConditionalSelect(context.ICompareLessUnsigned(sum, lo), Const(1), Const(0))
                    : context.ConditionalSelect(context.ICompareGreaterUnsigned(sum, lo), Const(-1), Const(0));

                hi = context.IAdd(hi, carry);
                lo = sum;
            }

            return (lo, hi);
        }

        // LDG Rd, [Ra + UR + offset]: size 73..76; the uniform form (0x981) loads only where its predicate (64..67, stored as 7 - P; 67 inverts) holds,
        // and is zero elsewhere
        public static void ALdg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x.Bits(73, 76);
            int count = GetVectorCount(size);
            StorageKind storageKind = GetStorageKind(size);

            Operand pred = null;

            if (x.Opcode == 0x981)
            {
                int p = 7 - x.Bits(64, 67);

                if (p != RegisterConsts.PredicateTrueIndex || x.Bit(67))
                {
                    Operand pr = Register(p, RegisterType.Predicate);
                    pred = x.Bit(67) ? context.BitwiseNot(pr) : pr;
                }
            }

            Operand lblSkip = null;

            if (pred != null)
            {
                for (int index = 0; index < count && x.Rd + index < RegisterConsts.RegisterZeroIndex; index++)
                {
                    context.Copy(Register(x.Rd + index, RegisterType.Gpr), Const(0));
                }

                lblSkip = Label();
                context.BranchIfFalse(lblSkip, pred);
            }

            // The address is read once, before any result is written (the destination may be the address register: LDG.128 R0, [R0.64])
            (Operand[] los, Operand[] his) = (new Operand[count], new Operand[count]);

            for (int index = 0; index < count; index++)
            {
                (los[index], his[index]) = AGlobalAddress(context, x, 90, 32, index * 4);
            }

            for (int index = 0; index < count; index++)
            {
                if (x.Rd + index >= RegisterConsts.RegisterZeroIndex)
                {
                    break;
                }

                context.Copy(Register(x.Rd + index, RegisterType.Gpr), context.Load(storageKind, los[index], his[index]));
            }

            if (lblSkip != null)
            {
                context.MarkLabel(lblSkip);
            }
        }

        // STG [Ra + UR + offset], Rb: UR at 64 in the uniform form (0x986)
        public static void AStg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x.Bits(73, 76);

            if (size > LsSize.B128)
            {
                throw AFail(context, $"STG size {size}");
            }

            int count = GetVectorCount(size);
            StorageKind storageKind = GetStorageKind(size);

            for (int index = 0; index < count; index++)
            {
                bool isRz = x.Rb + index >= RegisterConsts.RegisterZeroIndex;
                Operand value = isRz ? Const(0) : Register(x.Rb + index, RegisterType.Gpr);

                (Operand lo, Operand hi) = AGlobalAddress(context, x, 90, 64, index * 4);

                context.Store(storageKind, lo, hi, value);
            }
        }

        // RED [Ra + UR + offset], Rb: type 73..76, operation 87..90
        public static void ARed(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand lo, Operand hi) = AGlobalAddress(context, x, 90, 64, 0);

            EmitAtomicOp(context, StorageKind.GlobalMemory, (AtomOp)x.Bits(87, 90), (AtomSize)x.Bits(73, 76), lo, hi, Register(x.Rb, RegisterType.Gpr));
        }

        // ATOMG Rd, [Ra + UR + offset], Rb: operation 87..91; the uniform form keeps Ra's pair bit at 70
        public static void AAtomg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand lo, Operand hi) = AGlobalAddress(context, x, 70, 64, 0);

            Operand res = EmitAtomicOp(context, StorageKind.GlobalMemory, (AtomOp)x.Bits(87, 91), (AtomSize)x.Bits(73, 76), lo, hi, Register(x.Rb, RegisterType.Gpr));

            ASet(context, x, res);
        }

        // ATOMS Rd, [Ra + UR + offset], Rb: as ATOMG (operation 87..91, size 73..76) on shared memory, the byte offset at 40..64 and UR at 64 as STS's. Compute only.
        // 0xf8c, ATOMS.POPC.INC (Pokémon Pokopia): an increment by 1 with no source register (the hardware sums the warp's lanes on one address; per lane an add of 1)
        public static void AAtoms(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand offset = context.ShiftRightU32(context.IAdd(ALocalAddress(context, x, 64), Const((int)x.SignedField(40, 64))), Const(2));
            bool inc = x.Opcode == 0xf8c;
            Operand value = inc ? Const(1) : x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr);
            Operand res = EmitAtomicOp(context, StorageKind.SharedMemory, inc ? AtomOp.Add : (AtomOp)x.Bits(87, 91), inc ? AtomSize.U32 : (AtomSize)x.Bits(73, 76), Const(context.ResourceManager.SharedMemoryId), offset, value);

            ASet(context, x, res);
        }

        // MEMBAR: the order of this thread's memory operations
        public static void AMembar(EmitterContext context)
        {
            context.MemoryBarrier();
        }

        // ERRBAR, after a MEMBAR: waits for errors of earlier memory operations to be reported, which here is nothing to do
        public static void AErrbar(EmitterContext context)
        {
        }

        // ATOMS.CAS Rd, [Ra + offset], Rb (expected), Rc (new value): Rd = 1 when the swap happened, not the old value (Fire Emblem Fortune's Weave's only use is a retry loop for a float add that repeats until Rd == 1).
        // Only the 32-bit form (operation 3 at 87..91, size 0) occurs
        public static void AAtomsCas(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand ra = x.Ra >= RegisterConsts.RegisterZeroIndex ? Const(0) : AStride(context, x, Register(x.Ra, RegisterType.Gpr));
            Operand offset = context.ShiftRightU32(context.IAdd(ra, Const((int)x.SignedField(40, 64))), Const(2));
            Operand expected = context.Copy(x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr));
            Operand value = x.Rc >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rc, RegisterType.Gpr);
            Operand old = context.AtomicCompareAndSwap(StorageKind.SharedMemory, Const(context.ResourceManager.SharedMemoryId), offset, expected, value);

            ASet(context, x, context.ConditionalSelect(context.ICompareEqual(old, expected), Const(1), Const(0)));
        }

        // ---- local and shared memory ----

        // A shared or local address register's stride, bits 78..79: it counts elements of 1, 4, 8 or 16 bytes (Fire Emblem Fortune's Weave has 1 on all its 32-bit and 2 on all its 64-bit accesses with a
        // register, whose code bounds that register by the element count; 3 by the pattern). Others, read unscaled, land on the wrong words:
        // - Pokémon Pokopia's 64-bin histogram (e4ec13f79f9e0bd4) increments [R3.X4] with R3 = 0..63 in 256 bytes of shared memory (1); its 64-bit [Ra.X8] reads at +0 and +8 (2).
        // - Mario Kart World's froxel shaders (03923de3c507135e and 35 variants) keep a float4 a thread at [R.X16 + 0x500] (3): overlapping, the fog volume came out as 4x4-tile
        //   garbage and the race picture as rainbow bands by depth slice. f94552a2a2bff505 stores 8 bytes at [R9.X16] and 4 at [R9.X16 + 8] (3, a 12-byte record a thread).
        // - Donkey Kong Bananza's upscaler (agl_ndsr.pck, 7629d857a0a93833) stores 8 bytes at [i.X4 + 0x900] (1), i = row * 40 + column * 2: a 20 x 32 tile of 8-byte
        //   elements, 5,120 bytes, in its 9,984 of shared memory (scaled by 8 it would run past them).
        private static Operand AStride(EmitterContext context, AmpereOp x, Operand ra)
        {
            int stride = x.Bits(78, 80);

            return stride == 0 ? ra : context.ShiftLeft(ra, Const(stride + 1));
        }

        // The 32-bit address of a local / shared access: Ra by its stride, plus the uniform register (at urLo in the uniform form, bit 91)
        private static Operand ALocalAddress(EmitterContext context, AmpereOp x, int urLo)
        {
            Operand addr = x.Ra == RegisterConsts.RegisterZeroIndex ? Const(0) : AStride(context, x, Register(x.Ra, RegisterType.Gpr));

            if (x.Bit(91) && x.Bits(urLo, urLo + 6) != RegisterConsts.UniformZeroIndex)
            {
                addr = context.IAdd(addr, Register(x.Bits(urLo, urLo + 6), RegisterType.Uniform));
            }

            return addr;
        }

        // LDL / LDS Rd, [Ra + UR + offset]: UR at 32, size 73..76, offset 40..64
        public static void ALdLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            StorageKind kind = x.Opcode == AmpereOpcodes.Ldl ? StorageKind.LocalMemory : StorageKind.SharedMemory;

            EmitLoad(context, kind, (LsSize2)x.Bits(73, 76), ALocalAddress(context, x, 32), x.Rd, (int)x.SignedField(40, 64));
        }

        // STL / STS [Ra + UR + offset], Rb: UR at 64
        public static void AStLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            StorageKind kind = x.Opcode is 0x387 or 0x987 ? StorageKind.LocalMemory : StorageKind.SharedMemory;

            EmitStore(context, kind, (LsSize2)x.Bits(73, 76), ALocalAddress(context, x, 64), x.Rb, (int)x.SignedField(40, 64));
        }

        // ---- surfaces ----

        // SULD Rd, [Ra] (0xb97 formatted with a channel mask at 72..76, 0xb99 binary with a size at 73..76) and SUST [Ra], Rb (0xb9b, 0xb9d): the handle as
        // a texture's (40..59), the dimension at 61..64 (1D, 1D buffer, 1D array, 2D, 2D array, 3D: Maxwell's SuDim)
        public static void ASurface(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (int handle, int cbufSlot) = ATexHandle(context, x);
            bool formatted = x.Opcode is 0xb97 or 0xb9b;
            SuDim dim = (SuDim)x.Bits(61, 64);
            SuSize size = formatted ? 0 : (SuSize)x.Bits(73, 76);
            SuRgba mask = formatted ? (SuRgba)x.Bits(72, 76) : 0;

            if (x.Opcode is 0xb97 or 0xb99)
            {
                EmitSuld(context, CacheOpLd.Ca, dim, size, handle, mask, x.Ra, x.Rd, 0, formatted, false, false, cbufSlot);
            }
            else
            {
                EmitSust(context, CacheOpSt.Wb, dim, size, handle, mask, x.Ra, x.Rb, 0, formatted, false, false, cbufSlot);
            }
        }

        // SUATOM Rd, [Ra], Rb (0x593): an atomic on an image through a handle, laid out as SUST's (handle 40..59, dimension 61..64) and as ATOMG's (operation
        // 87..91, size 73..76, Pu 81..83). Donkey Kong Bananza's append compute shaders take a counter with it (SUATOM.ADD.U32 on a 1D image, the old value
        // shuffled to the warp) before their SUST. Another handle bank or a predicate result is not met in a game: refused.
        public static void ASuatom(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (int handle, int cbufSlot) = ATexHandle(context, x);

            if (cbufSlot != TextureOperation.DefaultCbufSlot)
            {
                throw AFail(context, $"SUATOM handle in bank {cbufSlot}");
            }

            if (x.Bits(81, 84) != 7)
            {
                throw AFail(context, "SUATOM with a predicate result");
            }

            EmitSuatom(context, (SuDim)x.Bits(61, 64), 0, (SuatomOp)x.Bits(87, 91), (SuatomSize)x.Bits(73, 76), handle, x.Ra, x.Rb, 0, x.Rd, false, isBindless: false, compareAndSwap: false);
        }

        // SURED [Ra], Rb (0x59f): SUATOM without the old value (a reduction), its fields laid out as SUATOM's. Mario Kart World's compute shader ab77eaee44cfef88 counts
        // into a 2D image with SURED.ADD.U32; untranslated, the shader was missing and its dispatches skipped
        public static void ASured(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (int handle, int cbufSlot) = ATexHandle(context, x);

            if (cbufSlot != TextureOperation.DefaultCbufSlot)
            {
                throw AFail(context, $"SURED handle in bank {cbufSlot}");
            }

            EmitSured(context, (SuDim)x.Bits(61, 64), 0, (RedOp)x.Bits(87, 91), (SuatomSize)x.Bits(73, 76), handle, x.Ra, x.Rb, 0, false, isBindless: false);
        }

        // BAR.SYNC: the workgroup barrier
        public static void ABar(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(77, 80) != 0 && x.Bits(77, 80) != 4)
            {
                throw AFail(context, $"BAR mode {x.Bits(77, 80)}");
            }

            context.Barrier();
        }

        // ---- textures ----

        // Texture dimension (61..64) → Maxwell's TexDim
        private static TexDim ATexDim(EmitterContext context, int dim)
        {
            return dim switch
            {
                0 => TexDim._1d,
                4 => TexDim.Array1d,
                1 => TexDim._2d,
                5 => TexDim.Array2d,
                2 => TexDim._3d,
                3 => TexDim.Cube,
                7 => TexDim.ArrayCube,
                _ => throw AFail(context, $"texture dimension {dim}"),
            };
        }

        // The constant buffer word that holds the texture handle (40..59: word offset 0..14, bank 14..19). NVN keeps the handle table in c[2]
        // (0x20 + 8 slot), the texture buffer Ryujinx assumes; NVN2 compute's handles in c[0] are mapped there (AMapC0); a handle in another bank is passed
        // with its bank.
        private static (int Handle, int CbufSlot) ATexHandle(EmitterContext context, AmpereOp x)
        {
            (int bank, int offset) = AMapC0(context, x.Bits(54, 59), x.Bits(40, 54) * 4);

            return (offset >> 2, bank == 2 ? TextureOperation.DefaultCbufSlot : bank);
        }

        // The two source blocks of a texture instruction from Ra and Rb (n0 and n1 registers). With .SCR (60) the whole sequence was spread over the two
        // registers instead (4 → 2 + 2, 3 → 2 + 1, 2 → 1 + 1; NAK try_scalarize_tex).
        private static (List<Operand> A, List<Operand> B) ATexBlocks(EmitterContext context, AmpereOp x, int n0, int n1)
        {
            List<Operand> seq = [];

            void Take(int reg, int n)
            {
                for (int k = 0; k < n; k++)
                {
                    seq.Add(reg + k >= RegisterConsts.RegisterZeroIndex ? Const(0) : context.Copy(Register(reg + k, RegisterType.Gpr)));
                }
            }

            if (x.Bit(60))
            {
                int total = n0 + n1;

                if (total > 4)
                {
                    throw AFail(context, "texture .SCR with more than four sources");
                }

                int k0 = total >= 3 ? 2 : 1;

                Take(x.Ra, System.Math.Min(k0, total));
                Take(x.Rb, total - System.Math.Min(k0, total));
            }
            else
            {
                Take(x.Ra, n0);
                Take(x.Rb, n1);
            }

            return (seq.GetRange(0, n0), seq.GetRange(n0, seq.Count - n0));
        }

        // The results (mask 72..76) of a texture instruction to their registers: in order to Rd, Rd + 1, ...; with a second destination (64..72) the first
        // two to Rd and the others to it. .F16 (79): two results a register, packed as halves (the first two to Rd, the others to the second destination or
        // Rd + 1).
        private static void ATexResults(EmitterContext context, AmpereOp x, Operand[] results, bool f16)
        {
            int d0 = x.Rd;
            int d1 = x.Rc;
            bool split = d1 != RegisterConsts.RegisterZeroIndex;

            void Write(int reg, Operand value)
            {
                if (reg < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(reg, RegisterType.Gpr), value);
                }
            }

            if (d0 == RegisterConsts.RegisterZeroIndex)
            {
                return;
            }

            if (f16)
            {
                for (int pair = 0; pair * 2 < results.Length; pair++)
                {
                    Operand lo = results[pair * 2];
                    Operand hi = pair * 2 + 1 < results.Length ? results[pair * 2 + 1] : ConstF(0);
                    Write(pair == 0 ? d0 : split ? d1 : d0 + 1, context.PackHalf2x16(lo, hi));
                }

                return;
            }

            for (int i = 0; i < results.Length; i++)
            {
                Write(!split || i < 2 ? d0 + i : d1 + i - 2, results[i]);
            }
        }

        // TEX, TLD, TLD4, TMML, TXD. Source blocks (NAK nak_nir_lower_tex, SM70): A = array index, coordinates (TXD: coordinates, then the array index
        // with the offsets in its high half); B = LOD / bias, offset, depth reference, sample (TXD: the derivatives, x and y interleaved). The results are
        // read into locals, then written in Ampere's layout.
        public static void ATex(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int opcode = x.Opcode;

            // TEX through a handle in a register, not named by its offset: the compiler's form for a handle it read itself, usually from a table at a computed offset. BindlessElimination takes the
            // handle from there, as for Maxwell's TEX.B. The other fields are 0xb60's. 0xf60: a uniform register at 40..46. 0x361 (bit 59 set): a register, the first of the second source block,
            // before the LOD (a census of Fire Emblem Fortune's Weave's 3,327: of the sources that an LDC just wrote, always that place, with and without .SCR). Rc stays the second destination in both
            Operand boundHandle = null;
            bool handleInB = opcode == 0x361;
            bool bindless = handleInB || opcode == 0xf60;

            if (opcode == 0xf60)
            {
                int ur = x.Bits(40, 46);

                boundHandle = ur == RegisterConsts.UniformZeroIndex ? Const(0) : context.Copy(Register(ur, RegisterType.Uniform));
            }

            if (bindless)
            {
                opcode = 0xb60;
            }

            TexDim dim = ATexDim(context, x.Bits(61, 64));
            SamplerType type = ConvertSamplerType(dim);
            bool isArray = type.HasFlag(SamplerType.Array);
            int coords = type.Dimensions;
            int mask = x.Bits(72, 76);
            bool f16 = x.Bit(79) && opcode != 0xb6c;

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex && opcode != 0xb69)
            {
                throw AFail(context, "texture residency predicate");
            }

            TextureFlags flags = TextureFlags.None;
            int lod = 0;
            bool offset = false, depth = false, ms = false, ptp = false;
            int n1 = 0;

            switch (opcode)
            {
                case 0xb60: // TEX: lod 87..90 (0 auto, 1 zero, 2 bias, 3 level), AOFFI 76, depth reference 78
                    lod = x.Bits(87, 90);
                    offset = x.Bit(76);
                    depth = x.Bit(78);
                    if (lod > 3)
                    {
                        throw AFail(context, $"TEX lod mode {lod}");
                    }

                    n1 = (lod >= 2 ? 1 : 0) + (offset ? 1 : 0) + (depth ? 1 : 0);
                    break;
                case 0xb66: // TLD: lod 87..90 (1 zero, 3 level), AOFFI 76, multisample 78
                    lod = x.Bits(87, 90);
                    offset = x.Bit(76);
                    ms = x.Bit(78);
                    if (lod is not 1 and not 3)
                    {
                        throw AFail(context, $"TLD lod mode {lod}");
                    }

                    flags |= TextureFlags.IntCoords;
                    n1 = (lod == 3 ? 1 : 0) + (offset ? 1 : 0) + (ms ? 1 : 0);
                    break;
                case 0xb63: // TLD4: offset mode 76..78 (1 AOFFI, 2 per texel), depth reference 78, component 87..89
                    int offMode = x.Bits(76, 78);
                    depth = x.Bit(78);
                    if (offMode == 3)
                    {
                        throw AFail(context, "TLD4 offset mode 3");
                    }

                    offset = offMode != 0;
                    ptp = offMode == 2;
                    flags |= TextureFlags.Gather;
                    n1 = (offMode == 2 ? 2 : offMode) + (depth ? 1 : 0);
                    break;
                case 0xb6c: // TXD: AOFFI 76
                    offset = x.Bit(76);
                    flags |= TextureFlags.Derivatives;
                    break;
            }

            (int handle, int cbufSlot) = bindless ? (0, TextureOperation.DefaultCbufSlot) : ATexHandle(context, x);
            List<Operand> sources = [];
            Operand arrayIndex = null;

            if (bindless)
            {
                flags |= TextureFlags.Bindless;
            }

            if (opcode == 0xb6c)
            {
                int n0 = coords + (isArray || offset ? 1 : 0);
                (List<Operand> a, List<Operand> b) = ATexBlocks(context, x, n0, 2 * coords);

                sources.AddRange(a.GetRange(0, coords));

                if (isArray)
                {
                    arrayIndex = context.BitwiseAnd(a[coords], Const(0xffff));
                }

                if (isArray)
                {
                    sources.Add(arrayIndex);
                }

                sources.AddRange(b);

                if (offset)
                {
                    for (int i = 0; i < coords; i++)
                    {
                        sources.Add(context.BitfieldExtractS32(a[coords], Const(16 + i * 4), Const(4)));
                    }

                    flags |= TextureFlags.Offset;
                }
            }
            else
            {
                int n0 = coords + (isArray ? 1 : 0);
                (List<Operand> a, List<Operand> b) = ATexBlocks(context, x, n0, n1 + (handleInB ? 1 : 0));
                int ia = 0, ib = 0;

                if (handleInB)
                {
                    boundHandle = b[ib++];
                }

                if (bindless)
                {
                    sources.Add(boundHandle);
                }

                if (isArray)
                {
                    arrayIndex = a[ia++];
                }

                for (int i = 0; i < coords; i++)
                {
                    sources.Add(a[ia++]);
                }

                if (isArray)
                {
                    sources.Add(arrayIndex);
                }

                Operand lodValue = null;

                if ((opcode == 0xb60 && lod >= 2) || (opcode == 0xb66 && lod == 3))
                {
                    lodValue = b[ib++];
                }

                Operand off0 = offset ? b[ib++] : null;
                Operand off1 = ptp ? b[ib++] : null;

                if (depth)
                {
                    sources.Add(b[ib++]);
                    type |= SamplerType.Shadow;
                }

                if (opcode == 0xb60 || opcode == 0xb66)
                {
                    if (!ms && (lod == 1 || lod == 3))
                    {
                        sources.Add(lodValue ?? (opcode == 0xb66 ? Const(0) : ConstF(0)));
                        flags |= TextureFlags.LodLevel;
                    }
                }

                if (offset && opcode != 0xb63)
                {
                    for (int i = 0; i < coords; i++)
                    {
                        sources.Add(context.BitfieldExtractS32(off0, Const(i * 4), Const(4)));
                    }

                    flags |= TextureFlags.Offset;
                }
                else if (offset)
                {
                    int texels = ptp ? 4 : 1;

                    for (int i = 0; i < coords * texels; i++)
                    {
                        Operand packed = (i >> 2 & 1) == 0 ? off0 : off1;
                        sources.Add(context.BitfieldExtractS32(packed, Const((i & 3) * 8), Const(6)));
                    }

                    flags |= ptp ? TextureFlags.Offsets : TextureFlags.Offset;
                }

                if (opcode == 0xb60 && lod == 2)
                {
                    sources.Add(lodValue);
                    flags |= TextureFlags.LodBias;
                }

                if (ms)
                {
                    sources.Add(b[ib++]);
                    type |= SamplerType.Multisample;
                }

                if (opcode == 0xb63 && !depth)
                {
                    sources.Add(Const(x.Bits(87, 89)));
                }

                if (opcode == 0xb66 && type == SamplerType.Texture1D && lod == 1 && !offset)
                {
                    if (context.TranslatorContext.GpuAccessor.QuerySamplerType(handle) == SamplerType.TextureBuffer)
                    {
                        type = SamplerType.TextureBuffer;
                        sources.RemoveAt(sources.Count - 1);
                        flags &= ~TextureFlags.LodLevel;
                    }
                    else if (Sample1DAs2D)
                    {
                        // nxnative: as Maxwell's TLDS Texture1DLodZero, the bound descriptor decides between a 1D texture and a texture buffer, and with no type answer
                        // the fetch is 2D at (x & 0x3FFF, x >> 14): a 1D texture (one row on the host) reads (x, 0) as before, and the host binds a texture buffer in
                        // the slot as 16384-texel rows (nvn_mtl.m tbuf_as_2d). Pokémon Pokopia's KTGL reads its light and decal lists this way (NVN target 10 in slots
                        // the shaders declare 1D); bound as is, the 1D slot took a buffer view and the GPU faulted
                        Operand x0 = sources[0];
                        sources[0] = context.BitwiseAnd(x0, Const(0x3FFF));
                        sources.Insert(1, context.ShiftRightU32(x0, Const(14)));
                        context.TranslatorContext.GpuAccessor.RecordTexelFetch1D(handle);
                        type = SamplerType.Texture2D;
                    }
                }
            }

            int count = BitOperations.PopCount((uint)mask);
            Operand[] results = new Operand[count];

            for (int i = 0; i < count; i++)
            {
                results[i] = Local();
            }

            if (opcode == 0xb69)
            {
                ATmml(context, type, flags, handle, cbufSlot, mask, results, sources.ToArray());
            }
            else
            {
                SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureSample,
                    type,
                    TextureFormat.Unknown,
                    flags,
                    cbufSlot,
                    handle);

                context.TextureSample(type, flags, setAndBinding, mask, results, sources.ToArray());
            }

            ATexResults(context, x, results, f16);
        }

        // TMML: the LOD a sample would use, x and y in 8.8 fixed point (as Maxwell's)
        private static void ATmml(EmitterContext context, SamplerType type, TextureFlags flags, int handle, int cbufSlot, int mask, Operand[] results, Operand[] sources)
        {
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.Lod, type, TextureFormat.Unknown, flags, cbufSlot, handle);
            int r = 0;

            for (int comp = 0; comp < 4; comp++)
            {
                if ((mask & (1 << comp)) == 0)
                {
                    continue;
                }

                if (comp >= 2)
                {
                    context.Copy(results[r++], Const(0));
                    continue;
                }

                Operand res = context.Lod(type, flags, setAndBinding, comp ^ 1, sources);
                context.Copy(results[r++], context.FP32ConvertToS32(context.FPMultiply(res, ConstF(256.0f))));
            }
        }

        // TXQ Rd, Ra: query 62..64 (dimensions with Ra the LOD, texture type); results as the other texture instructions
        public static void ATxq(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int query = x.Bits(62, 64);
            int mask = x.Bits(72, 76);
            // 0x370 is to 0xb6f as 0x361 to 0xb60: the handle in a register, Ra, with the LOD after it (Fire Emblem Fortune's Weave's lighting: MOV R1c, R64; MOV R1d, RZ; TXQ R1d, R1c). The shape
            // has no field here and the size query does not depend on it, so 2D, as Maxwell's bindless TXQ
            bool bindless = x.Opcode == 0x370;
            (int handle, int cbufSlot) = bindless ? (0, TextureOperation.DefaultCbufSlot) : ATexHandle(context, x);
            SamplerType type = bindless ? SamplerType.Texture2D : context.TranslatorContext.GpuAccessor.QuerySamplerType(handle);
            TextureFlags flags = bindless ? TextureFlags.Bindless : TextureFlags.None;

            int count = BitOperations.PopCount((uint)mask);
            Operand[] results = new Operand[count];

            for (int i = 0; i < count; i++)
            {
                results[i] = Local();
            }

            Operand Src(int reg) => reg >= RegisterConsts.RegisterZeroIndex ? Const(0) : context.Copy(Register(reg, RegisterType.Gpr));

            Operand[] sources = bindless ? [Src(x.Ra), Src(x.Ra + 1)] : [Src(x.Ra)];

            if (query == 0)
            {
                SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureQuerySize, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, handle);
                int r = 0;

                for (int comp = 0; comp < 4; comp++)
                {
                    if ((mask & (1 << comp)) != 0)
                    {
                        context.Copy(results[r++], context.TextureQuerySize(type, flags, setAndBinding, comp, sources));
                    }
                }
            }
            else if (query == 1)
            {
                SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureQuerySamples, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, handle);
                int r = 0;

                for (int comp = 0; comp < 4; comp++)
                {
                    if ((mask & (1 << comp)) != 0)
                    {
                        context.Copy(results[r++], comp == 2 ? context.TextureQuerySamples(type, flags, setAndBinding, sources) : Const(0));
                    }
                }
            }
            else
            {
                throw AFail(context, $"TXQ query {query}");
            }

            ATexResults(context, x, results, false);
        }
    }
}

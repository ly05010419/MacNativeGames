using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class BindlessToArray
    {
        private const int NvnTextureBufferIndex = 2;
        private const int HardcodedArrayLengthOgl = 4;

        // 1 and 0 elements are not considered arrays anymore.
        public const int MinimumArrayLength = 2;

        public static void RunPassOgl(BasicBlock block, ResourceManager resourceManager)
        {
            // We can turn a bindless texture access into a indexed access,
            // as long the following conditions are true:
            // - The handle is loaded using a LDC instruction.
            // - The handle is loaded from the constant buffer with the handles (CB2 for NVN).
            // - The load has a constant offset.
            // The base offset of the array of handles on the constant buffer is the constant offset.
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                if (texOp.GetSource(0).AsgOp is not Operation handleAsgOp)
                {
                    continue;
                }

                if (handleAsgOp.Inst != Instruction.Load ||
                    handleAsgOp.StorageKind != StorageKind.ConstantBuffer ||
                    handleAsgOp.SourcesCount != 4)
                {
                    continue;
                }

                Operand ldcSrc0 = handleAsgOp.GetSource(0);

                if (ldcSrc0.Type != OperandType.Constant ||
                    !resourceManager.TryGetConstantBufferSlot(ldcSrc0.Value, out int src0CbufSlot) ||
                    src0CbufSlot != NvnTextureBufferIndex)
                {
                    continue;
                }

                Operand ldcSrc1 = handleAsgOp.GetSource(1);

                // We expect field index 0 to be accessed.
                if (ldcSrc1.Type != OperandType.Constant || ldcSrc1.Value != 0)
                {
                    continue;
                }

                Operand ldcSrc2 = handleAsgOp.GetSource(2);

                // FIXME: This is missing some checks, for example, a check to ensure that the shift value is 2.
                // Might be not worth fixing since if that doesn't kick in, the result will be no texture
                // to access anyway which is also wrong.
                // Plus this whole transform is fundamentally flawed as-is since we have no way to know the array size.
                // Eventually, this should be entirely removed in favor of a implementation that supports true bindless
                // texture access.
                if (ldcSrc2.AsgOp is not Operation shrOp || shrOp.Inst != Instruction.ShiftRightU32)
                {
                    continue;
                }

                if (shrOp.GetSource(0).AsgOp is not Operation shrOp2 || shrOp2.Inst != Instruction.ShiftRightU32)
                {
                    continue;
                }

                if (shrOp2.GetSource(0).AsgOp is not Operation addOp || addOp.Inst != Instruction.Add)
                {
                    continue;
                }

                Operand addSrc1 = addOp.GetSource(1);

                if (addSrc1.Type != OperandType.Constant)
                {
                    continue;
                }

                TurnIntoArray(resourceManager, texOp, NvnTextureBufferIndex, addSrc1.Value / 4, HardcodedArrayLengthOgl);

                Operand index = Local();

                Operand source = addOp.GetSource(0);

                Operation shrBy3 = new(Instruction.ShiftRightU32, index, source, Const(3));

                block.Operations.AddBefore(node, shrBy3);

                texOp.SetSource(0, index);
            }
        }

        public static void RunPass(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor)
        {
            // We can turn a bindless texture access into a indexed access,
            // as long the following conditions are true:
            // - The handle is loaded using a LDC instruction.
            // - The handle is loaded from the constant buffer with the handles (CB2 for NVN).
            // - The load has a constant offset.
            // The base offset of the array of handles on the constant buffer is the constant offset.
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                Operand bindlessHandle = Utils.FindLastOperation(texOp.GetSource(0), block);

                if (bindlessHandle.AsgOp is not Operation handleAsgOp)
                {
                    continue;
                }

                int secondaryCbufSlot = 0;
                int secondaryCbufOffset = 0;
                bool hasSecondaryHandle = false;

                if (handleAsgOp.Inst == Instruction.BitwiseOr)
                {
                    Operand src0 = Utils.FindLastOperation(handleAsgOp.GetSource(0), block);
                    Operand src1 = Utils.FindLastOperation(handleAsgOp.GetSource(1), block);

                    if (src0.Type == OperandType.ConstantBuffer && src1.AsgOp is Operation)
                    {
                        handleAsgOp = src1.AsgOp as Operation;
                        secondaryCbufSlot = src0.GetCbufSlot();
                        secondaryCbufOffset = src0.GetCbufOffset();
                        hasSecondaryHandle = true;
                    }
                    else if (src0.AsgOp is Operation && src1.Type == OperandType.ConstantBuffer)
                    {
                        handleAsgOp = src0.AsgOp as Operation;
                        secondaryCbufSlot = src1.GetCbufSlot();
                        secondaryCbufOffset = src1.GetCbufOffset();
                        hasSecondaryHandle = true;
                    }
                }

                if (handleAsgOp.Inst != Instruction.Load ||
                    handleAsgOp.StorageKind != StorageKind.ConstantBuffer ||
                    handleAsgOp.SourcesCount != 4)
                {
                    continue;
                }

                Operand ldcSrc0 = handleAsgOp.GetSource(0);

                if (ldcSrc0.Type != OperandType.Constant ||
                    !resourceManager.TryGetConstantBufferSlot(ldcSrc0.Value, out int src0CbufSlot))
                {
                    continue;
                }

                Operand ldcSrc1 = handleAsgOp.GetSource(1);

                // We expect field index 0 to be accessed.
                if (ldcSrc1.Type != OperandType.Constant || ldcSrc1.Value != 0)
                {
                    continue;
                }

                Operand ldcVecIndex = handleAsgOp.GetSource(2);
                Operand ldcElemIndex = handleAsgOp.GetSource(3);

                if (ldcVecIndex.Type != OperandType.LocalVariable || ldcElemIndex.Type != OperandType.LocalVariable)
                {
                    continue;
                }

                int cbufSlot;
                int handleIndex;

                if (hasSecondaryHandle)
                {
                    cbufSlot = TextureHandle.PackSlots(src0CbufSlot, secondaryCbufSlot);
                    handleIndex = TextureHandle.PackOffsets(0, secondaryCbufOffset, TextureHandleType.SeparateSamplerHandle);
                }
                else
                {
                    cbufSlot = src0CbufSlot;
                    handleIndex = 0;
                }

                int length = Math.Max(MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromBuffer(src0CbufSlot));

                // nxnative: handles at a computed index past a constant base, the base beyond the array's length: every read would clamp to its last element, which is no handle. Fire Emblem Fortune's
                // Weave's character shaders read their cube maps as c[14][8 i + 0x340] (LDC after a shift of 3). The array starts at the base instead (the host fills element k from base + 8k), so
                // the index is that of the handle within the table. A base inside the array keeps the array at 0, as before
                if (!hasSecondaryHandle && TryArrayBase(ldcVecIndex, out Operand tableIndex, out int baseOffset) && baseOffset / 8 >= length)
                {
                    TurnIntoArray(resourceManager, texOp, cbufSlot, baseOffset / 4, length);

                    Operand element = Local();
                    Operand elementMin = Local();

                    block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, element, tableIndex, Const(3)));
                    block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, elementMin, element, Const(length - 1)));

                    texOp.SetSource(0, elementMin);

                    continue;
                }

                TurnIntoArray(resourceManager, texOp, cbufSlot, handleIndex, length);

                Operand vecIndex = Local();
                Operand elemIndex = Local();
                Operand index = Local();
                Operand indexMin = Local();

                block.Operations.AddBefore(node, new Operation(Instruction.ShiftLeft, vecIndex, ldcVecIndex, Const(1)));
                block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, elemIndex, ldcElemIndex, Const(1)));
                block.Operations.AddBefore(node, new Operation(Instruction.Add, index, vecIndex, elemIndex));
                block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, indexMin, index, Const(length - 1)));

                texOp.SetSource(0, indexMin);
            }
        }

        // nxnative: the vector index of a constant buffer load whose address is (index + a constant byte offset) >> 2 >> 2: that index and offset
        private static bool TryArrayBase(Operand vecIndex, out Operand index, out int byteOffset)
        {
            index = null;
            byteOffset = 0;

            static bool ShiftBy2(Operand o, out Operand source)
            {
                source = null;

                if (o.AsgOp is Operation op && op.Inst == Instruction.ShiftRightU32 && op.GetSource(1).Type == OperandType.Constant && op.GetSource(1).Value == 2)
                {
                    source = op.GetSource(0);

                    return true;
                }

                return false;
            }

            if (!ShiftBy2(vecIndex, out Operand word))
            {
                return false;
            }

            if (word.AsgOp is Operation plus0 && plus0.Inst == Instruction.Add && plus0.GetSource(1).Type == OperandType.Constant && plus0.GetSource(1).Value == 0)
            {
                word = plus0.GetSource(0);
            }

            if (!ShiftBy2(word, out Operand address) || address.AsgOp is not Operation add || add.Inst != Instruction.Add || add.GetSource(1).Type != OperandType.Constant)
            {
                return false;
            }

            index = add.GetSource(0);
            byteOffset = add.GetSource(1).Value;

            return byteOffset > 0 && (byteOffset & 7) == 0;
        }

        private static void TurnIntoArray(ResourceManager resourceManager, TextureOperation texOp, int cbufSlot, int handleIndex, int length)
        {
            SetBindingPair setAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                cbufSlot,
                handleIndex,
                length);

            texOp.TurnIntoArray(setAndBinding);
        }
    }
}

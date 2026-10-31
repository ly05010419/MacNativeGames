using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class XmadOptimizer
    {
        public static void RunPass(BasicBlock block)
        {
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not Operation operation)
                {
                    continue;
                }

                if (TryMatchXmadPattern(operation, out Operand x, out Operand y, out Operand addend))
                {
                    LinkedListNode<INode> nextNode;

                    if (addend != null)
                    {
                        Operand temp = OperandHelper.Local();

                        nextNode = block.Operations.AddAfter(node, new Operation(Instruction.Multiply, temp, x, y));
                        nextNode = block.Operations.AddAfter(nextNode, new Operation(Instruction.Add, operation.Dest, temp, addend));
                    }
                    else
                    {
                        nextNode = block.Operations.AddAfter(node, new Operation(Instruction.Multiply, operation.Dest, x, y));
                    }

                    Optimizer.RemoveNode(block, node);
                    node = nextNode;
                }
            }
        }

        private static bool TryMatchXmadPattern(Operation operation, out Operand x, out Operand y, out Operand addend)
        {
            return TryMatchXmad32x32Pattern(operation, out x, out y, out addend) ||
                   TryMatchXmad32x16Pattern(operation, out x, out y, out addend);
        }

        private static bool TryMatchXmad32x32Pattern(Operation operation, out Operand x, out Operand y, out Operand addend)
        {
            x = null;
            y = null;
            addend = null;

            if (operation.Inst != Instruction.Add)
            {
                return false;
            }

            Operand src1 = operation.GetSource(0);
            Operand src2 = operation.GetSource(1);

            if (src2.AsgOp is not Operation addOp || addOp.Inst != Instruction.Add)
            {
                return false;
            }

            Operand lowTimesLowResult = GetCopySource(addOp.GetSource(0));

            if (lowTimesLowResult.AsgOp is not Operation lowTimesLowOp)
            {
                return false;
            }

            if (!TryMatchLowTimesLow(lowTimesLowOp, out x, out y, out addend))
            {
                return false;
            }

            Operand lowTimesHighResult = GetCopySource(GetShifted16Source(addOp.GetSource(1), Instruction.ShiftLeft));

            if (lowTimesHighResult.AsgOp is not Operation lowTimesHighOp)
            {
                return false;
            }

            if (!TryMatchLowTimesHigh(lowTimesHighOp, x, y))
            {
                return false;
            }

            if (src1.AsgOp is not Operation highTimesHighOp)
            {
                return false;
            }

            if (!TryMatchHighTimesHigh(highTimesHighOp, x, lowTimesHighResult))
            {
                return false;
            }

            return true;
        }

        private static bool TryMatchXmad32x16Pattern(Operation operation, out Operand x, out Operand y, out Operand addend)
        {
            x = null;
            y = null;
            addend = null;

            if (operation.Inst != Instruction.Add)
            {
                return false;
            }

            Operand src1 = operation.GetSource(0);
            Operand src2 = operation.GetSource(1);

            Operand lowTimesLowResult = GetCopySource(src2);

            if (lowTimesLowResult.AsgOp is not Operation lowTimesLowOp)
            {
                return false;
            }

            if (!TryMatchLowTimesLow(lowTimesLowOp, out x, out y, out addend))
            {
                return false;
            }

            Operand highTimesLowResult = src1;

            if (highTimesLowResult.AsgOp is not Operation highTimesLowOp)
            {
                return false;
            }

            if (!TryMatchHighTimesLow(highTimesLowOp, x, y))
            {
                return false;
            }

            return y.Type == OperandType.Constant && (ushort)y.Value == y.Value;
        }

        private static bool TryMatchLowTimesLow(Operation operation, out Operand x, out Operand y, out Operand addend)
        {
            // x = x & 0xFFFF
            // y = y & 0xFFFF
            // lowTimesLow = x * y

            x = null;
            y = null;
            addend = null;

            if (operation.Inst == Instruction.Add)
            {
                if (operation.GetSource(0).AsgOp is not Operation mulOp)
                {
                    return false;
                }

                addend = operation.GetSource(1);
                operation = mulOp;
            }

            if (operation.Inst != Instruction.Multiply)
            {
                return false;
            }

            Operand src1 = GetMasked16Source(operation.GetSource(0));
            Operand src2 = GetMasked16Source(operation.GetSource(1));

            if (src1 == null || src2 == null)
            {
                return false;
            }

            x = src1;
            y = src2;

            return true;
        }

        private static bool TryMatchLowTimesHigh(Operation operation, Operand x, Operand y)
        {
            // xLow = x & 0xFFFF
            // yHigh = y >> 16
            // lowTimesHigh = xLow * yHigh
            // result = (lowTimesHigh & 0xFFFF) | (y << 16)

            if (operation.Inst != Instruction.BitwiseOr)
            {
                return false;
            }

            Operand mulResult = GetMasked16Source(operation.GetSource(0));

            if (mulResult == null)
            {
                return false;
            }

            mulResult = GetCopySource(mulResult);

            if (mulResult.AsgOp is not Operation mulOp || mulOp.Inst != Instruction.Multiply)
            {
                return false;
            }

            if (GetMasked16Source(mulOp.GetSource(0)) != x)
            {
                return false;
            }

            if (GetShifted16Source(mulOp.GetSource(1), Instruction.ShiftRightU32) != y)
            {
                return false;
            }

            if (GetShifted16Source(operation.GetSource(1), Instruction.ShiftLeft) != y)
            {
                return false;
            }

            return true;
        }

        private static bool TryMatchHighTimesLow(Operation operation, Operand x, Operand y)
        {
            // xHigh = x >> 16
            // yLow = y & 0xFFFF
            // highTimesLow = xHigh * yLow
            // result = highTimesLow << 16

            if (operation.Inst != Instruction.ShiftLeft || !IsConst(operation.GetSource(1), 16))
            {
                return false;
            }

            Operand mulResult = operation.GetSource(0);

            if (mulResult.AsgOp is not Operation mulOp || mulOp.Inst != Instruction.Multiply)
            {
                return false;
            }

            if (GetShifted16Source(mulOp.GetSource(0), Instruction.ShiftRightU32) != x)
            {
                return false;
            }

            Operand src2 = GetMasked16Source(mulOp.GetSource(1));

            if (src2.Type != y.Type || src2.Value != y.Value)
            {
                return false;
            }

            return true;
        }

        private static bool TryMatchHighTimesHigh(Operation operation, Operand x, Operand lowTimesHighResult)
        {
            // xHigh = x >> 16
            // lowTimesHighResultHigh = lowTimesHighResult >> 16
            // highTimesHigh = xHigh * lowTimesHighResultHigh
            // result = highTimesHigh << 16

            if (operation.Inst != Instruction.ShiftLeft || !IsConst(operation.GetSource(1), 16))
            {
                return false;
            }

            Operand mulResult = operation.GetSource(0);

            if (mulResult.AsgOp is not Operation mulOp || mulOp.Inst != Instruction.Multiply)
            {
                return false;
            }

            if (GetShifted16Source(mulOp.GetSource(0), Instruction.ShiftRightU32) != x)
            {
                return false;
            }

            if (GetCopySource(GetShifted16Source(mulOp.GetSource(1), Instruction.ShiftRightU32)) != lowTimesHighResult)
            {
                return false;
            }

            return true;
        }

        private static Operand GetMasked16Source(Operand value)
        {
            if (value.AsgOp is not Operation maskOp)
            {
                return null;
            }

            if (maskOp.Inst != Instruction.BitwiseAnd || !IsConst(maskOp.GetSource(1), ushort.MaxValue))
            {
                return null;
            }

            return maskOp.GetSource(0);
        }

        private static Operand GetShifted16Source(Operand value, Instruction shiftInst)
        {
            if (value.AsgOp is not Operation shiftOp)
            {
                return null;
            }

            if (shiftOp.Inst != shiftInst || !IsConst(shiftOp.GetSource(1), 16))
            {
                return null;
            }

            return shiftOp.GetSource(0);
        }

        private static Operand GetCopySource(Operand value)
        {
            while (value.AsgOp is Operation operation && IsCopy(operation))
            {
                value = operation.GetSource(0);
            }

            return value;
        }

        private static bool IsCopy(Operation operation)
        {
            return operation.Inst == Instruction.Copy || (operation.Inst == Instruction.Add && IsConst(operation.GetSource(1), 0));
        }

        private static bool IsConst(Operand operand, int value)
        {
            return operand.Type == OperandType.Constant && operand.Value == value;
        }
    }
}

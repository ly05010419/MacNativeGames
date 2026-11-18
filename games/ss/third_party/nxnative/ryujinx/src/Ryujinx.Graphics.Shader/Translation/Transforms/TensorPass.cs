using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using System.Collections.Generic;
using System.Diagnostics;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    // nxnative: MatrixMultiplyAdd (Ampere HMMA.16816 with an f16 accumulator, InstEmitAmpereTensor.AHmma) becomes a call of the helper dmsh_hmma16816_f16, whose body the
    // shader build replaces with simdgroup_matrix code in the Metal source. Sources: the scratch shared memory id (constant), the lane, the subgroup's block offset, A (4), B (2),
    // C (2); destinations: D (2), the first as the call's result, the second through its out argument (as ShufflePass does with its valid flag)
    class TensorPass : ITransformPass
    {
        public static bool IsEnabled(IGpuAccessor gpuAccessor, ShaderStage stage, TargetLanguage targetLanguage, FeatureFlags usedFeatures)
        {
            return true;
        }

        public static LinkedListNode<INode> RunPass(TransformContext context, LinkedListNode<INode> node)
        {
            Operation operation = (Operation)node.Value;

            if (operation.Inst != Instruction.MatrixMultiplyAdd)
            {
                return node;
            }

            Operand memoryId = operation.GetSource(0);

            Debug.Assert(memoryId.Type == OperandType.Constant);

            int functionId = context.Hfm.GetOrCreateFunctionId(HelperFunctionName.Hmma16816F16, memoryId.Value);

            Operand d0 = operation.GetDest(0);
            Operand d1 = operation.GetDest(1);

            operation.Dest = null;

            Operand[] callArgs = new Operand[12];
            callArgs[0] = Const(functionId);

            for (int i = 0; i < 10; i++)
            {
                callArgs[1 + i] = operation.GetSource(1 + i);
            }

            callArgs[11] = d1;

            LinkedListNode<INode> newNode = node.List.AddBefore(node, new Operation(Instruction.Call, 0, d0, callArgs));

            Utils.DeleteNode(node, operation);

            return newNode;
        }
    }
}

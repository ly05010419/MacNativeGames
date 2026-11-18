using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;

using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        public static void Al2p(EmitterContext context)
        {
            InstAl2p op = context.GetOp<InstAl2p>();

            context.Copy(GetDest(op.Dest), context.IAdd(GetSrcReg(context, op.SrcA), Const(op.Imm11)));
        }

        public static void Ald(EmitterContext context)
        {
            InstAld op = context.GetOp<InstAld>();

            EmitAld(context, op.Dest, op.SrcA, op.SrcB, op.Imm11, (int)op.AlSize, op.P, op.O, op.Phys);
        }

        internal static void EmitAld(EmitterContext context, int dest, int srcA, int srcB, int imm11, int alSize, bool p, bool o, bool phys)
        {
            // Some of those attributes are per invocation,
            // so we should ignore any primitive vertex indexing for those.
            bool hasPrimitiveVertex = AttributeMap.HasPrimitiveVertex(context.TranslatorContext.Definitions.Stage, o) && !p;

            if (!phys)
            {
                hasPrimitiveVertex &= HasPrimitiveVertex(imm11);
            }

            Operand primVertex = hasPrimitiveVertex ? context.Copy(GetSrcReg(context, srcB)) : null;

            for (int index = 0; index < alSize + 1; index++)
            {
                Register rd = new(dest + index, RegisterType.Gpr);

                if (rd.IsRZ)
                {
                    break;
                }

                if (phys)
                {
                    Operand offset = context.ISubtract(GetSrcReg(context, srcA), Const(AttributeConsts.UserAttributeBase));
                    Operand vecIndex = context.ShiftRightU32(offset, Const(4));
                    Operand elemIndex = context.BitwiseAnd(context.ShiftRightU32(offset, Const(2)), Const(3));

                    StorageKind storageKind = o ? StorageKind.Output : StorageKind.Input;

                    context.Copy(Register(rd), context.Load(storageKind, IoVariable.UserDefined, primVertex, vecIndex, elemIndex));
                }
                else if (srcB == RegisterConsts.RegisterZeroIndex || p)
                {
                    int offset = FixedFuncToUserAttribute(context.TranslatorContext, imm11 + index * 4, o);
                    bool isOutput = o && CanLoadOutput(offset);

                    if (!p && !isOutput && TryConvertIdToIndexForVulkan(context, offset, out Operand value))
                    {
                        context.Copy(Register(rd), value);
                    }
                    else
                    {
                        value = AttributeMap.GenerateAttributeLoad(context, primVertex, offset, isOutput, p);

                        if ((!context.TranslatorContext.Definitions.SupportsScaledVertexFormats || context.VertexAsCompute) &&
                            context.TranslatorContext.Stage == ShaderStage.Vertex &&
                            !o &&
                            offset >= 0x80 &&
                            offset < 0x280)
                        {
                            // The host does not support scaled vertex formats,
                            // the emulator should use a integer format, and
                            // we compensate here inserting the conversion to float.

                            AttributeType type = context.TranslatorContext.Definitions.GetAttributeType((offset - 0x80) >> 4);

                            if (type == AttributeType.Sscaled)
                            {
                                value = context.IConvertS32ToFP32(value);
                            }
                            else if (type == AttributeType.Uscaled)
                            {
                                value = context.IConvertU32ToFP32(value);
                            }
                        }
                        else if (offset == AttributeConsts.PrimitiveId && context.TranslatorContext.Definitions.HalvePrimitiveId)
                        {
                            value = context.ShiftRightS32(value, Const(1));
                        }

                        context.Copy(Register(rd), value);
                    }
                }
                else
                {
                    int offset = FixedFuncToUserAttribute(context.TranslatorContext, imm11 + index * 4, o);
                    bool isOutput = o && CanLoadOutput(offset);

                    context.Copy(Register(rd), AttributeMap.GenerateAttributeLoad(context, primVertex, offset, isOutput, false));
                }
            }
        }

        public static void Ast(EmitterContext context)
        {
            InstAst op = context.GetOp<InstAst>();

            EmitAst(context, op.SrcA, op.SrcB, op.Imm11, (int)op.AlSize, op.P, op.Phys);
        }

        internal static void EmitAst(EmitterContext context, int srcA, int data, int imm11, int alSize, bool p, bool phys)
        {
            for (int index = 0; index < alSize + 1; index++)
            {
                if (data + index > RegisterConsts.RegisterZeroIndex)
                {
                    break;
                }

                Register rd = new(data + index, RegisterType.Gpr);

                if (phys)
                {
                    Operand offset = context.ISubtract(GetSrcReg(context, srcA), Const(AttributeConsts.UserAttributeBase));
                    Operand vecIndex = context.ShiftRightU32(offset, Const(4));
                    Operand elemIndex = context.BitwiseAnd(context.ShiftRightU32(offset, Const(2)), Const(3));
                    Operand invocationId = AttributeMap.HasInvocationId(context.TranslatorContext.Definitions.Stage, isOutput: true)
                        ? context.Load(StorageKind.Input, IoVariable.InvocationId)
                        : null;

                    context.Store(StorageKind.Output, IoVariable.UserDefined, invocationId, vecIndex, elemIndex, Register(rd));
                }
                else
                {
                    // TODO: Support indirect stores using Ra.

                    int offset = imm11 + index * 4;

                    if (!context.TranslatorContext.AttributeUsage.IsUsedOutputAttribute(offset))
                    {
                        return;
                    }

                    offset = FixedFuncToUserAttribute(context.TranslatorContext, offset, isOutput: true);
                    AttributeMap.GenerateAttributeStore(context, offset, p, Register(rd));
                }
            }
        }

        public static void Ipa(EmitterContext context)
        {
            InstIpa op = context.GetOp<InstIpa>();

            EmitIpa(context, op.Idx ? GetSrcReg(context, op.SrcA) : null, op.Imm10, op.IpaOp, op.SrcB, op.Sat, op.Dest);
        }

        // idxAddress: the whole attribute address of IPA.IDX (null otherwise)
        internal static void EmitIpa(EmitterContext context, Operand idxAddress, int imm10, IpaOp ipaOp, int srcB, bool sat, int dest)
        {
            Operand res;

            bool isFixedFunc = false;

            if (idxAddress != null)
            {
                Operand offset = context.ISubtract(idxAddress, Const(AttributeConsts.UserAttributeBase));
                Operand vecIndex = context.ShiftRightU32(offset, Const(4));
                Operand elemIndex = context.BitwiseAnd(context.ShiftRightU32(offset, Const(2)), Const(3));

                res = context.Load(StorageKind.Input, IoVariable.UserDefined, null, vecIndex, elemIndex);
                res = context.FPMultiply(res, context.Load(StorageKind.Input, IoVariable.FragmentCoord, null, Const(3)));
            }
            else
            {
                isFixedFunc = TryFixedFuncToUserAttributeIpa(context, imm10, out res);

                if (imm10 is >= AttributeConsts.UserAttributeBase and < AttributeConsts.UserAttributeEnd)
                {
                    int index = (imm10 - AttributeConsts.UserAttributeBase) >> 4;

                    if (context.TranslatorContext.Definitions.ImapTypes[index].GetFirstUsedType() == PixelImap.Perspective)
                    {
                        res = context.FPMultiply(res, context.Load(StorageKind.Input, IoVariable.FragmentCoord, null, Const(3)));
                    }
                }
                else if (imm10 is AttributeConsts.PositionX or AttributeConsts.PositionY)
                {
                    // FragCoord X/Y must be divided by the render target scale, if resolution scaling is active,
                    // because the shader code is not expecting scaled values.
                    res = context.FPDivide(res, context.Load(StorageKind.ConstantBuffer, SupportBuffer.Binding, Const((int)SupportBufferField.RenderScale), Const(0)));

                    if (imm10 == AttributeConsts.PositionY && context.TranslatorContext.Options.TargetApi != TargetApi.OpenGL)
                    {
                        // If YNegate is enabled, we need to flip the fragment coordinates vertically, unless
                        // the API supports changing the origin (only OpenGL does).
                        if (context.TranslatorContext.Definitions.YNegateEnabled)
                        {
                            Operand viewportHeight = context.Load(StorageKind.ConstantBuffer, 0, Const((int)SupportBufferField.ViewportSize), Const(1));

                            res = context.FPSubtract(viewportHeight, res);
                        }
                    }
                }
                else if (imm10 == AttributeConsts.PrimitiveId && context.TranslatorContext.Definitions.HalvePrimitiveId)
                {
                    // If quads are used, but the host does not support them, they need to be converted to triangles.
                    // Since each quad becomes 2 triangles, we need to compensate here and divide primitive ID by 2.
                    res = context.ShiftRightS32(res, Const(1));
                }
                else if (imm10 == AttributeConsts.FrontFacing && context.TranslatorContext.GpuAccessor.QueryHostHasFrontFacingBug())
                {
                    // gl_FrontFacing sometimes has incorrect (flipped) values depending how it is accessed on Intel GPUs.
                    // This weird trick makes it behave.
                    res = context.ICompareLess(context.INegate(context.FP32ConvertToS32(context.ConditionalSelect(res, ConstF(1f), ConstF(0f)))), Const(0));
                }
            }

            if (ipaOp == IpaOp.Multiply && !isFixedFunc)
            {
                Operand multiplier = GetSrcReg(context, srcB);

                res = context.FPMultiply(res, multiplier);
            }

            res = context.FPSaturate(res, sat);

            context.Copy(GetDest(dest), res);
        }

        public static void Isberd(EmitterContext context)
        {
            InstIsberd op = context.GetOp<InstIsberd>();

            // This instruction performs a load from ISBE (Internal Stage Buffer Entry) memory.
            // Here, we just propagate the offset, as the result from this instruction is usually
            // used with ALD to perform vertex load on geometry or tessellation shaders.
            // The offset is calculated as (PrimitiveIndex * VerticesPerPrimitive) + VertexIndex.
            // Since we hardcode PrimitiveIndex to zero, then the offset will be just VertexIndex.
            context.Copy(GetDest(op.Dest), GetSrcReg(context, op.SrcA));
        }

        public static void OutR(EmitterContext context)
        {
            InstOutR op = context.GetOp<InstOutR>();

            EmitOut(context, op.OutType.HasFlag(OutType.Emit), op.OutType.HasFlag(OutType.Cut));
        }

        public static void OutI(EmitterContext context)
        {
            InstOutI op = context.GetOp<InstOutI>();

            EmitOut(context, op.OutType.HasFlag(OutType.Emit), op.OutType.HasFlag(OutType.Cut));
        }

        public static void OutC(EmitterContext context)
        {
            InstOutC op = context.GetOp<InstOutC>();

            EmitOut(context, op.OutType.HasFlag(OutType.Emit), op.OutType.HasFlag(OutType.Cut));
        }

        private static void EmitOut(EmitterContext context, bool emit, bool cut)
        {
            if (!(emit || cut))
            {
                context.TranslatorContext.GpuAccessor.Log("Invalid OUT encoding.");
            }

            if (emit)
            {
                if (context.TranslatorContext.Definitions.LastInVertexPipeline)
                {
                    context.PrepareForVertexReturn(out Operand tempXLocal, out Operand tempYLocal, out Operand tempZLocal);

                    context.EmitVertex();

                    // Restore output position value before transformation.

                    if (tempXLocal != null)
                    {
                        context.Copy(context.Load(StorageKind.Input, IoVariable.Position, null, Const(0)), tempXLocal);
                    }

                    if (tempYLocal != null)
                    {
                        context.Copy(context.Load(StorageKind.Input, IoVariable.Position, null, Const(1)), tempYLocal);
                    }

                    if (tempZLocal != null)
                    {
                        context.Copy(context.Load(StorageKind.Input, IoVariable.Position, null, Const(2)), tempZLocal);
                    }
                }
                else
                {
                    context.EmitVertex();
                }
            }

            if (cut)
            {
                context.EndPrimitive();
            }
        }

        private static bool HasPrimitiveVertex(int attr)
        {
            return attr is not AttributeConsts.PrimitiveId and
                   not AttributeConsts.TessCoordX and
                   not AttributeConsts.TessCoordY;
        }

        private static bool CanLoadOutput(int attr)
        {
            return attr is not AttributeConsts.TessCoordX and not AttributeConsts.TessCoordY;
        }

        private static bool TryFixedFuncToUserAttributeIpa(EmitterContext context, int attr, out Operand selectedAttr)
        {
            if (attr is >= AttributeConsts.FrontColorDiffuseR and < AttributeConsts.BackColorDiffuseR)
            {
                // TODO: If two sided rendering is enabled, then this should return
                // FrontColor if the fragment is front facing, and back color otherwise.
                selectedAttr = GenerateIpaLoad(context, FixedFuncToUserAttribute(context.TranslatorContext, attr, isOutput: false));
                return true;
            }
            else if (attr == AttributeConsts.FogCoord)
            {
                // TODO: We likely need to emulate the fixed-function functionality for FogCoord here.
                selectedAttr = GenerateIpaLoad(context, FixedFuncToUserAttribute(context.TranslatorContext, attr, isOutput: false));
                return true;
            }
            else if (attr is >= AttributeConsts.BackColorDiffuseR and < AttributeConsts.ClipDistance0)
            {
                selectedAttr = ConstF(((attr >> 2) & 3) == 3 ? 1f : 0f);
                return true;
            }
            else if (attr is >= AttributeConsts.TexCoordBase and < AttributeConsts.TexCoordEnd)
            {
                selectedAttr = GenerateIpaLoad(context, FixedFuncToUserAttribute(context.TranslatorContext, attr, isOutput: false));
                return true;
            }

            selectedAttr = GenerateIpaLoad(context, attr);
            return false;
        }

        private static Operand GenerateIpaLoad(EmitterContext context, int offset)
        {
            return AttributeMap.GenerateAttributeLoad(context, null, offset, isOutput: false, isPerPatch: false);
        }

        private static int FixedFuncToUserAttribute(TranslatorContext translatorContext, int attr, bool isOutput)
        {
            bool supportsLayerFromVertexOrTess = translatorContext.GpuAccessor.QueryHostSupportsLayerVertexTessellation();
            int fixedStartAttr = supportsLayerFromVertexOrTess ? 0 : 1;

            if (attr == AttributeConsts.Layer && translatorContext.Definitions.Stage != ShaderStage.Geometry && !supportsLayerFromVertexOrTess)
            {
                attr = FixedFuncToUserAttribute(translatorContext, attr, AttributeConsts.Layer, 0, isOutput);
                translatorContext.SetLayerOutputAttribute(attr);
            }
            else if (attr == AttributeConsts.FogCoord)
            {
                attr = FixedFuncToUserAttribute(translatorContext, attr, AttributeConsts.FogCoord, fixedStartAttr, isOutput);
            }
            else if (attr is >= AttributeConsts.FrontColorDiffuseR and < AttributeConsts.ClipDistance0)
            {
                attr = FixedFuncToUserAttribute(translatorContext, attr, AttributeConsts.FrontColorDiffuseR, fixedStartAttr + 1, isOutput);
            }
            else if (attr is >= AttributeConsts.TexCoordBase and < AttributeConsts.TexCoordEnd)
            {
                attr = FixedFuncToUserAttribute(translatorContext, attr, AttributeConsts.TexCoordBase, fixedStartAttr + 5, isOutput);
            }

            return attr;
        }

        private static int FixedFuncToUserAttribute(TranslatorContext translatorContext, int attr, int baseAttr, int baseIndex, bool isOutput)
        {
            int index = (attr - baseAttr) >> 4;
            int userAttrIndex = translatorContext.AttributeUsage.GetFreeUserAttribute(isOutput, baseIndex + index);

            if ((uint)userAttrIndex < Constants.MaxAttributes)
            {
                attr = AttributeConsts.UserAttributeBase + userAttrIndex * 16 + (attr & 0xf);
            }
            else
            {
                translatorContext.GpuAccessor.Log($"No enough user attributes for fixed attribute offset 0x{attr:X}.");
            }

            return attr;
        }

        private static bool TryConvertIdToIndexForVulkan(EmitterContext context, int attr, out Operand value)
        {
            if (context.TranslatorContext.Options.TargetApi == TargetApi.Vulkan)
            {
                if (attr == AttributeConsts.InstanceId)
                {
                    value = context.ISubtract(
                        context.Load(StorageKind.Input, IoVariable.InstanceIndex),
                        context.Load(StorageKind.Input, IoVariable.BaseInstance));
                    return true;
                }
                else if (attr == AttributeConsts.VertexId)
                {
                    value = context.Load(StorageKind.Input, IoVariable.VertexIndex);
                    return true;
                }
            }

            value = null;
            return false;
        }
    }
}

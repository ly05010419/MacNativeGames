namespace Ryujinx.Graphics.Shader.Decoders
{
    enum RegisterType
    {
        Flag,
        Gpr,
        Predicate,
        Uniform,            // Ampere's uniform registers UR0-UR62 (UR63 = URZ reads as zero)
        UniformPredicate,   // Ampere's uniform predicates UP0-UP6 (UP7 = UPT reads as true)
        Shadow,             // the Ampere frontend's deferred register writes, within one block (Translator.EmitOpsAmpere)
    }
}

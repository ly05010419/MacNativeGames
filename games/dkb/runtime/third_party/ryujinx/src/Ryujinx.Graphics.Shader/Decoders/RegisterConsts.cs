namespace Ryujinx.Graphics.Shader.Decoders
{
    static class RegisterConsts
    {
        public const int GprsCount = 255;
        public const int PredsCount = 7;
        public const int FlagsCount = 4;
        public const int UniformsCount = 63;
        public const int UniformPredsCount = 7;
        public const int ShadowsCount = 256;
        public const int TotalCount = GprsCount + PredsCount + FlagsCount + UniformsCount + UniformPredsCount + ShadowsCount;

        public const int RegisterZeroIndex = GprsCount;
        public const int PredicateTrueIndex = PredsCount;
        public const int UniformZeroIndex = UniformsCount;
        public const int UniformPredicateTrueIndex = UniformPredsCount;
    }
}

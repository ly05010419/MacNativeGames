namespace Ryujinx.Graphics.Shader
{
    public enum TessPatchType
    {
        Isolines = 0,
        Triangles = 1,
        Quads = 2,
    }

    static class TessPatchTypeExtensions
    {
        extension(TessPatchType patchType)
        {
            public string Glsl => patchType switch
            {
                TessPatchType.Isolines => "isolines",
                TessPatchType.Quads => "quads",
                _ => "triangles",
            };
        }
    }
}

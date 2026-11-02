// The preprocessor: function-like macros with # and ##, conditionals,
// defined, #undef, a macro from the command line (-D FROM_COMMAND_LINE),
// effect syntax that D3DXCompileShader skips.
#define ADD(a, b) ((a) + (b))
#define CAT(a, b) a##b
#define STR(x) #x
#define TWICE(x) ADD(x, x)
#define LEVEL 2
#if LEVEL > 1 && defined(ADD)
#define COLOR_SCALE 2.0
#elif LEVEL == 1
#error wrong branch
#else
#error wrong branch
#endif
#ifndef MISSING
#define HAVE_MISSING 0
#endif
#undef LEVEL
#ifdef LEVEL
#error #undef failed
#endif
#pragma warning(disable : 3571)

float4 CAT(ti, nt) : register(c2) < string UIName = "Tint"; >;
string name = STR(hello world);
texture tex;
sampler2D s = sampler_state { Texture = <tex>; MinFilter = Linear; };

float4 main(float4 c : COLOR0) : COLOR0 {
  return TWICE(c) * COLOR_SCALE * tint * FROM_COMMAND_LINE + HAVE_MISSING;
}

technique T0 {
  pass P0 {
    PixelShader = compile ps_2_0 main();
  }
}

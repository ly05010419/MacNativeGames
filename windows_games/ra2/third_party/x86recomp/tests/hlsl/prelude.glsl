// A stand-in for a graphics backend's prelude (runtime/hlsl/hlsl.h), with
// SDL_GPU's SPIR-V set numbers.
#ifdef R_VERTEX_SHADER
#define R_FLOAT_BINDING set = 1, binding = 0
#define R_INT_BINDING set = 1, binding = 1
#define R_SAMPLER_BINDING(n) set = 0, binding = n
layout(std140, set = 1, binding = 2) uniform r_test_vs { vec4 r_test_input[16]; };
vec4 r_fetch(int slot) { return r_test_input[slot]; }
vec4 r_position(vec4 p) { return vec4(p.x, -p.y, p.z, p.w); }
#else
#define R_FLOAT_BINDING set = 3, binding = 0
#define R_INT_BINDING set = 3, binding = 1
#define R_SAMPLER_BINDING(n) set = 2, binding = n
#define R_BIAS(n) 0.0
vec2 r_vpos() { return gl_FragCoord.xy - 0.5; }
float r_vface() { return gl_FrontFacing ? 1.0 : -1.0; }
layout(std140, set = 3, binding = 2) uniform r_test_ps { float r_test_ref; };
void r_color0(inout vec4 c) { if (c.a < r_test_ref) discard; }
#endif

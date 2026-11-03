#pragma once
// HLSL as D3DX 9's compiler accepts it (shader models 1.1-3.0: the
// D3DXCompileShader dialect, effect syntax skipped) translated to GLSL
// 4.50 for Vulkan-style SPIR-V (and MSL through SPIRV-Cross). Generated
// from source, so the translation keeps Direct3D 9's interface instead of
// a compiled shader's: the constant registers the application sets, the
// constant table it queries, and vertex input / varying semantics.
//
// The GLSL is a body without #version: the graphics backend prepends
// `#version 450` and a prelude that defines
//   R_FLOAT_BINDING, R_INT_BINDING  layout qualifiers (set, binding) of the
//                                   uniform blocks below;
//   R_SAMPLER_BINDING(n)            layout qualifiers of sampler register n;
//   vec4 r_fetch(int slot)          vertex shaders: input `slot` (see
//                                   Program::inputs) as the input register
//                                   holds it, defaults for missing ones;
//   vec4 r_position(vec4 p)         vertex shaders: the POSITION output to
//                                   gl_Position (clip-space conventions);
//   vec2 r_vpos(), float r_vface()  pixel shaders: VPOS and VFACE;
//   float R_BIAS(n)                 pixel shaders: an LOD bias added where
//                                   sampler register n is sampled with an
//                                   implicit level (0.0 if the backend's
//                                   samplers apply D3D9's MIPMAPLODBIAS);
//   void r_color0(inout vec4 c)     pixel shaders that write COLOR0: called
//                                   last with it (the alpha test).
// The body declares, if it reads them:
//   layout(std140, R_FLOAT_BINDING) uniform r_float_regs { vec4 c[n]; };
//   layout(std140, R_INT_BINDING) uniform r_int_regs { ivec4 r_i[16];
//                                                       ivec4 r_b[4]; };
//   (bool register k is r_b[k / 4][k % 4])
//   layout(R_SAMPLER_BINDING(k)) uniform samplerXX r_s<k>;
// and varyings at fixed locations per semantic (VaryingLocation), so any
// vertex shader pairs with any pixel shader without linking.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::hlsl {

struct Define {
  std::string name, value;
};

enum class Stage : uint8_t { Vertex, Pixel };

// D3DXREGISTER_SET, D3DXPARAMETER_CLASS and D3DXPARAMETER_TYPE values.
enum class RegisterSet : uint32_t { Bool = 0, Int4 = 1, Float4 = 2, Sampler = 3 };
enum class ParamClass : uint32_t {
  Scalar = 0, Vector = 1, MatrixRows = 2, MatrixColumns = 3, Object = 4, Struct = 5,
};
enum class ParamType : uint32_t {
  Void = 0, Bool = 1, Int = 2, Float = 3, String = 4, Texture = 5,
  Texture1D = 6, Texture2D = 7, Texture3D = 8, TextureCube = 9,
  Sampler = 10, Sampler1D = 11, Sampler2D = 12, Sampler3D = 13, SamplerCube = 14,
};

// One entry of the constant table (D3DXCONSTANT_DESC).
struct Constant {
  std::string name;
  RegisterSet set = RegisterSet::Float4;
  uint32_t index = 0, count = 0;  // registers
  ParamClass cls = ParamClass::Scalar;
  ParamType type = ParamType::Float;
  uint32_t rows = 1, columns = 1, elements = 1, struct_members = 0, bytes = 0;
};

// D3DDECLUSAGE values, plus the pixel shader's own input semantics.
enum Usage : uint32_t {
  kPosition = 0, kBlendWeight = 1, kBlendIndices = 2, kNormal = 3, kPSize = 4,
  kTexcoord = 5, kTangent = 6, kBinormal = 7, kTessFactor = 8, kPositionT = 9,
  kColor = 10, kFog = 11, kDepth = 12, kSample = 13,
  kVPos = 0x100, kVFace = 0x101,
};

struct Semantic {
  uint32_t usage = 0, index = 0;
  bool operator==(const Semantic&) const = default;
};

// Location of a varying (vertex shader output, pixel shader input), the
// same in every shader; -1 if the semantic cannot be a varying.
int VaryingLocation(Semantic s);

struct Sampler {
  uint32_t reg = 0;
  ParamType type = ParamType::Sampler2D;  // Sampler2D, Sampler3D or SamplerCube
};

struct Program {
  Stage stage = Stage::Vertex;
  uint32_t major = 0, minor = 0;  // shader model
  std::string glsl;
  std::vector<Constant> constants;
  std::vector<Semantic> inputs;    // vertex shaders: r_fetch slot -> semantic
  std::vector<Semantic> varyings;  // written (vertex) or read (pixel)
  uint32_t color_outputs = 0;      // pixel shaders: mask of COLORn written
  bool writes_depth = false, writes_point_size = false;
  std::vector<Sampler> samplers;
  uint32_t float_regs = 0;  // c registers the GLSL declares
};

// D3DXSHADER flags that change the translation.
enum : uint32_t {
  kPackMatrixRowMajor = 0x8,
  kPackMatrixColumnMajor = 0x10,
};

// Translates `source` (entry function `entry`, profile such as "vs_3_0").
// On failure returns false with fxc-style messages in *errors.
bool Compile(std::string_view source, const std::vector<Define>& defines,
             const std::string& entry, const std::string& profile, uint32_t flags,
             Program* out, std::string* errors);

// Translates a compiled D3D9 shader (shader model 1.1-3.0 tokens, as
// CreateVertexShader / CreatePixelShader take them, END included) to the
// same GLSL contract. No constant table: applications set the registers
// the bytecode reads. On failure returns false with a message in *errors.
bool TranslateBytecode(const uint32_t* tokens, size_t count, Program* out, std::string* errors);

// A Program as bytes and back (shader containers).
std::vector<uint8_t> Serialize(const Program& p);
bool Deserialize(const uint8_t* data, size_t size, Program* p);

}  // namespace recomp::hlsl

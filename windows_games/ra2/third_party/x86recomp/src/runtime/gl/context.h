#pragma once
// An OpenGL 1.x rendering context on gfx::Device: the GL state machine
// (fixed-function pipeline, texture objects, vertex arrays and immediate
// mode, matrix stacks) translated into the device's D3D-style state and
// draws. It takes plain host pointers and knows nothing of guest memory or
// Win32, so the opengl32 HLE marshals into it and native code can use it
// directly. One Context per GL context; calls come from one thread.
//
// Coverage is what the supported games use: GL 1.1 plus ARB_multitexture,
// EXT_texture_env_add, ARB_texture_env_combine, EXT_compiled_vertex_array,
// S3TC compressed textures and anisotropic filtering. Anything else
// records GL_INVALID_ENUM and warns once, so a gap is visible rather than
// silently drawn wrong.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"

namespace recomp::gl {

using Enum = uint32_t;

// The GL enum values the context understands (standard OpenGL numbering).
enum : Enum {
  NO_ERROR = 0, INVALID_ENUM = 0x0500, INVALID_VALUE = 0x0501,
  INVALID_OPERATION = 0x0502, STACK_OVERFLOW = 0x0503, STACK_UNDERFLOW = 0x0504,
  // primitives
  POINTS = 0x0000, LINES = 0x0001, LINE_LOOP = 0x0002, LINE_STRIP = 0x0003,
  TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005, TRIANGLE_FAN = 0x0006,
  QUADS = 0x0007, QUAD_STRIP = 0x0008, POLYGON = 0x0009,
  // comparison
  NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203,
  GREATER = 0x0204, NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207,
  // blending
  ZERO = 0, ONE = 1, SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301,
  SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304,
  ONE_MINUS_DST_ALPHA = 0x0305, DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307,
  SRC_ALPHA_SATURATE = 0x0308,
  // faces and polygon modes
  FRONT = 0x0404, BACK = 0x0405, FRONT_AND_BACK = 0x0408, CW = 0x0900,
  CCW = 0x0901, POINT = 0x1B00, LINE = 0x1B01, FILL = 0x1B02, FLAT = 0x1D00,
  SMOOTH = 0x1D01,
  // capabilities
  CULL_FACE = 0x0B44, LIGHTING = 0x0B50, FOG = 0x0B60, DEPTH_TEST = 0x0B71,
  STENCIL_TEST = 0x0B90, NORMALIZE = 0x0BA1, ALPHA_TEST = 0x0BC0,
  DITHER = 0x0BD0, BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, TEXTURE_2D = 0x0DE1,
  POLYGON_OFFSET_FILL = 0x8037, CLIP_PLANE0 = 0x3000, LIGHT0 = 0x4000,
  COLOR_MATERIAL = 0x0B57, TEXTURE_GEN_S = 0x0C60, TEXTURE_GEN_T = 0x0C61,
  RESCALE_NORMAL = 0x803A,
  // client arrays
  VERTEX_ARRAY = 0x8074, NORMAL_ARRAY = 0x8075, COLOR_ARRAY = 0x8076,
  TEXTURE_COORD_ARRAY = 0x8078,
  // types
  BYTE = 0x1400, UNSIGNED_BYTE = 0x1401, SHORT = 0x1402, UNSIGNED_SHORT = 0x1403,
  INT = 0x1404, UNSIGNED_INT = 0x1405, FLOAT = 0x1406, DOUBLE = 0x140A,
  // pixel formats
  ALPHA = 0x1906, RGB = 0x1907, RGBA = 0x1908, LUMINANCE = 0x1909,
  LUMINANCE_ALPHA = 0x190A, BGR_EXT = 0x80E0, BGRA_EXT = 0x80E1,
  DEPTH_COMPONENT = 0x1902,
  RGB5 = 0x8050, RGB8 = 0x8051, RGBA4 = 0x8056, RGB5_A1 = 0x8057, RGBA8 = 0x8058,
  ALPHA8 = 0x803C, LUMINANCE8 = 0x8040, LUMINANCE8_ALPHA8 = 0x8045,
  RGB4 = 0x804F, INTENSITY = 0x8049, INTENSITY8 = 0x804B,
  COMPRESSED_RGB_S3TC_DXT1 = 0x83F0, COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1,
  COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2, COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3,
  // textures
  TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801,
  TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, NEAREST = 0x2600,
  LINEAR = 0x2601, NEAREST_MIPMAP_NEAREST = 0x2700,
  LINEAR_MIPMAP_NEAREST = 0x2701, NEAREST_MIPMAP_LINEAR = 0x2702,
  LINEAR_MIPMAP_LINEAR = 0x2703, CLAMP = 0x2900, REPEAT = 0x2901,
  CLAMP_TO_EDGE = 0x812F, TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE,
  MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF, TEXTURE_BORDER_COLOR = 0x1004,
  // texture environment
  TEXTURE_ENV = 0x2300, TEXTURE_ENV_MODE = 0x2200, TEXTURE_ENV_COLOR = 0x2201,
  MODULATE = 0x2100, DECAL = 0x2101, ADD = 0x0104, REPLACE = 0x1E01,
  BLEND_MODE = 0x0BE2,  // GL_BLEND as an env mode
  COMBINE = 0x8570, COMBINE_RGB = 0x8571, COMBINE_ALPHA = 0x8572,
  RGB_SCALE = 0x8573, ADD_SIGNED = 0x8574, INTERPOLATE = 0x8575,
  CONSTANT = 0x8576, PRIMARY_COLOR = 0x8577, PREVIOUS = 0x8578,
  SUBTRACT = 0x84E7, DOT3_RGB = 0x86AE, DOT3_RGBA = 0x86AF,
  SOURCE0_RGB = 0x8580, SOURCE1_RGB = 0x8581, SOURCE2_RGB = 0x8582,
  SOURCE0_ALPHA = 0x8588, SOURCE1_ALPHA = 0x8589, SOURCE2_ALPHA = 0x858A,
  OPERAND0_RGB = 0x8590, OPERAND1_RGB = 0x8591, OPERAND2_RGB = 0x8592,
  OPERAND0_ALPHA = 0x8598, OPERAND1_ALPHA = 0x8599, OPERAND2_ALPHA = 0x859A,
  ALPHA_SCALE = 0x0D1C, TEXTURE = 0x1702,
  TEXTURE0 = 0x84C0,
  // matrices
  MODELVIEW = 0x1700, PROJECTION = 0x1701, MATRIX_MODE = 0x0BA0,
  MODELVIEW_MATRIX = 0x0BA6, PROJECTION_MATRIX = 0x0BA7,
  // fog
  FOG_DENSITY = 0x0B62, FOG_START = 0x0B63, FOG_END = 0x0B64,
  FOG_MODE = 0x0B65, FOG_COLOR = 0x0B66, EXP = 0x0800, EXP2 = 0x0801,
  // stencil
  KEEP = 0x1E00, INCR = 0x1E02, DECR = 0x1E03, INVERT = 0x150A,
  // clear bits
  DEPTH_BUFFER_BIT = 0x0100, STENCIL_BUFFER_BIT = 0x0400,
  COLOR_BUFFER_BIT = 0x4000,
  // lighting
  SPOT_EXPONENT = 0x1205, SPOT_CUTOFF = 0x1206, CONSTANT_ATTENUATION = 0x1207,
  LINEAR_ATTENUATION = 0x1208, QUADRATIC_ATTENUATION = 0x1209,
  AMBIENT = 0x1200, DIFFUSE = 0x1201, SPECULAR = 0x1202, POSITION = 0x1203,
  SPOT_DIRECTION = 0x1204, EMISSION = 0x1600, SHININESS = 0x1601,
  AMBIENT_AND_DIFFUSE = 0x1602, LIGHT_MODEL_LOCAL_VIEWER = 0x0B51,
  LIGHT_MODEL_TWO_SIDE = 0x0B52, LIGHT_MODEL_AMBIENT = 0x0B53,
  MAX_LIGHTS = 0x0D31, MAX_CLIP_PLANES = 0x0D32,
  // pixel store
  UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
  UNPACK_ROW_LENGTH = 0x0CF2,
  // queries
  VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, EXTENSIONS = 0x1F03,
  MAX_TEXTURE_SIZE = 0x0D33, MAX_TEXTURE_UNITS = 0x84E2, VIEWPORT = 0x0BA2,
  DEPTH_BITS = 0x0D56, STENCIL_BITS = 0x0D57, RED_BITS = 0x0D52,
  ALPHA_BITS = 0x0D55,
};

class Context {
 public:
  Context(gfx::Device* dev, int fb_w, int fb_h);
  ~Context();

  // The window (default framebuffer) changed size.
  void Resize(int fb_w, int fb_h);
  int Width() const { return fb_w_; }
  int Height() const { return fb_h_; }
  // SwapBuffers: the frame is complete; the caller presents the device.
  void EndFrame();

  Enum GetError();
  const char* GetString(Enum name) const;
  // Queries; false (and INVALID_ENUM) for parameters not modelled.
  bool GetIntegerv(Enum pname, int32_t* out);
  bool GetFloatv(Enum pname, float* out);

  // ---- fixed-function state
  void SetEnabled(Enum cap, bool on);
  bool IsEnabled(Enum cap) const;
  void BlendFunc(Enum src, Enum dst);
  void AlphaFunc(Enum func, float ref);
  void DepthFunc(Enum func);
  void DepthMask(bool on);
  void DepthRange(double near_, double far_);
  void ColorMask(bool r, bool g, bool b, bool a);
  void CullFace(Enum face);
  void FrontFace(Enum dir);
  void PolygonMode(Enum face, Enum mode);
  void PolygonOffset(float factor, float units);
  void ShadeModel(Enum mode);
  void Viewport(int x, int y, int w, int h);
  void Scissor(int x, int y, int w, int h);
  void StencilFunc(Enum func, int ref, uint32_t mask);
  void StencilOp(Enum fail, Enum zfail, Enum zpass);
  void StencilMask(uint32_t mask);
  void Fog(Enum pname, const float* v);
  void ClipPlane(Enum plane, const double eqn[4]);
  void ClearColor(float r, float g, float b, float a);
  void ClearDepth(double d);
  void ClearStencil(int s);
  void Clear(uint32_t mask);
  void PixelStore(Enum pname, int v);

  // ---- lighting
  void Light(Enum light, Enum pname, const float* v);
  void LightModel(Enum pname, const float* v);
  void Material(Enum face, Enum pname, const float* v);
  void ColorMaterial(Enum face, Enum mode);

  // ---- matrices
  void MatrixMode(Enum mode);
  void LoadIdentity();
  void LoadMatrix(const float m[16]);
  void MultMatrix(const float m[16]);
  void PushMatrix();
  void PopMatrix();
  void Ortho(double l, double r, double b, double t, double n, double f);
  void Frustum(double l, double r, double b, double t, double n, double f);
  void Translate(float x, float y, float z);
  void Rotate(float angle_deg, float x, float y, float z);
  void Scale(float x, float y, float z);

  // ---- textures
  void GenTextures(int n, uint32_t* names);
  void DeleteTextures(int n, const uint32_t* names);
  void BindTexture(Enum target, uint32_t name);
  void TexParameter(Enum target, Enum pname, const float* v);
  void TexImage2D(Enum target, int level, Enum internal_format, int w, int h,
                  int border, Enum format, Enum type, const void* pixels);
  void TexSubImage2D(Enum target, int level, int x, int y, int w, int h,
                     Enum format, Enum type, const void* pixels);
  void CompressedTexImage2D(Enum target, int level, Enum internal_format,
                            int w, int h, int border, int size, const void* data);
  void TexEnv(Enum target, Enum pname, const float* v);
  void ActiveTexture(Enum unit);
  void ClientActiveTexture(Enum unit);

  // ---- vertex arrays and drawing
  void VertexPointer(int size, Enum type, int stride, const void* p);
  void ColorPointer(int size, Enum type, int stride, const void* p);
  void TexCoordPointer(int size, Enum type, int stride, const void* p);
  void NormalPointer(Enum type, int stride, const void* p);
  void SetClientState(Enum array, bool on);
  void DrawArrays(Enum mode, int first, int count);
  void DrawElements(Enum mode, int count, Enum type, const void* indices);
  void ArrayElement(int i);

  // ---- immediate mode
  void Begin(Enum mode);
  void End();
  void Vertex(float x, float y, float z, float w);
  void Color(float r, float g, float b, float a);
  void TexCoord(int unit, float s, float t);
  void Normal(float x, float y, float z);

  // ---- framebuffer readback (rows bottom-up, as GL returns them)
  void ReadPixels(int x, int y, int w, int h, Enum format, Enum type, void* out);

  void SetVsync(bool on);

  static constexpr int kUnits = 4;  // texture units advertised

 private:
  struct Texture;
  struct Unit;
  struct Array {
    bool enabled = false;
    int size = 4;
    Enum type = FLOAT;
    int stride = 0;
    const uint8_t* ptr = nullptr;
  };
  struct Vtx {  // one assembled vertex
    float pos[4];
    float normal[3];
    uint32_t color;  // D3DCOLOR (A8R8G8B8)
    float tc[kUnits][2];
  };

  void Error(Enum e, const char* what);
  void Unsupported(const char* what, Enum v);
  float* Top();  // current matrix
  void MarkMatrix();
  Texture* Bound(int unit);
  void ApplyState();
  int ApplyTextures();  // returns the texture coordinate sets needed
  void ApplyMatrices();
  void RS(uint32_t state, uint32_t value);
  void TSS(uint32_t stage, uint32_t type, uint32_t value);
  void FetchVertex(int i, Vtx* v) const;
  // What a draw reads for each attribute; an array that is not enabled
  // (or has no pointer) stands for the current value.
  struct Sources {
    Array pos, normal, color, tc[kUnits];
    bool d3d_color = false;  // colours are D3DCOLORs (immediate mode)
  };
  Sources ClientSources() const;
  // Draws draw_verts_ (the source element of each device vertex) as
  // primitive `mode` over draw_seq_ (the device vertex of each GL vertex).
  void DrawVertices(Enum mode, const Sources& src);

  gfx::Device* dev_;
  int fb_w_, fb_h_;
  gfx::DepthId depth_ = 0;
  Enum error_ = NO_ERROR;
  std::set<std::pair<std::string, Enum>> warned_;

  // fixed-function state
  std::set<Enum> enabled_;
  Enum blend_src_ = ONE, blend_dst_ = ZERO;
  Enum alpha_func_ = ALWAYS;
  float alpha_ref_ = 0;
  Enum depth_func_ = LESS;
  bool depth_mask_ = true;
  double depth_near_ = 0, depth_far_ = 1;
  uint32_t color_mask_ = 0xF;
  Enum cull_face_ = BACK, front_face_ = CCW, polygon_mode_ = FILL;
  float offset_factor_ = 0, offset_units_ = 0;
  Enum shade_model_ = SMOOTH;
  int vp_[4] = {0, 0, 0, 0};
  int scissor_[4] = {0, 0, 0, 0};
  Enum stencil_func_ = ALWAYS;
  int stencil_ref_ = 0;
  uint32_t stencil_mask_ = 0xFFFFFFFF, stencil_write_ = 0xFFFFFFFF;
  Enum stencil_fail_ = KEEP, stencil_zfail_ = KEEP, stencil_zpass_ = KEEP;
  Enum fog_mode_ = EXP;
  float fog_density_ = 1, fog_start_ = 0, fog_end_ = 1;
  float fog_color_[4] = {0, 0, 0, 0};
  double clip_planes_[6][4] = {};  // eye space

  struct LightState {
    float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1},
          specular[4] = {0, 0, 0, 1};
    float position[4] = {0, 0, 1, 0};  // eye space
    float spot_dir[3] = {0, 0, -1};    // eye space
    float spot_exp = 0, spot_cutoff = 180;
    float att[3] = {1, 0, 0};
  };
  LightState lights_[8];
  float model_ambient_[4] = {0.2f, 0.2f, 0.2f, 1};
  float mat_ambient_[4] = {0.2f, 0.2f, 0.2f, 1}, mat_diffuse_[4] = {0.8f, 0.8f, 0.8f, 1},
        mat_specular_[4] = {0, 0, 0, 1}, mat_emission_[4] = {0, 0, 0, 1};
  float mat_shininess_ = 0;
  Enum color_material_ = AMBIENT_AND_DIFFUSE;
  float clear_color_[4] = {0, 0, 0, 0};
  double clear_depth_ = 1;
  int clear_stencil_ = 0;
  int unpack_alignment_ = 4, pack_alignment_ = 4, unpack_row_length_ = 0;

  // matrices: stacks for modelview, projection, and one per texture unit
  Enum matrix_mode_ = MODELVIEW;
  std::vector<std::array<float, 16>> stacks_[2 + kUnits];
  bool matrices_dirty_ = true;

  // textures
  std::map<uint32_t, Texture*> textures_;
  uint32_t next_name_ = 1;
  int active_unit_ = 0, client_unit_ = 0;
  Unit* units_;

  // arrays and immediate mode
  Array vertex_, color_, normal_, texcoord_[kUnits];
  bool in_begin_ = false;
  Enum begin_mode_ = POINTS;
  std::vector<Vtx> immediate_;
  Vtx current_{};
  // per-draw scratch, kept between draws
  std::vector<uint32_t> draw_verts_;
  std::vector<uint16_t> draw_seq_;
  std::vector<uint32_t> remap_;  // DrawElements: epoch << 16 | device vertex
  uint32_t remap_epoch_ = 0;

  // applied device state (to skip redundant calls)
  uint32_t rs_cache_[256];
  uint32_t tss_cache_[8][32];
  bool scene_ = false;
  bool vsync_ = false;
};

}  // namespace recomp::gl

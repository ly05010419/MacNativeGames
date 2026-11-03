// Compile: preprocess, parse, check, allocate constant registers, emit.

#include "runtime/hlsl/hlsl.h"

#include <cctype>
#include <cstring>

#include "runtime/hlsl/ast.h"

namespace recomp::hlsl {

bool ParseSemantic(const std::string& s, Semantic* out) {
  static const std::pair<const char*, uint32_t> kUsages[] = {
      {"POSITIONT", kPositionT}, {"POSITION", kPosition}, {"BLENDWEIGHT", kBlendWeight},
      {"BLENDINDICES", kBlendIndices}, {"NORMAL", kNormal}, {"PSIZE", kPSize},
      {"TEXCOORD", kTexcoord}, {"TANGENT", kTangent}, {"BINORMAL", kBinormal},
      {"TESSFACTOR", kTessFactor}, {"COLOR", kColor}, {"FOG", kFog}, {"DEPTH", kDepth},
      {"SAMPLE", kSample}, {"VPOS", kVPos}, {"VFACE", kVFace},
      {"SV_POSITION", kPosition}, {"SV_TARGET", kColor}, {"SV_DEPTH", kDepth},
  };
  size_t d = s.size();
  while (d > 0 && std::isdigit(uint8_t(s[d - 1]))) d--;
  std::string name = s.substr(0, d);
  for (const auto& [n, usage] : kUsages) {
    if (name != n) continue;
    out->usage = usage;
    out->index = d < s.size() ? uint32_t(std::stoul(s.substr(d))) : 0;
    return true;
  }
  return false;
}

int VaryingLocation(Semantic s) {
  // TEXCOORD0-15 0-15, COLOR0-1 16-17, FOG 18, NORMAL0-1 19-20, TANGENT0
  // 21, BINORMAL0 22, BLENDWEIGHT0 23, BLENDINDICES0 24, POSITION1-3
  // 25-27, PSIZE as a varying 28, DEPTH 29: within the 32 locations that
  // Vulkan and Metal implementations offer.
  switch (s.usage) {
    case kTexcoord: return s.index < 16 ? int(s.index) : -1;
    case kColor: return s.index < 2 ? 16 + int(s.index) : -1;
    case kFog: return s.index == 0 ? 18 : -1;
    case kNormal: return s.index < 2 ? 19 + int(s.index) : -1;
    case kTangent: return s.index == 0 ? 21 : -1;
    case kBinormal: return s.index == 0 ? 22 : -1;
    case kBlendWeight: return s.index == 0 ? 23 : -1;
    case kBlendIndices: return s.index == 0 ? 24 : -1;
    case kPosition: return s.index >= 1 && s.index < 4 ? 24 + int(s.index) : -1;
    case kPSize: return s.index == 0 ? 28 : -1;
    case kDepth: return s.index == 0 ? 29 : -1;
    default: return -1;
  }
}

namespace {

struct Limits {
  uint32_t c, i, b, s;
};

Limits LimitsOf(const Target& t) {
  if (t.stage == Stage::Vertex) return {256, 16, 16, t.major >= 3 ? 4u : 0u};
  uint32_t c = t.major >= 3 ? 224 : t.major == 2 ? (t.minor ? 224 : 32) : 8;
  return {c, t.major >= 3 || t.minor ? 16u : 0u, t.major >= 3 || t.minor ? 16u : 0u, 16};
}

bool ParseProfile(const std::string& p, Target* t) {
  if (p.size() != 6 || p[2] != '_' || p[4] != '_') return false;
  if (p.compare(0, 2, "vs") == 0) t->stage = Stage::Vertex;
  else if (p.compare(0, 2, "ps") == 0) t->stage = Stage::Pixel;
  else return false;
  if (p[3] < '1' || p[3] > '3') return false;
  t->major = uint32_t(p[3] - '0');
  if (p[5] == 'a' || p[5] == 'b' || p[5] == 'x') t->minor = 1;  // 2_a, 2_b: version 2.1
  else if (std::isdigit(uint8_t(p[5]))) t->minor = uint32_t(p[5] - '0');
  else return false;
  return true;
}

RegisterSet SetOf(const VarDecl& v) {
  if (v.type.base == Base::Sampler) return RegisterSet::Sampler;
  if (v.reg_set == 'b' || (v.reg_set == 0 && v.type.base == Base::Bool)) return RegisterSet::Bool;
  if (v.reg_set == 'i') return RegisterSet::Int4;
  return RegisterSet::Float4;
}

uint32_t CountOf(const VarDecl& v) {
  Type e = v.type.Elem();
  uint32_t per = 1;
  if (e.matrix) per = (v.flags & kRowMajor) ? e.rows : e.cols;
  return per * (v.type.array ? v.type.array : 1);
}

}  // namespace

void AllocateRegisters(Unit& unit, const Target& target, std::vector<Constant>* table) {
  Limits lim = LimitsOf(target);
  std::vector<bool> taken[4] = {std::vector<bool>(lim.b), std::vector<bool>(lim.i),
                                std::vector<bool>(lim.c), std::vector<bool>(lim.s)};
  // Explicit bindings of every global, read or not, are off limits for the
  // compiler's own placement: applications set registers by number.
  for (auto& g : unit.globals) {
    if (!g->reg_set || (g->flags & kStatic)) continue;
    auto& file = taken[uint32_t(SetOf(*g))];
    for (uint32_t r = g->reg; r < g->reg + CountOf(*g) && r < file.size(); r++) file[r] = true;
  }
  for (auto& g : unit.globals) {
    if (!g->used || g->cls != VarDecl::Class::Uniform) continue;
    const Type& t = g->type;
    if (t.base == Base::Struct) Fail(g->loc, "struct uniforms are not supported");
    if (!t.Numeric() && t.base != Base::Sampler) Fail(g->loc, "uniform of type " + TypeName(t));
    g->set = SetOf(*g);
    g->reg_count = CountOf(*g);
    auto& file = taken[uint32_t(g->set)];
    if (g->reg_set) {
      g->reg_index = g->reg;
    } else {
      uint32_t r = 0;
      for (;; r++) {
        if (r + g->reg_count > file.size()) Fail(g->loc, "out of registers for " + g->name);
        bool free = true;
        for (uint32_t k = 0; k < g->reg_count && free; k++) free = !file[r + k];
        if (free) break;
      }
      g->reg_index = r;
      for (uint32_t k = 0; k < g->reg_count; k++) file[r + k] = true;
    }
    if (g->reg_index + g->reg_count > file.size())
      Fail(g->loc, g->name + " does not fit in the registers of this profile");

    Constant c;
    c.name = g->name;
    c.set = g->set;
    c.index = g->reg_index;
    c.count = g->reg_count;
    c.elements = t.array ? t.array : 1;
    Type e = t.Elem();
    if (e.base == Base::Sampler) {
      c.cls = ParamClass::Object;
      Dim d = e.dim != Dim::Any ? e.dim : g->sampler_dim;
      c.type = d == Dim::D1 ? ParamType::Sampler1D : d == Dim::D3 ? ParamType::Sampler3D
               : d == Dim::Cube ? ParamType::SamplerCube : d == Dim::D2 ? ParamType::Sampler2D
               : ParamType::Sampler;
      c.bytes = 4 * c.elements;
    } else {
      c.cls = e.matrix ? ((g->flags & kRowMajor) ? ParamClass::MatrixRows : ParamClass::MatrixColumns)
              : e.cols > 1 ? ParamClass::Vector
                           : ParamClass::Scalar;
      c.type = e.base == Base::Bool ? ParamType::Bool
               : e.base == Base::Int || e.base == Base::Uint ? ParamType::Int
                                                             : ParamType::Float;
      c.rows = e.rows;
      c.columns = e.cols;
      c.bytes = 4 * e.rows * e.cols * c.elements;
    }
    table->push_back(std::move(c));
  }
}

bool Compile(std::string_view source, const std::vector<Define>& defines,
             const std::string& entry, const std::string& profile, uint32_t flags,
             Program* out, std::string* errors) {
  Target target;
  if (!ParseProfile(profile, &target)) {
    *errors = "error: unknown profile " + profile;
    return false;
  }
  if (target.stage == Stage::Pixel && target.major == 1) {
    *errors = "error: ps_1_x profiles are not supported";
    return false;
  }
  // Lengths that applications pass often include the terminating NUL.
  source = source.substr(0, source.find('\0'));
  try {
    Unit unit = Parse(Preprocess(source, defines), flags);
    FuncDecl* e = Check(unit, entry, target);
    *out = Program{};
    out->stage = target.stage;
    out->major = target.major;
    out->minor = target.minor;
    AllocateRegisters(unit, target, &out->constants);
    EmitGlsl(unit, e, target, out);
  } catch (const Error& err) {
    *errors = "memory(" + std::to_string(err.loc.line) + "," + std::to_string(err.loc.col) +
              "): error: " + err.msg;
    return false;
  }
  return true;
}

// ---- Serialisation: little-endian words, strings as length + bytes.

namespace {

constexpr uint32_t kMagic = 0x534C4852;  // 'RHLS'
constexpr uint32_t kVersion = 1;

struct Writer {
  std::vector<uint8_t> b;
  void U32(uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
  }
  void Str(const std::string& s) {
    U32(uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
};

struct Reader {
  const uint8_t* p;
  size_t n, pos = 0;
  bool ok = true;
  uint32_t U32() {
    if (pos + 4 > n) {
      ok = false;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= uint32_t(p[pos + i]) << (8 * i);
    pos += 4;
    return v;
  }
  std::string Str() {
    uint32_t len = U32();
    if (!ok || pos + len > n) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p + pos), len);
    pos += len;
    return s;
  }
};

}  // namespace

std::vector<uint8_t> Serialize(const Program& p) {
  Writer w;
  w.U32(kMagic);
  w.U32(kVersion);
  w.U32(uint32_t(p.stage));
  w.U32(p.major);
  w.U32(p.minor);
  w.Str(p.glsl);
  w.U32(uint32_t(p.constants.size()));
  for (const auto& c : p.constants) {
    w.Str(c.name);
    for (uint32_t v : {uint32_t(c.set), c.index, c.count, uint32_t(c.cls), uint32_t(c.type),
                       c.rows, c.columns, c.elements, c.struct_members, c.bytes})
      w.U32(v);
  }
  w.U32(uint32_t(p.inputs.size()));
  for (const auto& s : p.inputs) {
    w.U32(s.usage);
    w.U32(s.index);
  }
  w.U32(uint32_t(p.varyings.size()));
  for (const auto& s : p.varyings) {
    w.U32(s.usage);
    w.U32(s.index);
  }
  w.U32(p.color_outputs);
  w.U32(uint32_t(p.writes_depth) | uint32_t(p.writes_point_size) << 1);
  w.U32(uint32_t(p.samplers.size()));
  for (const auto& s : p.samplers) {
    w.U32(s.reg);
    w.U32(uint32_t(s.type));
  }
  w.U32(p.float_regs);
  return std::move(w.b);
}

bool Deserialize(const uint8_t* data, size_t size, Program* p) {
  Reader r{data, size};
  if (r.U32() != kMagic || r.U32() != kVersion) return false;
  *p = Program{};
  p->stage = Stage(r.U32());
  p->major = r.U32();
  p->minor = r.U32();
  p->glsl = r.Str();
  uint32_t n = r.U32();
  for (uint32_t i = 0; i < n && r.ok; i++) {
    Constant c;
    c.name = r.Str();
    c.set = RegisterSet(r.U32());
    c.index = r.U32();
    c.count = r.U32();
    c.cls = ParamClass(r.U32());
    c.type = ParamType(r.U32());
    c.rows = r.U32();
    c.columns = r.U32();
    c.elements = r.U32();
    c.struct_members = r.U32();
    c.bytes = r.U32();
    p->constants.push_back(std::move(c));
  }
  n = r.U32();
  for (uint32_t i = 0; i < n && r.ok; i++) p->inputs.push_back(Semantic{r.U32(), r.U32()});
  n = r.U32();
  for (uint32_t i = 0; i < n && r.ok; i++) p->varyings.push_back(Semantic{r.U32(), r.U32()});
  p->color_outputs = r.U32();
  uint32_t f = r.U32();
  p->writes_depth = f & 1;
  p->writes_point_size = f & 2;
  n = r.U32();
  for (uint32_t i = 0; i < n && r.ok; i++) p->samplers.push_back(Sampler{r.U32(), ParamType(r.U32())});
  p->float_regs = r.U32();
  return r.ok && r.pos == size;
}

}  // namespace recomp::hlsl

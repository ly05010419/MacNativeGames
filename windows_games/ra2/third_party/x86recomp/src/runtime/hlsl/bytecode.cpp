// Compiled Direct3D 9 shaders (shader model 1.1-3.0 tokens) translated to
// the GLSL contract of hlsl.h, for games that ship their shaders compiled.
//
// Registers become GLSL variables: temps r_t[], inputs r_v[] (filled from
// r_fetch or the varyings), address a0 (r_a0), loop counter aL (r_aL),
// predicate p0 (r_p0), outputs per kind; constants read the register file
// c[] unless the shader defines them (def/defi/defb), and relative reads
// go through r_crel, which honours the definitions too. Every instruction
// computes a vec4 from its swizzled, modified sources, then applies the
// shift and saturate result modifiers, the write mask and predication.
// Co-issued pairs (ps_1_x) read their sources before either writes.
// Subroutines (label/call) become GLSL functions; flow control (rep, loop,
// if, break) maps onto GLSL's.
//
// The scalar instructions (rcp, rsq, exp, log, pow, ...) take the first
// component of the swizzled source, which compilers always replicate.
// As in the HLSL path, rsq, log and pow take |x| as the instructions do.
// ps_1_x: t# registers hold tex / texcoord results from TEXCOORD#, samplers
// are 2D (the stage's texture), r0 is the colour. The ps_1_x texture-matrix
// and bump instructions and ps_1_4 phases are not supported (named).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/hlsl/hlsl.h"

namespace recomp::hlsl {
namespace {

enum RegType : uint32_t {
  TEMP = 0, INPUT = 1, CONST = 2, ADDR = 3, TEXTURE = 3, RASTOUT = 4, ATTROUT = 5,
  TEXCRDOUT = 6, OUTPUT = 6, CONSTINT = 7, COLOROUT = 8, DEPTHOUT = 9, SAMPLER = 10,
  CONST2 = 11, CONST3 = 12, CONST4 = 13, CONSTBOOL = 14, LOOP = 15, TEMPFLOAT16 = 16,
  MISCTYPE = 17, LABEL = 18, PREDICATE = 19,
};

enum Op : uint32_t {
  NOP = 0, MOV, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST,
  LRP, FRC, M4X4, M4X3, M3X4, M3X3, M3X2, CALL, CALLNZ, LOOP_, RET, ENDLOOP, LABEL_, DCL, POW,
  CRS, SGN, ABS, NRM, SINCOS, REP, ENDREP, IF, IFC, ELSE, ENDIF, BREAK, BREAKC, MOVA, DEFB, DEFI,
  TEXCOORD = 64, TEXKILL, TEX, TEXBEM, TEXBEML, TEXREG2AR, TEXREG2GB, TEXM3X2PAD, TEXM3X2TEX,
  TEXM3X3PAD, TEXM3X3TEX, TEXM3X3SPEC = 76, TEXM3X3VSPEC, EXPP, LOGP, CND, DEF, TEXREG2RGB,
  TEXDP3TEX, TEXM3X2DEPTH, TEXDP3, TEXM3X3, TEXDEPTH, CMP, BEM, DP2ADD, DSX, DSY, TEXLDD, SETP,
  TEXLDL, BREAKP, PHASE = 0xFFFD, COMMENT = 0xFFFE, END = 0xFFFF,
};

struct Error {
  std::string msg;
};
[[noreturn]] void Fail(std::string msg) { throw Error{std::move(msg)}; }

uint32_t RegTypeOf(uint32_t tok) { return ((tok >> 28) & 7) | ((tok >> 8) & 0x18); }

struct Param {
  uint32_t type = 0, num = 0;
  uint32_t mask = 0xF;     // destination
  uint32_t swz = 0xE4;     // source: 2 bits per component (xyzw = 0xE4)
  uint32_t mod = 0;        // source modifier / destination result modifier
  int shift = 0;           // destination shift scale
  bool rel = false;
  uint32_t rel_type = ADDR, rel_num = 0, rel_comp = 0;  // relative address
};

struct Ins {
  uint32_t op = 0, ctl = 0;
  bool coissue = false, predicated = false, has_dst = false;
  Param dst, pred;
  std::vector<Param> src;
  uint32_t raw[4] = {};  // def/defi/defb values, dcl usage token
};

bool HasDst(uint32_t op) {
  switch (op) {
    case NOP: case CALL: case CALLNZ: case LOOP_: case RET: case ENDLOOP: case LABEL_: case REP:
    case ENDREP: case IF: case IFC: case ELSE: case ENDIF: case BREAK: case BREAKC: case BREAKP:
      return false;
    default:
      return true;
  }
}

const char* kComp = "xyzw";

std::string FloatBitsExpr(uint32_t bits) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "uintBitsToFloat(0x%08Xu)", bits);
  return buf;
}

class Translator {
 public:
  Translator(const uint32_t* t, size_t n, Program* out) : t_(t), n_(n), out_(out) {}

  void Run() {
    if (n_ < 2) Fail("shader too short");
    uint32_t v = t_[0];
    pixel_ = (v >> 16) == 0xFFFF;
    if (!pixel_ && (v >> 16) != 0xFFFE) Fail("not a shader version token");
    major_ = (v >> 8) & 0xFF;
    minor_ = v & 0xFF;
    if (major_ < 1 || major_ > 3) Fail("unsupported shader model");
    if (pixel_ && major_ == 1 && minor_ >= 4) Fail("ps_1_4 is not supported");
    out_->stage = pixel_ ? Stage::Pixel : Stage::Vertex;
    out_->major = major_;
    out_->minor = minor_;
    Decode();
    Declare();
    Emit();
  }

 private:
  // ---- decoding

  Param Dst(size_t& i) {
    uint32_t tok = t_[i++];
    Param p;
    p.type = RegTypeOf(tok);
    p.num = tok & 0x7FF;
    p.mask = (tok >> 16) & 0xF;
    p.mod = (tok >> 20) & 0xF;
    int s = int((tok >> 24) & 0xF);
    p.shift = s >= 8 ? s - 16 : s;
    p.rel = tok & 0x2000;
    if (p.rel && major_ >= 3) RelToken(i, &p);
    return p;
  }

  Param Src(size_t& i) {
    uint32_t tok = t_[i++];
    Param p;
    p.type = RegTypeOf(tok);
    p.num = tok & 0x7FF;
    p.swz = (tok >> 16) & 0xFF;
    p.mod = (tok >> 24) & 0xF;
    p.rel = tok & 0x2000;
    if (p.rel) {
      if (major_ >= 2) {
        RelToken(i, &p);
      } else {
        p.rel_type = ADDR;  // vs_1_1: a0.x
        p.rel_num = 0;
        p.rel_comp = 0;
      }
    }
    return p;
  }

  void RelToken(size_t& i, Param* p) {
    if (i >= n_) Fail("truncated relative address");
    uint32_t tok = t_[i++];
    p->rel_type = RegTypeOf(tok);
    p->rel_num = tok & 0x7FF;
    p->rel_comp = (tok >> 16) & 3;
  }

  void Decode() {
    size_t i = 1;
    while (i < n_) {
      uint32_t tok = t_[i];
      uint32_t op = tok & 0xFFFF;
      if (op == END) return;
      if (op == COMMENT) {
        i += 1 + ((tok >> 16) & 0x7FFF);
        continue;
      }
      Ins in;
      in.op = op;
      in.ctl = (tok >> 16) & 0xFF;
      in.coissue = tok & 0x40000000;
      in.predicated = tok & 0x10000000;
      size_t start = ++i, end;
      if (major_ >= 2) {
        end = start + ((tok >> 24) & 0xF);
      } else if (op == DEF || op == DEFI) {
        end = start + 5;
      } else if (op == DEFB) {
        end = start + 2;
      } else {
        end = start;
        while (end < n_ && (t_[end] & 0x80000000)) end++;
      }
      if (end > n_) Fail("truncated instruction");
      if (op == DEF || op == DEFI || op == DEFB) {
        in.has_dst = true;
        in.dst = Dst(i);
        for (int k = 0; k < (op == DEFB ? 1 : 4); k++) in.raw[k] = t_[i++];
      } else if (op == DCL) {
        in.raw[0] = t_[i++];
        in.has_dst = true;
        in.dst = Dst(i);
      } else {
        if (HasDst(op)) {
          in.has_dst = true;
          in.dst = Dst(i);
        }
        if (in.predicated) in.pred = Src(i);
        while (i < end) in.src.push_back(Src(i));
      }
      if (i != end) Fail("bad instruction length for opcode " + std::to_string(op));
      ins_.push_back(std::move(in));
    }
    Fail("shader without END");
  }

  // ---- declarations: registers used, inputs, outputs, samplers

  void Declare() {
    for (const Ins& in : ins_) {
      if (in.op == DEF) {
        defs_[ConstIndex(in.dst)] = {in.raw[0], in.raw[1], in.raw[2], in.raw[3]};
      } else if (in.op == DEFI) {
        defi_[in.dst.num] = {in.raw[0], in.raw[1], in.raw[2], in.raw[3]};
      } else if (in.op == DEFB) {
        defb_[in.dst.num] = in.raw[0] != 0;
      } else if (in.op == DCL) {
        Dcl(in);
      }
      auto note = [&](const Param& p, bool write) {
        if (p.type == TEMP) temps_ = std::max(temps_, p.num + 1);
        if (p.type == CONST || p.type == CONST2 || p.type == CONST3 || p.type == CONST4) {
          if (p.rel) rel_const_ = true;
          else if (!defs_.count(ConstIndex(p))) max_c_ = std::max(max_c_, ConstIndex(p) + 1);
        }
        if (p.type == CONSTINT || p.type == CONSTBOOL) uses_int_ = true;
        if (write && !pixel_ && (p.type == RASTOUT || p.type == ATTROUT || p.type == TEXCRDOUT))
          written_.insert({p.type, p.num});
        if (write && pixel_ && (p.type == COLOROUT || p.type == DEPTHOUT))
          written_.insert({p.type, p.num});
        if (!write && pixel_ && major_ < 3 && p.type == INPUT) ps_colors_.insert(p.num);
        if (!write && pixel_ && major_ == 2 && p.type == TEXTURE) ps_texcoords_.insert(p.num);
      };
      if (in.has_dst && in.op != DCL && in.op != DEF && in.op != DEFI && in.op != DEFB) note(in.dst, true);
      for (const Param& p : in.src) note(p, false);
      if (in.op == M4X4 || in.op == M4X3 || in.op == M3X4 || in.op == M3X3 || in.op == M3X2) {
        Param p = in.src[1];
        for (int k = 1; k < 4; k++) {
          p.num++;
          note(p, false);
        }
      }
      if (pixel_ && major_ == 1 && (in.op == TEX || in.op == TEXCOORD)) {
        ps_texcoords_.insert(in.dst.num);
        if (in.op == TEX) sampler_type_[in.dst.num] = ParamType::Sampler2D;
      }
    }
    if (rel_const_) max_c_ = pixel_ ? 224 : 256;
  }

  uint32_t ConstIndex(const Param& p) const {
    switch (p.type) {
      case CONST2: return p.num + 2048;
      case CONST3: return p.num + 4096;
      case CONST4: return p.num + 6144;
      default: return p.num;
    }
  }

  void Dcl(const Ins& in) {
    uint32_t u = in.raw[0];
    const Param& d = in.dst;
    if (d.type == SAMPLER) {
      uint32_t tt = (u >> 27) & 0xF;
      sampler_type_[d.num] = tt == 3 ? ParamType::SamplerCube
                             : tt == 4 ? ParamType::Sampler3D
                                       : ParamType::Sampler2D;
      return;
    }
    Semantic s{u & 0x1F, (u >> 16) & 0xF};
    if (!pixel_ && d.type == INPUT) {
      vs_inputs_[d.num] = s;
    } else if (!pixel_ && d.type == OUTPUT && major_ >= 3) {
      vs3_outputs_[d.num] = s;
    } else if (pixel_ && d.type == INPUT && major_ >= 3) {
      ps3_inputs_[d.num] = {s, d.mod};
    } else if (pixel_ && d.type == INPUT) {
      ps_colors_.insert(d.num);
      if (d.mod & 4) centroid_.insert({kColor, d.num});
    } else if (pixel_ && d.type == TEXTURE) {
      ps_texcoords_.insert(d.num);
      if (d.mod & 4) centroid_.insert({kTexcoord, d.num});
    } else if (pixel_ && d.type == MISCTYPE) {
      // vPos / vFace: read through the prelude.
    } else {
      Fail("unsupported dcl of register type " + std::to_string(d.type));
    }
  }

  // ---- expressions

  std::string IntReg(uint32_t n) {
    if (defi_.count(n)) {
      const auto& d = defi_[n];
      return "ivec4(" + std::to_string(int32_t(d[0])) + ", " + std::to_string(int32_t(d[1])) + ", " +
             std::to_string(int32_t(d[2])) + ", " + std::to_string(int32_t(d[3])) + ")";
    }
    uses_int_ = true;
    return "r_i[" + std::to_string(n) + "]";
  }

  std::string BoolReg(uint32_t n) {
    if (defb_.count(n)) return defb_[n] ? "true" : "false";
    uses_int_ = true;
    return "(r_b[" + std::to_string(n >> 2) + "][" + std::to_string(n & 3) + "] != 0)";
  }

  std::string RelIndex(const Param& p) {
    if (p.rel_type == LOOP) return "r_aL";
    if (p.rel_type == ADDR) return std::string("r_a0.") + kComp[p.rel_comp];
    Fail("unsupported relative address register");
  }

  // A register as a vec4 (no swizzle or modifier).
  std::string Reg(const Param& p) {
    switch (p.type) {
      case TEMP: return "r_t[" + std::to_string(p.num) + "]";
      case INPUT: return "r_v[" + std::to_string(p.num) + "]";
      case CONST: case CONST2: case CONST3: case CONST4: {
        uint32_t k = ConstIndex(p);
        if (p.rel) return "r_crel(" + RelIndex(p) + " + " + std::to_string(k) + ")";
        if (defs_.count(k)) return "r_dc" + std::to_string(k);
        return "c[" + std::to_string(k) + "]";
      }
      case TEXTURE:  // ps (ADDR in vertex shaders)
        if (!pixel_) return "vec4(r_a0)";
        return major_ == 1 ? "r_tx[" + std::to_string(p.num) + "]"
                           : "r_tc[" + std::to_string(p.num) + "]";
      case CONSTINT: return "vec4(" + IntReg(p.num) + ")";
      case CONSTBOOL: return "vec4(" + BoolReg(p.num) + ")";
      case LOOP: return "vec4(float(r_aL))";
      case MISCTYPE:
        if (p.num == 0) return "vec4(r_vpos(), 0.0, 0.0)";
        return "vec4(r_vface())";
      case PREDICATE: return "vec4(r_p0)";
      default: Fail("unsupported source register type " + std::to_string(p.type));
    }
  }

  static std::string Swizzle(uint32_t swz) {
    if (swz == 0xE4) return "";
    std::string s = ".";
    for (int i = 0; i < 4; i++) s += kComp[(swz >> (2 * i)) & 3];
    return s;
  }

  std::string S(const Param& p) {
    std::string x = Reg(p) + Swizzle(p.swz);
    switch (p.mod) {
      case 0: return x;
      case 1: return "(-" + x + ")";
      case 2: return "(" + x + " - 0.5)";
      case 3: return "(0.5 - " + x + ")";
      case 4: return "(2.0 * " + x + " - 1.0)";
      case 5: return "(1.0 - 2.0 * " + x + ")";
      case 6: return "(1.0 - " + x + ")";
      case 7: return "(2.0 * " + x + ")";
      case 8: return "(-2.0 * " + x + ")";
      case 9: return "(" + x + " / " + x + ".z)";
      case 10: return "(" + x + " / " + x + ".w)";
      case 11: return "abs(" + x + ")";
      case 12: return "(-abs(" + x + "))";
      default: Fail("unsupported source modifier " + std::to_string(p.mod));
    }
  }

  // First component of a source (scalar instructions).
  std::string S1(const Param& p) { return S(p) + ".x"; }

  // Comparison for ifc / breakc / setp (control bits).
  static const char* Cmp(uint32_t ctl) {
    switch (ctl & 7) {
      case 1: return ">";
      case 2: return "==";
      case 3: return ">=";
      case 4: return "<";
      case 5: return "!=";
      case 6: return "<=";
      default: Fail("bad comparison");
    }
  }

  // ---- emission

  std::string Ind() const { return std::string(size_t(indent_) * 2, ' '); }

  std::string& Out() { return func_ < 0 ? main_ : funcs_[func_]; }

  void Line(const std::string& s) { Out() += Ind() + s + "\n"; }

  std::string SamplerName(uint32_t n) {
    if (!sampler_type_.count(n)) sampler_type_[n] = ParamType::Sampler2D;
    used_samplers_.insert(n);
    return "r_s" + std::to_string(n);
  }

  // The value of an instruction as a vec4 (before result modifiers).
  std::string Value(const Ins& in) {
    const auto& s = in.src;
    auto need = [&](size_t n) {
      if (s.size() < n) Fail("opcode " + std::to_string(in.op) + " with too few sources");
    };
    switch (in.op) {
      case MOV: need(1); return S(s[0]);
      case MOVA: need(1); return S(s[0]);
      case ADD: need(2); return "(" + S(s[0]) + " + " + S(s[1]) + ")";
      case SUB: need(2); return "(" + S(s[0]) + " - " + S(s[1]) + ")";
      case MUL: need(2); return "(" + S(s[0]) + " * " + S(s[1]) + ")";
      case MAD: need(3); return "(" + S(s[0]) + " * " + S(s[1]) + " + " + S(s[2]) + ")";
      case MIN: need(2); return "min(" + S(s[0]) + ", " + S(s[1]) + ")";
      case MAX: need(2); return "max(" + S(s[0]) + ", " + S(s[1]) + ")";
      case SLT: need(2); return "vec4(lessThan(" + S(s[0]) + ", " + S(s[1]) + "))";
      case SGE: need(2); return "vec4(greaterThanEqual(" + S(s[0]) + ", " + S(s[1]) + "))";
      case RCP: need(1); return "vec4(1.0 / " + S1(s[0]) + ")";
      case RSQ: need(1); return "vec4(inversesqrt(abs(" + S1(s[0]) + ")))";
      case EXP: need(1); return "vec4(exp2(" + S1(s[0]) + "))";
      case LOG: need(1); return "vec4(log2(abs(" + S1(s[0]) + ")))";
      case EXPP:
        need(1);
        if (!pixel_ && major_ == 1) {
          helpers_.insert("expp");
          return "r_expp1(" + S1(s[0]) + ")";
        }
        return "vec4(exp2(" + S1(s[0]) + "))";
      case LOGP:
        need(1);
        if (!pixel_ && major_ == 1) {
          helpers_.insert("logp");
          return "r_logp1(" + S1(s[0]) + ")";
        }
        return "vec4(log2(abs(" + S1(s[0]) + ")))";
      case POW: need(2); return "vec4(pow(abs(" + S1(s[0]) + "), " + S1(s[1]) + "))";
      case DP3: need(2); return "vec4(dot((" + S(s[0]) + ").xyz, (" + S(s[1]) + ").xyz))";
      case DP4: need(2); return "vec4(dot(" + S(s[0]) + ", " + S(s[1]) + "))";
      case DP2ADD:
        need(3);
        return "vec4(dot((" + S(s[0]) + ").xy, (" + S(s[1]) + ").xy) + " + S1(s[2]) + ")";
      case LIT: need(1); helpers_.insert("lit"); return "r_lit(" + S(s[0]) + ")";
      case DST:
        need(2);
        return "vec4(1.0, (" + S(s[0]) + ").y * (" + S(s[1]) + ").y, (" + S(s[0]) + ").z, (" +
               S(s[1]) + ").w)";
      case LRP: need(3); return "mix(" + S(s[2]) + ", " + S(s[1]) + ", " + S(s[0]) + ")";
      case FRC: need(1); return "fract(" + S(s[0]) + ")";
      case ABS: need(1); return "abs(" + S(s[0]) + ")";
      case SGN: need(1); return "sign(" + S(s[0]) + ")";
      case CRS: need(2); return "vec4(cross((" + S(s[0]) + ").xyz, (" + S(s[1]) + ").xyz), 0.0)";
      case NRM: need(1); return "(" + S(s[0]) + " * inversesqrt(dot((" + S(s[0]) + ").xyz, (" + S(s[0]) + ").xyz)))";
      case SINCOS: need(1); return "vec4(cos(" + S1(s[0]) + "), sin(" + S1(s[0]) + "), 0.0, 0.0)";
      case CMP:
        need(3);
        return "mix(" + S(s[2]) + ", " + S(s[1]) + ", greaterThanEqual(" + S(s[0]) + ", vec4(0.0)))";
      case CND:
        need(3);
        return "mix(" + S(s[2]) + ", " + S(s[1]) + ", greaterThan(" + S(s[0]) + ", vec4(0.5)))";
      case DSX: need(1); return "dFdx(" + S(s[0]) + ")";
      case DSY: need(1); return "dFdy(" + S(s[0]) + ")";
      case M4X4: case M4X3: case M3X4: case M3X3: case M3X2: {
        need(2);
        int rows = in.op == M4X4 || in.op == M3X4 ? 4 : in.op == M3X2 ? 2 : 3;
        bool three = in.op == M3X4 || in.op == M3X3 || in.op == M3X2;
        std::string a = S(s[0]);
        std::string r = "vec4(";
        for (int k = 0; k < 4; k++) {
          if (k) r += ", ";
          if (k >= rows) {
            r += "0.0";
            continue;
          }
          Param row = s[1];
          row.num += uint32_t(k);
          std::string b = S(row);
          r += three ? "dot((" + a + ").xyz, (" + b + ").xyz)" : "dot(" + a + ", " + b + ")";
        }
        return r + ")";
      }
      case TEX: {  // texld (ps_2+); ps_1_x handled in Instruction
        need(2);
        ParamType ty = sampler_type_.count(s[1].num) ? sampler_type_[s[1].num] : ParamType::Sampler2D;
        std::string smp = SamplerName(s[1].num), c = S(s[0]);
        std::string co = ty == ParamType::Sampler2D ? "(" + c + ").xy" : "(" + c + ").xyz";
        // The sampler's LOD bias (R_BIAS, hlsl.h); texld is pixel-only.
        std::string b = "R_BIAS(" + std::to_string(s[1].num) + ")";
        if (in.ctl == 1) {  // texldp
          if (ty == ParamType::Sampler2D) return "textureProj(" + smp + ", " + c + ", " + b + ")";
          return "texture(" + smp + ", (" + c + ").xyz / (" + c + ").w, " + b + ")";
        }
        if (in.ctl == 2)  // texldb
          return "texture(" + smp + ", " + co + ", (" + c + ").w + " + b + ")";
        return "texture(" + smp + ", " + co + ", " + b + ")";
      }
      case TEXLDL: {
        need(2);
        ParamType ty = sampler_type_.count(s[1].num) ? sampler_type_[s[1].num] : ParamType::Sampler2D;
        std::string smp = SamplerName(s[1].num), c = S(s[0]);
        std::string co = ty == ParamType::Sampler2D ? "(" + c + ").xy" : "(" + c + ").xyz";
        return "textureLod(" + smp + ", " + co + ", (" + c + ").w)";
      }
      case TEXLDD: {
        need(4);
        ParamType ty = sampler_type_.count(s[1].num) ? sampler_type_[s[1].num] : ParamType::Sampler2D;
        std::string smp = SamplerName(s[1].num);
        const char* sw = ty == ParamType::Sampler2D ? ".xy" : ".xyz";
        return "textureGrad(" + smp + ", (" + S(s[0]) + ")" + sw + ", (" + S(s[2]) + ")" + sw +
               ", (" + S(s[3]) + ")" + sw + ")";
      }
      default:
        Fail("unsupported opcode " + std::to_string(in.op) + " in " +
             std::string(pixel_ ? "ps_" : "vs_") + std::to_string(major_) + "_" +
             std::to_string(minor_));
    }
  }

  // The destination as an assignable name.
  std::string DstName(const Param& p) {
    switch (p.type) {
      case TEMP: return "r_t[" + std::to_string(p.num) + "]";
      case TEXTURE:
        if (!pixel_) return "r_a0";
        if (major_ == 1) return "r_tx[" + std::to_string(p.num) + "]";
        break;
      case RASTOUT:
        if (p.num == 0) return "r_opos";
        if (p.num == 1) return "r_ofog";
        return "r_opts";
      case ATTROUT: return "r_od[" + std::to_string(p.num) + "]";
      case TEXCRDOUT:  // OUTPUT in vs_3_0
        if (major_ >= 3) {
          if (p.rel) Fail("relative output registers are not supported");
          return "r_o[" + std::to_string(p.num) + "]";
        }
        return "r_ot[" + std::to_string(p.num) + "]";
      case COLOROUT: return "r_oc[" + std::to_string(p.num) + "]";
      case DEPTHOUT: return "r_odepth";
      case PREDICATE: return "r_p0";
      default: break;
    }
    Fail("unsupported destination register type " + std::to_string(p.type));
  }

  static std::string Mask(uint32_t m) {
    std::string s;
    for (int i = 0; i < 4; i++)
      if (m & (1u << i)) s += kComp[i];
    return s;
  }

  // Applies result modifiers to the value `v` (a variable name) and
  // writes it to the destination, honouring predication.
  void Write(const Ins& in, const std::string& v) {
    const Param& d = in.dst;
    std::string x = v;
    if (d.shift > 0) x = "(" + x + " * " + std::to_string(1 << d.shift) + ".0)";
    if (d.shift < 0) x = "(" + x + " / " + std::to_string(1 << -d.shift) + ".0)";
    if (d.mod & 1) x = "clamp(" + x + ", 0.0, 1.0)";
    std::string name = DstName(d);
    uint32_t mask = d.mask ? d.mask : 0xF;
    std::string m = Mask(mask);
    bool full = mask == 0xF;
    if (d.type == TEXTURE && !pixel_) {
      // a0: vs_1_1 mov truncates toward -inf; mova rounds to nearest.
      std::string r = in.op == MOVA ? "floor(" + x + " + 0.5)" : "floor(" + x + ")";
      x = "ivec4(" + r + ")";
    }
    if (in.predicated) {
      std::string pr = "r_p0" + Swizzle(in.pred.swz);
      if (in.pred.mod == 13) pr = "not(" + pr + ")";
      for (int i = 0; i < 4; i++)
        if (mask & (1u << i))
          Line("if ((" + pr + ")." + kComp[i] + ") " + name + "." + kComp[i] + " = (" + x + ")." + kComp[i] + ";");
      return;
    }
    if (full)
      Line(name + " = " + x + ";");
    else
      Line(name + "." + m + " = (" + x + ")." + m + ";");
  }

  void Instruction(size_t idx) {
    const Ins& in = ins_[idx];
    switch (in.op) {
      case NOP: case DCL: case DEF: case DEFI: case DEFB: return;
      case PHASE: Fail("ps_1_4 phases are not supported");
      case REP: {
        int k = loops_++;
        Line("for (int r_rep" + std::to_string(k) + " = 0; r_rep" + std::to_string(k) + " < " +
             IntReg(in.src[0].num) + ".x; r_rep" + std::to_string(k) + "++) {");
        loop_kinds_.push_back(false);
        indent_++;
        return;
      }
      case LOOP_: {
        // loop aL, i#: aL starts at i.y and steps by i.z, i.x times; the
        // enclosing loop's aL is restored after.
        int k = loops_++;
        std::string ks = std::to_string(k), iv = IntReg(in.src[1].num);
        Line("{");
        indent_++;
        Line("int r_saved_aL" + ks + " = r_aL;");
        Line("ivec4 r_li" + ks + " = " + iv + ";");
        Line("r_aL = r_li" + ks + ".y;");
        Line("for (int r_n" + ks + " = 0; r_n" + ks + " < r_li" + ks + ".x; r_n" + ks + "++, r_aL += r_li" + ks + ".z) {");
        indent_++;
        loop_kinds_.push_back(true);
        loop_ids_.push_back(k);
        return;
      }
      case ENDREP: case ENDLOOP: {
        if (loop_kinds_.empty()) Fail("unbalanced end of loop");
        bool aloop = loop_kinds_.back();
        loop_kinds_.pop_back();
        indent_--;
        Line("}");
        if (aloop) {
          int k = loop_ids_.back();
          loop_ids_.pop_back();
          Line("r_aL = r_saved_aL" + std::to_string(k) + ";");
          indent_--;
          Line("}");
        }
        return;
      }
      case IF:
        if (in.src[0].type == PREDICATE) {
          std::string p = "r_p0" + Swizzle(in.src[0].swz) + ".x";
          Line(std::string("if (") + (in.src[0].mod == 13 ? "!" : "") + p + ") {");
        } else {
          Line("if (" + BoolReg(in.src[0].num) + ") {");
        }
        indent_++;
        return;
      case IFC:
        Line("if (" + S1(in.src[0]) + " " + Cmp(in.ctl) + " " + S1(in.src[1]) + ") {");
        indent_++;
        return;
      case ELSE:
        indent_--;
        Line("} else {");
        indent_++;
        return;
      case ENDIF:
        indent_--;
        Line("}");
        return;
      case BREAK: Line("break;"); return;
      case BREAKC:
        Line("if (" + S1(in.src[0]) + " " + Cmp(in.ctl) + " " + S1(in.src[1]) + ") break;");
        return;
      case BREAKP: {
        std::string p = "r_p0" + Swizzle(in.src[0].swz) + ".x";
        Line(std::string("if (") + (in.src[0].mod == 13 ? "!" : "") + p + ") break;");
        return;
      }
      case CALL: Line("r_l" + std::to_string(in.src[0].num) + "();"); return;
      case CALLNZ: {
        const Param& b = in.src[1];
        std::string cond = b.type == PREDICATE
                               ? std::string(b.mod == 13 ? "!" : "") + "r_p0" + Swizzle(b.swz) + ".x"
                               : BoolReg(b.num);
        Line("if (" + cond + ") r_l" + std::to_string(in.src[0].num) + "();");
        return;
      }
      case RET:
        if (func_ < 0) {
          main_done_ = true;
        } else {
          indent_--;
          Line("}");
          func_ = -1;
          indent_ = 1;
        }
        return;
      case LABEL_: {
        if (func_ >= 0) Fail("label inside a subroutine");
        func_ = int(in.src[0].num);
        labels_.insert(in.src[0].num);
        indent_ = 0;
        Line("void r_l" + std::to_string(func_) + "() {");
        indent_ = 1;
        return;
      }
      case TEXKILL: {
        std::string v = pixel_ && major_ == 1 ? Reg(in.dst) : DstName(in.dst);
        const char* sw = major_ == 1 ? ".xyz" : "";
        Line(std::string("if (any(lessThan(") + v + sw + ", " + (major_ == 1 ? "vec3" : "vec4") + "(0.0)))) discard;");
        return;
      }
      case SETP: {
        std::string m = Mask(in.dst.mask ? in.dst.mask : 0xF);
        static const char* const kFn[] = {"", "greaterThan", "equal", "greaterThanEqual", "lessThan",
                                          "notEqual", "lessThanEqual"};
        uint32_t ctl = in.ctl & 7;
        if (ctl < 1 || ctl > 6) Fail("bad setp comparison");
        Line("r_p0." + m + " = " + kFn[ctl] + "(" + S(in.src[0]) + ", " + S(in.src[1]) + ")." + m + ";");
        return;
      }
      default: break;
    }
    if (pixel_ && major_ == 1 && in.op == TEX) {
      std::string n = std::to_string(in.dst.num);
      Line("r_tx[" + n + "] = texture(" + SamplerName(in.dst.num) + ", r_tc[" + n + "].xy, R_BIAS(" +
           n + "));");
      return;
    }
    if (pixel_ && major_ == 1 && in.op == TEXCOORD) {
      std::string n = std::to_string(in.dst.num);
      Line("r_tx[" + n + "] = vec4(clamp(r_tc[" + n + "].xyz, 0.0, 1.0), 1.0);");
      return;
    }
    switch (in.op) {
      case TEXBEM: case TEXBEML: case TEXREG2AR: case TEXREG2GB: case TEXM3X2PAD: case TEXM3X2TEX:
      case TEXM3X3PAD: case TEXM3X3TEX: case TEXM3X3SPEC: case TEXM3X3VSPEC: case TEXREG2RGB:
      case TEXDP3TEX: case TEXM3X2DEPTH: case TEXDP3: case TEXM3X3: case TEXDEPTH: case BEM:
        Fail("ps_1_x texture instruction " + std::to_string(in.op) + " is not supported");
      default:
        break;
    }
    // Co-issued pairs read their sources before either writes.
    if (idx + 1 < ins_.size() && ins_[idx + 1].coissue) return;  // emitted with its partner
    if (in.coissue && idx > 0) {
      const Ins& first = ins_[idx - 1];
      Line("{");
      indent_++;
      Line("vec4 r_va = " + Value(first) + ";");
      Line("vec4 r_vb = " + Value(in) + ";");
      Write(first, "r_va");
      Write(in, "r_vb");
      indent_--;
      Line("}");
      return;
    }
    Line("{");
    indent_++;
    Line("vec4 r_va = " + Value(in) + ";");
    Write(in, "r_va");
    indent_--;
    Line("}");
  }

  // ---- interface

  std::string Varying(Semantic s, bool out, uint32_t mods = 0) {
    int l = VaryingLocation(s);
    if (l < 0) Fail("semantic usage " + std::to_string(s.usage) + " cannot be a varying");
    std::string name = "r_var" + std::to_string(l);
    if (declared_.insert(name).second) {
      std::string q = (mods & 4) ? "centroid " : "";
      io_ += "layout(location = " + std::to_string(l) + ") " + q + (out ? "out" : "in") +
             " vec4 " + name + ";\n";
      out_->varyings.push_back(s);
    }
    return name;
  }

  void Emit() {
    indent_ = 1;
    for (size_t i = 0; i < ins_.size(); i++) {
      if (main_done_ && ins_[i].op != LABEL_ && func_ < 0) continue;
      Instruction(i);
    }
    if (func_ >= 0) {
      indent_ = 0;
      Line("}");
      func_ = -1;
    }
    if (!loop_kinds_.empty()) Fail("unterminated loop");

    std::string h = std::string("// ") + (pixel_ ? "ps_" : "vs_") + std::to_string(major_) + "_" +
                    std::to_string(minor_) + ", translated from shader bytecode\n";
    if (max_c_) {
      h += "layout(std140, R_FLOAT_BINDING) uniform r_float_regs { vec4 c[" + std::to_string(max_c_) +
           "]; };\n";
      out_->float_regs = max_c_;
    }
    if (uses_int_)
      h += "layout(std140, R_INT_BINDING) uniform r_int_regs { ivec4 r_i[16]; ivec4 r_b[4]; };\n";
    for (uint32_t n : used_samplers_) {
      ParamType t = sampler_type_[n];
      const char* ty = t == ParamType::SamplerCube ? "samplerCube"
                       : t == ParamType::Sampler3D ? "sampler3D"
                                                   : "sampler2D";
      h += "layout(R_SAMPLER_BINDING(" + std::to_string(n) + ")) uniform " + ty + " r_s" +
           std::to_string(n) + ";\n";
      out_->samplers.push_back(Sampler{n, t});
    }

    // main: inputs, definitions, the body, outputs.
    std::string pre, post;
    for (const auto& [k, d] : defs_)
      pre += "  r_dc" + std::to_string(k) + " = vec4(" + FloatBitsExpr(d[0]) + ", " + FloatBitsExpr(d[1]) +
             ", " + FloatBitsExpr(d[2]) + ", " + FloatBitsExpr(d[3]) + ");\n";
    std::string g;  // globals
    g += "vec4 r_t[" + std::to_string(std::max(temps_, 1u)) + "];\n";
    g += "ivec4 r_a0;\nint r_aL;\nbvec4 r_p0;\n";
    for (const auto& [k, d] : defs_) g += "vec4 r_dc" + std::to_string(k) + ";\n";
    if (!pixel_) {
      g += "vec4 r_v[16];\n";
      for (const auto& [reg, s] : vs_inputs_) {
        size_t slot = out_->inputs.size();
        out_->inputs.push_back(s);
        pre += "  r_v[" + std::to_string(reg) + "] = r_fetch(" + std::to_string(slot) + ");\n";
      }
      if (major_ >= 3) {
        g += "vec4 r_o[12];\n";
        pre += "  for (int i = 0; i < 12; i++) r_o[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
        for (const auto& [reg, s] : vs3_outputs_) {
          std::string v = "r_o[" + std::to_string(reg) + "]";
          if (s.usage == kPosition && s.index == 0) {
            post += "  gl_Position = r_position(" + v + ");\n";
          } else if (s.usage == kPSize) {
            post += "  gl_PointSize = " + v + ".x;\n";
            out_->writes_point_size = true;
          } else if (s.usage == kFog) {
            post += "  " + Varying(s, true) + " = vec4(" + v + ".x, 0.0, 0.0, 1.0);\n";
          } else {
            post += "  " + Varying(s, true) + " = " + v + ";\n";
          }
        }
      } else {
        g += "vec4 r_opos, r_ofog, r_opts;\nvec4 r_od[2];\nvec4 r_ot[8];\n";
        pre += "  r_opos = vec4(0.0); r_ofog = vec4(0.0); r_opts = vec4(1.0);\n";
        pre += "  for (int i = 0; i < 2; i++) r_od[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
        pre += "  for (int i = 0; i < 8; i++) r_ot[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
        post += "  gl_Position = r_position(r_opos);\n";
        for (const auto& [type, num] : written_) {
          if (type == RASTOUT && num == 1) {
            post += "  " + Varying({kFog, 0}, true) + " = vec4(r_ofog.x, 0.0, 0.0, 1.0);\n";
          } else if (type == RASTOUT && num == 2) {
            post += "  gl_PointSize = r_opts.x;\n";
            out_->writes_point_size = true;
          } else if (type == ATTROUT) {
            // Colour outputs are clamped to [0, 1] before shader model 3.
            post += "  " + Varying({kColor, num}, true) + " = clamp(r_od[" + std::to_string(num) +
                    "], 0.0, 1.0);\n";
          } else if (type == TEXCRDOUT) {
            post += "  " + Varying({kTexcoord, num}, true) + " = r_ot[" + std::to_string(num) + "];\n";
          }
        }
      }
    } else {
      g += "vec4 r_v[16];\nvec4 r_oc[4];\nvec4 r_odepth;\n";
      if (major_ >= 3) {
        for (const auto& [reg, sm] : ps3_inputs_) {
          const Semantic& s = sm.first;
          std::string v = "r_v[" + std::to_string(reg) + "]";
          if (s.usage == kPosition)
            pre += "  " + v + " = vec4(0.0);\n";
          else
            pre += "  " + v + " = " + Varying(s, false, sm.second) + ";\n";
        }
      } else {
        for (uint32_t n : ps_colors_)
          pre += "  r_v[" + std::to_string(n) + "] = " +
                 Varying({kColor, n}, false, centroid_.count({kColor, n}) ? 4 : 0) + ";\n";
        g += major_ == 1 ? "vec4 r_tx[8];\n" : "";
        g += "vec4 r_tc[8];\n";
        for (uint32_t n : ps_texcoords_)
          pre += "  r_tc[" + std::to_string(n) + "] = " +
                 Varying({kTexcoord, n}, false, centroid_.count({kTexcoord, n}) ? 4 : 0) + ";\n";
      }
      // Outputs: r0 in ps_1_x, else what was written.
      std::vector<uint32_t> colors;
      if (major_ == 1) {
        colors.push_back(0);
        post += "  r_oc[0] = r_t[0];\n";
      }
      for (const auto& [type, num] : written_)
        if (type == COLOROUT) colors.push_back(num);
      for (uint32_t n : colors) {
        if (n > 3) Fail("bad colour output");
        if (out_->color_outputs & (1u << n)) continue;
        out_->color_outputs |= 1u << n;
        io_ += "layout(location = " + std::to_string(n) + ") out vec4 r_c" + std::to_string(n) + ";\n";
        post += "  r_c" + std::to_string(n) + " = r_oc[" + std::to_string(n) + "];\n";
      }
      for (const auto& [type, num] : written_)
        if (type == DEPTHOUT) {
          post += "  gl_FragDepth = r_odepth.x;\n";
          out_->writes_depth = true;
        }
      if (out_->color_outputs & 1) post += "  r_color0(r_c0);\n";
    }
    pre = "  for (int i = 0; i < " + std::to_string(std::max(temps_, 1u)) + "; i++) r_t[i] = vec4(0.0);\n" + pre;

    std::string help;
    if (rel_const_) {
      help += "vec4 r_crel(int i) {\n";
      for (const auto& [k, d] : defs_)
        help += "  if (i == " + std::to_string(k) + ") return r_dc" + std::to_string(k) + ";\n";
      help += "  return i >= 0 && i < " + std::to_string(max_c_) + " ? c[i] : vec4(0.0);\n}\n";
    }
    if (helpers_.count("lit"))
      help += "vec4 r_lit(vec4 s) {\n"
              "  float p = clamp(s.w, -128.0, 128.0);\n"
              "  return vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? pow(max(s.y, 0.0), p) : 0.0, 1.0);\n"
              "}\n";
    if (helpers_.count("expp"))
      help += "vec4 r_expp1(float s) { return vec4(exp2(floor(s)), fract(s), exp2(s), 1.0); }\n";
    if (helpers_.count("logp"))
      help += "vec4 r_logp1(float s) {\n"
              "  float a = abs(s), e = floor(log2(a));\n"
              "  return vec4(e, a / exp2(e), log2(a), 1.0);\n"
              "}\n";
    std::string protos;
    for (uint32_t l : labels_) protos += "void r_l" + std::to_string(l) + "();\n";
    std::string funcs;
    for (const auto& [l, body] : funcs_) funcs += body;

    out_->glsl = h + io_ + "\n" + g + help + protos + funcs + "\nvoid main() {\n" + pre + main_ +
                 post + "}\n";
  }

  const uint32_t* t_;
  size_t n_;
  Program* out_;
  bool pixel_ = false;
  uint32_t major_ = 0, minor_ = 0;
  std::vector<Ins> ins_;

  std::map<uint32_t, std::array<uint32_t, 4>> defs_, defi_;
  std::map<uint32_t, bool> defb_;
  uint32_t temps_ = 0, max_c_ = 0;
  bool rel_const_ = false, uses_int_ = false;
  std::map<uint32_t, Semantic> vs_inputs_, vs3_outputs_;
  std::map<uint32_t, std::pair<Semantic, uint32_t>> ps3_inputs_;  // semantic, dcl modifiers
  std::set<uint32_t> ps_colors_, ps_texcoords_;
  std::set<std::pair<uint32_t, uint32_t>> written_, centroid_;
  std::map<uint32_t, ParamType> sampler_type_;
  std::set<uint32_t> used_samplers_;
  std::set<std::string> helpers_;

  std::string main_, io_;
  std::map<int, std::string> funcs_;
  std::set<uint32_t> labels_;
  std::set<std::string> declared_;
  int func_ = -1, indent_ = 1, loops_ = 0;
  bool main_done_ = false;
  std::vector<bool> loop_kinds_;  // true: loop (aL), false: rep
  std::vector<int> loop_ids_;
};

}  // namespace

bool TranslateBytecode(const uint32_t* tokens, size_t count, Program* out, std::string* errors) {
  *out = Program{};
  try {
    Translator(tokens, count, out).Run();
  } catch (const Error& e) {
    *errors = "bytecode: " + e.msg;
    return false;
  }
  return true;
}

}  // namespace recomp::hlsl

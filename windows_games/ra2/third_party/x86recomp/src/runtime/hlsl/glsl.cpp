// GLSL emission from the checked tree.
//
// Matrices: an HLSL floatRxC is a GLSL matRxC whose columns are the HLSL
// rows, so m[i][j] means the same in both languages, m[i] is row i, and
// GLSL constructors take components in HLSL's (row) order. Products swap:
// mul(v, M) is M * v, mul(M, v) is v * M and mul(A, B) is B * A. The
// default column_major packing puts a matrix column in each constant
// register, so reading one transposes (r_cm*); row_major ones read as is
// (r_rm*).
//
// D3D9 shader instructions take the absolute value where GLSL's
// functions are undefined: log, rsq (so sqrt, which the compiler builds
// from rsq and rcp) and pow (log, mul, exp). The translation does the
// same.

#include <algorithm>
#include <cstdio>
#include <set>

#include "runtime/hlsl/ast.h"

namespace recomp::hlsl {
namespace {

std::string Mangle(const char* prefix, const std::string& name) {
  // GLSL reserves identifiers containing "__".
  std::string r = std::string(prefix) + name;
  for (size_t p = r.find("__"); p != std::string::npos; p = r.find("__", p)) r.replace(p, 2, "_0");
  return r;
}

const char* kComps = "xyzw";

std::string Swz(int n) { return n == 4 ? "" : "." + std::string(kComps, n); }

std::string FloatLit(double v) {
  char buf[64];
  if (v == double(int64_t(v)) && v > -1e15 && v < 1e15) {
    std::snprintf(buf, sizeof buf, "%.1f", v);
  } else {
    std::snprintf(buf, sizeof buf, "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    if (s == "inf") return "(1.0 / 0.0)";
    if (s == "-inf") return "(-1.0 / 0.0)";
    return v < 0 ? "(" + s + ")" : s;
  }
  return v < 0 ? "(" + std::string(buf) + ")" : buf;
}

bool SideEffects(const Expr& e) {
  if (e.kind == Expr::Assign || e.kind == Expr::Call) return true;
  if (e.kind == Expr::Unary && (e.op == Op::PreInc || e.op == Op::PreDec ||
                                e.op == Op::PostInc || e.op == Op::PostDec))
    return true;
  for (const auto& a : e.args)
    if (SideEffects(*a)) return true;
  return false;
}

class Emitter {
 public:
  Emitter(Unit& u, FuncDecl* entry, const Target& t, Program* out)
      : u_(u), entry_(entry), tg_(t), out_(out) {}

  void Run() {
    NameEverything();
    std::string funcs;
    std::vector<FuncDecl*> reach;
    Reachable(entry_, &reach);
    for (FuncDecl* f : reach) funcs += Signature(*f) + ";\n";
    funcs += "\n";
    for (auto& f : u_.funcs)
      if (std::find(reach.begin(), reach.end(), f.get()) != reach.end()) funcs += Function(*f);
    std::string init = GlobalInit();
    std::string main = Main();

    std::string h = "// " + std::string(tg_.stage == Stage::Vertex ? "vs_" : "ps_") +
                    std::to_string(tg_.major) + "_" + std::to_string(tg_.minor) + " " +
                    entry_->name + ", translated from HLSL\n";
    if (max_c_) {
      h += "layout(std140, R_FLOAT_BINDING) uniform r_float_regs { vec4 c[" +
           std::to_string(max_c_) + "]; };\n";
      out_->float_regs = max_c_;
    }
    if (uses_int_) {
      h += "layout(std140, R_INT_BINDING) uniform r_int_regs { ivec4 r_i[16]; ivec4 r_b[4]; };\n";
    }
    for (auto& [reg, dim] : samplers_) {
      const char* ty = dim == Dim::D3 ? "sampler3D" : dim == Dim::Cube ? "samplerCube" : "sampler2D";
      h += "layout(R_SAMPLER_BINDING(" + std::to_string(reg) + ")) uniform " + ty + " r_s" +
           std::to_string(reg) + ";\n";
      out_->samplers.push_back(Sampler{reg, dim == Dim::D3     ? ParamType::Sampler3D
                                            : dim == Dim::Cube ? ParamType::SamplerCube
                                                               : ParamType::Sampler2D});
    }
    h += io_;
    h += "\n";
    for (auto& s : u_.structs) {
      if (!s->used) continue;
      h += "struct " + s->glsl + " {\n";
      for (auto& f : s->fields) h += "  " + Decl(f->type, f->glsl) + ";\n";
      h += "};\n";
    }
    for (auto& g : u_.globals) {
      if (!g->used) continue;
      if (g->cls == VarDecl::Class::Const) {
        h += "const " + Decl(g->type, g->glsl) + " = " + Init(*g->init, g->type) + ";\n";
      } else if (g->cls == VarDecl::Class::Private) {
        h += Decl(g->type, g->glsl) + ";\n";
      }
    }
    h += "\n";
    for (const auto& name : helper_order_) h += helpers_[name];
    out_->glsl = h + "\n" + funcs + init + main;
  }

 private:
  // ---- Names

  void NameEverything() {
    int anon = 0;
    for (auto& s : u_.structs) {
      s->glsl = s->name.empty() ? "s_anon" + std::to_string(anon++) : Mangle("s_", s->name);
      for (auto& f : s->fields) f->glsl = Mangle("h_", f->name);
    }
    for (auto& g : u_.globals) g->glsl = Mangle("h_", g->name);
    std::map<std::string, int> overloads;
    for (auto& f : u_.funcs) {
      if (!f->body) continue;
      int k = overloads[f->name]++;
      f->glsl = Mangle("f_", f->name) + (k ? "_" + std::to_string(k) : "");
      for (auto& p : f->params) p->glsl = Mangle("h_", p->name);
    }
  }

  void Reachable(FuncDecl* f, std::vector<FuncDecl*>* out) {
    if (std::find(out->begin(), out->end(), f) != out->end()) return;
    for (FuncDecl* c : f->callees) Reachable(c, out);
    out->push_back(f);
  }

  // ---- Types

  std::string TypeOf(const Type& t, Loc loc = {}) {
    std::string s;
    switch (t.base) {
      case Base::Void: return "void";
      case Base::Struct: return t.st->glsl;
      case Base::Sampler:
        return t.dim == Dim::D3 ? "sampler3D" : t.dim == Dim::Cube ? "samplerCube" : "sampler2D";
      case Base::Bool: s = t.cols > 1 ? "bvec" : "bool"; break;
      case Base::Int: s = t.cols > 1 ? "ivec" : "int"; break;
      case Base::Uint: s = t.cols > 1 ? "uvec" : "uint"; break;
      case Base::Half: case Base::Float: case Base::Double:
        if (t.matrix) {
          if (t.rows == 1 || t.cols == 1) Fail(loc, TypeName(t) + " matrices are not supported");
          return t.rows == t.cols ? "mat" + std::to_string(t.rows)
                                  : "mat" + std::to_string(t.rows) + "x" + std::to_string(t.cols);
        }
        s = t.cols > 1 ? "vec" : "float";
        break;
      default:
        Fail(loc, "type " + TypeName(t) + " has no GLSL equivalent");
    }
    if (t.matrix) Fail(loc, TypeName(t) + " matrices are not supported");
    if (t.cols > 1) s += std::to_string(t.cols);
    return s;
  }

  std::string Decl(const Type& t, const std::string& name) {
    return TypeOf(t.Elem()) + " " + name + (t.array ? "[" + std::to_string(t.array) + "]" : "");
  }

  std::string Zero(const Type& t) {
    if (t.array) {
      std::string s = TypeOf(t.Elem()) + "[" + std::to_string(t.array) + "](";
      for (uint32_t i = 0; i < t.array; i++) s += (i ? ", " : "") + Zero(t.Elem());
      return s + ")";
    }
    if (t.base == Base::Struct) {
      std::string s = t.st->glsl + "(";
      for (size_t i = 0; i < t.st->fields.size(); i++)
        s += (i ? ", " : "") + Zero(t.st->fields[i]->type);
      return s + ")";
    }
    if (t.base == Base::Bool) return TypeOf(t) + "(false)";
    if (t.base == Base::Int) return TypeOf(t) + "(0)";
    if (t.base == Base::Uint) return TypeOf(t) + "(0u)";
    return TypeOf(t) + "(0.0)";  // matrices too: all zero
  }

  // ---- Helpers emitted on first use

  void Helper(const std::string& name, const std::string& code) {
    if (helpers_.count(name)) return;
    helpers_[name] = code;
    helper_order_.push_back(name);
  }

  // Overloads of a componentwise helper for float and vec2..vec4.
  void GenHelper(const std::string& name, const std::string& pattern) {
    if (helpers_.count(name)) return;
    std::string code;
    for (const char* t : {"float", "vec2", "vec3", "vec4"}) {
      std::string s = pattern;
      for (size_t p = s.find("$T"); p != std::string::npos; p = s.find("$T", p)) s.replace(p, 2, t);
      code += s;
    }
    Helper(name, code);
  }

  // ---- Uniforms (constant registers)

  uint32_t RegsPerElem(const VarDecl& v) const {
    const Type& t = v.type;
    if (t.Elem().matrix) return (v.flags & kRowMajor) ? t.rows : t.cols;
    return 1;
  }

  // Element `idx` (or the whole variable when idx is empty and it is not
  // an array) of a uniform.
  std::string UniformElem(VarDecl& v, const std::string& idx, Loc loc) {
    Type t = v.type.Elem();
    uint32_t stride = RegsPerElem(v);
    std::string r;
    if (idx.empty()) {
      r = std::to_string(v.reg_index);
    } else {
      r = "(" + std::to_string(v.reg_index) + " + " +
          (stride == 1 ? "" : std::to_string(stride) + " * ") + "(" + idx + "))";
    }
    switch (v.set) {
      case RegisterSet::Sampler: {
        if (!idx.empty() && idx.find_first_not_of("0123456789") != std::string::npos)
          Fail(loc, "samplers can only be indexed with constants");
        uint32_t reg = v.reg_index + (idx.empty() ? 0 : uint32_t(std::stoul(idx)));
        Dim d = v.sampler_dim == Dim::Any ? Dim::D2 : v.sampler_dim;
        samplers_[reg] = d;
        return "r_s" + std::to_string(reg);
      }
      case RegisterSet::Bool:
        uses_int_ = true;
        if (t.cols > 1) Fail(loc, "bool vectors in bool registers are not supported");
        return "(r_b[" + r + " >> 2][" + r + " & 3] != 0)";
      case RegisterSet::Int4:
        uses_int_ = true;
        return TypeOf(t) + "(r_i[" + r + "]" + Swz(t.cols) + ")";
      case RegisterSet::Float4:
        break;
    }
    max_c_ = std::max(max_c_, v.reg_index + v.reg_count);
    if (t.matrix) {
      bool row = v.flags & kRowMajor;
      std::string name = std::string(row ? "r_rm" : "r_cm") + std::to_string(t.rows) +
                         std::to_string(t.cols);
      std::string ty = TypeOf(t);
      std::string code;
      if (row) {
        code = ty + " " + name + "(int r) {\n  return " + ty + "(";
        for (int i = 0; i < t.rows; i++)
          code += (i ? ", " : "") + std::string("c[r + ") + std::to_string(i) + "]" + Swz(t.cols);
      } else {
        Type tt = Type::Make(Base::Float, t.cols, t.rows, true);
        code = ty + " " + name + "(int r) {\n  return transpose(" + TypeOf(tt) + "(";
        for (int i = 0; i < t.cols; i++)
          code += (i ? ", " : "") + std::string("c[r + ") + std::to_string(i) + "]" + Swz(t.rows);
        code += ")";
      }
      Helper(name, code + ");\n}\n");
      return name + "(" + r + ")";
    }
    std::string reg = "c[" + r + "]" + Swz(t.cols);
    switch (t.base) {
      case Base::Bool:
        return t.cols > 1 ? "notEqual(" + reg + ", vec" + std::to_string(t.cols) + "(0.0))"
                          : "(" + reg + " != 0.0)";
      case Base::Int:
      case Base::Uint:
        return TypeOf(t) + "(" + reg + ")";
      default:
        return reg;
    }
  }

  std::string UniformValue(VarDecl& v, Loc loc) {
    if (!v.type.array) return UniformElem(v, "", loc);
    if (v.set == RegisterSet::Sampler) Fail(loc, "sampler arrays can only be indexed");
    std::string s = TypeOf(v.type.Elem()) + "[" + std::to_string(v.type.array) + "](";
    for (uint32_t i = 0; i < v.type.array; i++)
      s += (i ? ", " : "") + UniformElem(v, std::to_string(i), loc);
    return s + ")";
  }

  // ---- Expressions

  std::string Convert(const std::string& s, const Type& from, const Type& to, Loc loc) {
    if (from.base == to.base || (from.Floating() && to.Floating())) {
      if (from.matrix == to.matrix && from.rows == to.rows && from.cols == to.cols &&
          from.array == to.array && from.st == to.st)
        return s;
    }
    if (to.base == Base::Struct || to.array) Fail(loc, "cannot convert " + TypeName(from) + " to " + TypeName(to));
    std::string ty = TypeOf(to, loc);
    if (to.Matrix() && from.Scalar()) {
      // HLSL fills every element; GLSL's mat(x) would be a diagonal.
      Type row = Type::Vec(Base::Float, to.cols);
      std::string r = TypeOf(row) + "(" + s + ")";
      std::string m = ty + "(";
      for (int i = 0; i < to.rows; i++) m += (i ? ", " : "") + r;
      return m + ")";
    }
    return ty + "(" + s + ")";
  }

  std::string E(const Expr& e) {
    switch (e.kind) {
      case Expr::Literal:
        switch (e.type.base) {
          case Base::Bool: return e.ival ? "true" : "false";
          case Base::Int: return e.ival < 0 ? "(" + std::to_string(e.ival) + ")" : std::to_string(e.ival);
          case Base::Uint: return std::to_string(uint32_t(e.ival)) + "u";
          default: return FloatLit(e.fval);
        }
      case Expr::Var:
        if (e.var->cls == VarDecl::Class::Uniform) return UniformValue(*e.var, e.loc);
        return e.var->glsl;
      case Expr::Unary:
        return Unary(e);
      case Expr::Binary:
        return Binary(e);
      case Expr::Assign:
        return "(" + Assign(e) + ")";
      case Expr::Ternary:
        if (e.args[0]->type.Scalar())
          return "(" + E(*e.args[0]) + " ? " + E(*e.args[1]) + " : " + E(*e.args[2]) + ")";
        return "mix(" + E(*e.args[2]) + ", " + E(*e.args[1]) + ", " + E(*e.args[0]) + ")";
      case Expr::Call:
        if (e.fn) {
          std::string s = e.fn->glsl + "(";
          for (size_t i = 0; i < e.args.size(); i++) s += (i ? ", " : "") + E(*e.args[i]);
          return s + ")";
        }
        return Intrinsic(e);
      case Expr::Construct: {
        if (e.type.Matrix() && e.args.size() == 1 && e.args[0]->type.Scalar())
          return Convert(E(*e.args[0]), e.args[0]->type, e.type, e.loc);
        std::string s = TypeOf(e.type, e.loc) + "(";
        for (size_t i = 0; i < e.args.size(); i++) s += (i ? ", " : "") + E(*e.args[i]);
        return s + ")";
      }
      case Expr::Cast:
        if (e.type.base == Base::Struct) return Zero(e.type);
        return Convert(E(*e.args[0]), e.args[0]->type, e.type, e.loc);
      case Expr::Convert:
        return Convert(E(*e.args[0]), e.args[0]->type, e.type, e.loc);
      case Expr::Member:
        return Member(e);
      case Expr::Index: {
        const Expr& b = *e.args[0];
        if (b.kind == Expr::Var && b.var->cls == VarDecl::Class::Uniform && b.var->type.array) {
          const Expr& i = *e.args[1];
          std::string idx = i.kind == Expr::Literal ? std::to_string(i.ival) : E(i);
          return UniformElem(*b.var, idx, e.loc);
        }
        return E(b) + "[" + E(*e.args[1]) + "]";
      }
      case Expr::Comma: {
        std::string s = "(";
        for (size_t i = 0; i < e.args.size(); i++) s += (i ? ", " : "") + E(*e.args[i]);
        return s + ")";
      }
      case Expr::InitList:
        return Init(e, e.type);
    }
    Fail(e.loc, "unexpected expression");
  }

  std::string Unary(const Expr& e) {
    std::string a = E(*e.args[0]);
    switch (e.op) {
      case Op::Not:
        return e.type.Scalar() ? "(!" + a + ")" : "not(" + a + ")";
      case Op::PostInc:
      case Op::PostDec:
        return "(" + a + OpText(e.op) + ")";
      default:
        return "(" + std::string(OpText(e.op)) + a + ")";
    }
  }

  std::string Binary(const Expr& e) {
    std::string a = E(*e.args[0]), b = E(*e.args[1]);
    const Type& t = e.args[0]->type;
    bool vec = !t.Scalar() && !t.Matrix();
    switch (e.op) {
      case Op::Mul:
        if (t.Matrix()) return "matrixCompMult(" + a + ", " + b + ")";
        break;
      case Op::Mod:
        if (t.Floating()) {
          GenHelper("r_fmod", "$T r_fmod($T x, $T y) { return x - y * trunc(x / y); }\n");
          return "r_fmod(" + a + ", " + b + ")";
        }
        break;
      case Op::Lt: case Op::Gt: case Op::Le: case Op::Ge: case Op::Eq: case Op::Ne:
        if (vec) {
          const char* f = e.op == Op::Lt   ? "lessThan"
                          : e.op == Op::Gt ? "greaterThan"
                          : e.op == Op::Le ? "lessThanEqual"
                          : e.op == Op::Ge ? "greaterThanEqual"
                          : e.op == Op::Eq ? "equal"
                                           : "notEqual";
          return std::string(f) + "(" + a + ", " + b + ")";
        }
        break;
      case Op::And:
      case Op::Or:
        if (vec) {
          // Componentwise and evaluated in full, as HLSL does for vectors.
          std::string n = std::to_string(t.cols);
          return "bvec" + n + "(uvec" + n + "(" + a + ")" + (e.op == Op::And ? " & " : " | ") +
                 "uvec" + n + "(" + b + "))";
        }
        break;
      default:
        break;
    }
    return "(" + a + " " + OpText(e.op) + " " + b + ")";
  }

  std::string Assign(const Expr& e) {
    const Expr& l = *e.args[0];
    std::string a = E(l), b = E(*e.args[1]);
    if (e.op == Op::None) return a + " = " + b;
    const Type& lt = l.type;
    bool same_base = SameBase(e.spec, lt);
    bool mat_mul = e.op == Op::Mul && lt.Matrix();
    bool fmod = e.op == Op::Mod && e.spec.Floating();
    if (same_base && !mat_mul && !fmod) return a + " " + OpText(e.op) + "= " + b;
    if (SideEffects(l)) Fail(e.loc, "compound assignment to an expression with side effects");
    std::string v;
    if (mat_mul) {
      v = "matrixCompMult(" + a + ", " + b + ")";
    } else if (fmod) {
      GenHelper("r_fmod", "$T r_fmod($T x, $T y) { return x - y * trunc(x / y); }\n");
      v = "r_fmod(" + Convert(a, lt, e.spec, e.loc) + ", " + b + ")";
    } else {
      v = "(" + Convert(a, lt, e.spec, e.loc) + " " + OpText(e.op) + " " + b + ")";
    }
    return a + " = " + Convert(v, e.spec, lt, e.loc);
  }

  static bool SameBase(const Type& a, const Type& b) {
    return a.base == b.base || (a.Floating() && b.Floating());
  }

  std::string Member(const Expr& e) {
    const Expr& b = *e.args[0];
    if (e.field >= 0) return E(b) + "." + b.type.st->fields[e.field]->glsl;
    std::string s = E(b);
    if (b.type.Matrix()) {
      auto elem = [&](uint8_t k) {
        return s + "[" + std::to_string(k / 4) + "][" + std::to_string(k % 4) + "]";
      };
      if (e.swz.size() == 1) return elem(e.swz[0]);
      if (SideEffects(b)) Fail(e.loc, "matrix swizzle of an expression with side effects");
      std::string r = TypeOf(e.type) + "(";
      for (size_t i = 0; i < e.swz.size(); i++) r += (i ? ", " : "") + elem(e.swz[i]);
      return r + ")";
    }
    if (b.type.Scalar()) {
      if (e.swz.size() == 1) return s;
      return TypeOf(e.type) + "(" + s + ")";
    }
    std::string sw;
    for (uint8_t k : e.swz) sw += kComps[k];
    return s + "." + sw;
  }

  // ---- Initialisers

  // Scalar components of a brace list, in order.
  void Components(const Expr& e, std::vector<std::string>* out) {
    if (e.kind == Expr::InitList) {
      for (const auto& a : e.args) Components(*a, out);
      return;
    }
    ComponentsOf(E(e), e.type, out, e.loc);
  }

  void ComponentsOf(const std::string& s, const Type& t, std::vector<std::string>* out, Loc loc) {
    if (t.array) {
      for (uint32_t i = 0; i < t.array; i++)
        ComponentsOf(s + "[" + std::to_string(i) + "]", t.Elem(), out, loc);
    } else if (t.base == Base::Struct) {
      for (const auto& f : t.st->fields) ComponentsOf(s + "." + f->glsl, f->type, out, loc);
    } else if (t.Matrix()) {
      for (int r = 0; r < t.rows; r++)
        for (int c = 0; c < t.cols; c++)
          out->push_back(s + "[" + std::to_string(r) + "][" + std::to_string(c) + "]");
    } else if (t.Vector()) {
      for (int c = 0; c < t.cols; c++) out->push_back(s + "." + kComps[c]);
    } else {
      out->push_back(s);
    }
  }

  std::string Build(const Type& t, const std::vector<std::string>& c, size_t* pos) {
    if (t.array) {
      std::string s = TypeOf(t.Elem()) + "[" + std::to_string(t.array) + "](";
      for (uint32_t i = 0; i < t.array; i++) s += (i ? ", " : "") + Build(t.Elem(), c, pos);
      return s + ")";
    }
    if (t.base == Base::Struct) {
      std::string s = t.st->glsl + "(";
      for (size_t i = 0; i < t.st->fields.size(); i++)
        s += (i ? ", " : "") + Build(t.st->fields[i]->type, c, pos);
      return s + ")";
    }
    std::string s = TypeOf(t) + "(";
    for (int i = 0; i < t.Components(); i++) s += (i ? ", " : "") + c[(*pos)++];
    return s + ")";
  }

  std::string Init(const Expr& init, const Type& t) {
    if (init.kind != Expr::InitList) return E(init);
    std::vector<std::string> comps;
    Components(init, &comps);
    size_t pos = 0;
    return Build(t, comps, &pos);
  }

  // ---- Intrinsics

  std::string Intrinsic(const Expr& e) {
    std::vector<std::string> a;
    for (const auto& x : e.args) a.push_back(E(*x));
    auto call = [&](const char* f) {
      std::string s = std::string(f) + "(";
      for (size_t i = 0; i < a.size(); i++) s += (i ? ", " : "") + a[i];
      return s + ")";
    };
    const Type& t0 = e.args[0]->type;
    switch (e.intrinsic) {
      case kAbs: return call("abs");
      case kAcos: return call("acos");
      case kAsin: return call("asin");
      case kAtan: return call("atan");
      case kAtan2: return call("atan");
      case kCeil: return call("ceil");
      case kClamp: return call("clamp");
      case kCos: return call("cos");
      case kCosh: return call("cosh");
      case kCross: return call("cross");
      case kDdx: return call("dFdx");
      case kDdy: return call("dFdy");
      case kDegrees: return call("degrees");
      case kDeterminant: return call("determinant");
      case kDistance: return call("distance");
      case kDot: return call("dot");
      case kExp: return call("exp");
      case kExp2: return call("exp2");
      case kFaceforward: return call("faceforward");
      case kFloor: return call("floor");
      case kFrac: return call("fract");
      case kFwidth: return call("fwidth");
      case kIsinf: return call("isinf");
      case kIsnan: return call("isnan");
      case kLength: return call("length");
      case kLerp: return call("mix");
      case kMax: return call("max");
      case kMin: return call("min");
      case kModf: return call("modf");
      case kNormalize: return call("normalize");
      case kRadians: return call("radians");
      case kReflect: return call("reflect");
      case kRefract: return call("refract");
      case kRound: return call("roundEven");
      case kSign: return call("sign");
      case kSin: return call("sin");
      case kSinh: return call("sinh");
      case kSmoothstep: return call("smoothstep");
      case kStep: return call("step");
      case kTan: return call("tan");
      case kTanh: return call("tanh");
      case kTranspose: return call("transpose");
      case kTrunc: return call("trunc");
      case kLog: return "log(abs(" + a[0] + "))";
      case kLog2: return "log2(abs(" + a[0] + "))";
      case kLog10: return "(log2(abs(" + a[0] + ")) * 0.301029996)";
      case kSqrt: return "sqrt(abs(" + a[0] + "))";
      case kRsqrt: return "inversesqrt(abs(" + a[0] + "))";
      case kPow: return "pow(abs(" + a[0] + "), " + a[1] + ")";
      case kSaturate: return "clamp(" + a[0] + ", 0.0, 1.0)";
      case kLdexp: return "(" + a[0] + " * exp2(" + a[1] + "))";
      case kFmod:
        GenHelper("r_fmod", "$T r_fmod($T x, $T y) { return x - y * trunc(x / y); }\n");
        return call("r_fmod");
      case kAll:
      case kAny: {
        bool all = e.intrinsic == kAll;
        if (t0.Scalar()) return t0.base == Base::Bool ? a[0] : "(" + a[0] + " != " + Zero(t0) + ")";
        std::string b = t0.base == Base::Bool ? a[0] : "notEqual(" + a[0] + ", " + Zero(t0) + ")";
        return std::string(all ? "all(" : "any(") + b + ")";
      }
      case kIsfinite:
        if (t0.Scalar()) return "!(isinf(" + a[0] + ") || isnan(" + a[0] + "))";
        return "not(bvec" + std::to_string(t0.cols) + "(uvec" + std::to_string(t0.cols) +
               "(isinf(" + a[0] + ")) | uvec" + std::to_string(t0.cols) + "(isnan(" + a[0] + "))))";
      case kClip:
        Helper("r_clip",
               "void r_clip(float x) { if (x < 0.0) discard; }\n"
               "void r_clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n"
               "void r_clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n"
               "void r_clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n");
        return call("r_clip");
      case kSincos:
        GenHelper("r_sincos", "void r_sincos($T x, out $T s, out $T c) { s = sin(x); c = cos(x); }\n");
        return call("r_sincos");
      case kFrexp:
        Helper("r_frexp",
               "float r_frexp(float x, out float e) { int i; float m = frexp(x, i); e = float(i); return m; }\n"
               "vec2 r_frexp(vec2 x, out vec2 e) { ivec2 i; vec2 m = frexp(x, i); e = vec2(i); return m; }\n"
               "vec3 r_frexp(vec3 x, out vec3 e) { ivec3 i; vec3 m = frexp(x, i); e = vec3(i); return m; }\n"
               "vec4 r_frexp(vec4 x, out vec4 e) { ivec4 i; vec4 m = frexp(x, i); e = vec4(i); return m; }\n");
        return call("r_frexp");
      case kLit:
        Helper("r_lit",
               "vec4 r_lit(float l, float h, float m) {\n"
               "  return vec4(1.0, max(l, 0.0), (l < 0.0 || h < 0.0) ? 0.0 : pow(abs(h), m), 1.0);\n"
               "}\n");
        return call("r_lit");
      case kD3DColorToUbyte4:
        return "ivec4((" + a[0] + ").zyxw * 255.001953)";
      case kMul:
        return Mul(e, a);
      default:
        return Texture(e, a);
    }
  }

  std::string Mul(const Expr& e, const std::vector<std::string>& a) {
    const Type& x = e.args[0]->type;
    const Type& y = e.args[1]->type;
    if (x.Scalar() || y.Scalar()) return "(" + a[0] + " * " + a[1] + ")";
    if (x.Vector() && y.Vector()) return "dot(" + a[0] + ", " + a[1] + ")";
    // See the top of the file: GLSL's product order is reversed.
    return "(" + a[1] + " * " + a[0] + ")";
  }

  std::string SamplerName(const Expr& s) {
    if (s.kind == Expr::Var) {
      if (s.var->cls == VarDecl::Class::Uniform) return UniformElem(*s.var, "", s.loc);
      return s.var->glsl;
    }
    if (s.kind == Expr::Index && s.args[0]->kind == Expr::Var &&
        s.args[0]->var->cls == VarDecl::Class::Uniform && s.args[1]->kind == Expr::Literal)
      return UniformElem(*s.args[0]->var, std::to_string(s.args[1]->ival), s.loc);
    Fail(s.loc, "unsupported sampler expression");
  }

  std::string Texture(const Expr& e, const std::vector<std::string>& a) {
    int k = e.intrinsic - kTex1D;
    Dim dim = Dim(1 + k / 5);
    int variant = k % 5;  // plain, bias, grad, lod, proj
    std::string s = SamplerName(*e.args[0]);
    const char* sampler = dim == Dim::D3 ? "sampler3D" : dim == Dim::Cube ? "samplerCube" : "sampler2D";
    // The coordinate swizzle of a float4 argument, and the coordinates
    // built from a 1D one (a 1D texture is a 2D one of height 1).
    std::string cs = dim == Dim::D1 ? ".x" : dim == Dim::D2 ? ".xy" : ".xyz";
    auto coords = [&](const std::string& v) {
      return dim == Dim::D1 ? "vec2(" + v + ", 0.5)" : v;
    };
    bool grads = e.args.size() == 4;
    // Pixel shaders add the sampler's LOD bias where the level is implicit
    // (R_BIAS, hlsl.h); a sampler passed as a parameter has no register.
    const bool pixel = tg_.stage == Stage::Pixel;
    const std::string bias =
        s.rfind("r_s", 0) == 0 ? "R_BIAS(" + s.substr(3) + ")" : std::string("0.0");
    if (variant == 0 || variant == 2) {
      if (grads) {
        std::string dx = dim == Dim::D1 ? "vec2(" + a[2] + ", 0.0)" : a[2];
        std::string dy = dim == Dim::D1 ? "vec2(" + a[3] + ", 0.0)" : a[3];
        return "textureGrad(" + s + ", " + coords(a[1]) + ", " + dx + ", " + dy + ")";
      }
      return "texture(" + s + ", " + coords(a[1]) + (pixel ? ", " + bias : "") + ")";
    }
    // bias, lod and proj take a float4: a helper so it is evaluated once.
    static const char* const kVariant[] = {"", "bias", "", "lod", "proj"};
    std::string dname = dim == Dim::D1 ? "1D" : dim == Dim::D2 ? "2D" : dim == Dim::D3 ? "3D" : "CUBE";
    std::string name = "r_tex" + dname + kVariant[variant];
    std::string body;
    std::string c = dim == Dim::D1 ? "vec2(t.x, 0.5)" : "t" + cs;
    // The bias and proj helpers of a pixel shader take the sampler's bias.
    const bool b = pixel && variant != 3;
    const std::string pb = b ? ", b" : "";
    switch (variant) {
      case 1: body = "texture(s, " + c + ", t.w" + (b ? " + b" : "") + ")"; break;
      case 3: body = "textureLod(s, " + c + ", t.w)"; break;
      default:
        if (dim == Dim::D2 || dim == Dim::D3) body = "textureProj(s, t" + pb + ")";
        else if (dim == Dim::D1) body = "texture(s, vec2(t.x / t.w, 0.5)" + pb + ")";
        else body = "texture(s, t.xyz / t.w" + pb + ")";
        break;
    }
    Helper(name, "vec4 " + name + "(" + sampler + " s, vec4 t" + (b ? ", float b" : "") +
                     ") { return " + body + "; }\n");
    return name + "(" + s + ", " + a[1] + (b ? ", " + bias : "") + ")";
  }

  // ---- Statements

  std::string Ind() const { return std::string(indent_ * 2, ' '); }

  std::string VarDecls(const Stmt& s) {
    std::string r;
    for (size_t i = 0; i < s.vars.size(); i++) {
      const VarDecl& v = *s.vars[i];
      const_cast<VarDecl&>(v).glsl = Mangle("h_", v.name);
      if (i == 0) {
        r += TypeOf(v.type.Elem(), v.loc) + " ";
      } else if (!(s.vars[0]->type.Elem() == v.type.Elem())) {
        Fail(v.loc, "declarations of different types in one statement");
      } else {
        r += ", ";
      }
      r += v.glsl + (v.type.array ? "[" + std::to_string(v.type.array) + "]" : "");
      if (v.init) r += " = " + Init(*v.init, v.type);
    }
    return r;
  }

  void Stm(const Stmt& s, std::string* o) {
    switch (s.kind) {
      case Stmt::Block:
        *o += Ind() + "{\n";
        indent_++;
        for (const auto& b : s.body) Stm(*b, o);
        indent_--;
        *o += Ind() + "}\n";
        break;
      case Stmt::Decl:
        for (const auto& v : s.vars) {
          v->glsl = Mangle("h_", v->name);
          *o += Ind() + Decl(v->type, v->glsl);
          if (v->init) *o += " = " + Init(*v->init, v->type);
          *o += ";\n";
        }
        break;
      case Stmt::Expression:
        *o += Ind() + (s.expr->kind == Expr::Assign ? Assign(*s.expr) : E(*s.expr)) + ";\n";
        break;
      case Stmt::If:
        *o += Ind() + "if (" + E(*s.cond) + ")\n";
        Body(*s.then, o);
        if (s.els) {
          *o += Ind() + "else\n";
          Body(*s.els, o);
        }
        break;
      case Stmt::For: {
        std::string init;
        if (s.init) {
          if (s.init->kind == Stmt::Decl) init = VarDecls(*s.init);
          else if (s.init->kind == Stmt::Expression) init = E(*s.init->expr);
        }
        *o += Ind() + "for (" + init + "; " + (s.cond ? E(*s.cond) : "") + "; " +
              (s.step ? E(*s.step) : "") + ")\n";
        Body(*s.then, o);
        break;
      }
      case Stmt::While:
        *o += Ind() + "while (" + E(*s.cond) + ")\n";
        Body(*s.then, o);
        break;
      case Stmt::Do:
        *o += Ind() + "do\n";
        Body(*s.then, o);
        *o += Ind() + "while (" + E(*s.cond) + ");\n";
        break;
      case Stmt::Return:
        *o += Ind() + "return" + (s.expr ? " " + E(*s.expr) : "") + ";\n";
        break;
      case Stmt::Break: *o += Ind() + "break;\n"; break;
      case Stmt::Continue: *o += Ind() + "continue;\n"; break;
      case Stmt::Discard: *o += Ind() + "discard;\n"; break;
      case Stmt::Empty: break;
    }
  }

  // A statement as the body of if/for/while: always braced.
  void Body(const Stmt& s, std::string* o) {
    if (s.kind == Stmt::Block) {
      Stm(s, o);
      return;
    }
    *o += Ind() + "{\n";
    indent_++;
    Stm(s, o);
    indent_--;
    *o += Ind() + "}\n";
  }

  std::string Signature(const FuncDecl& f) {
    std::string s = TypeOf(f.ret, f.loc) + " " + f.glsl + "(";
    for (size_t i = 0; i < f.params.size(); i++) {
      const VarDecl& p = *f.params[i];
      if (i) s += ", ";
      if ((p.flags & kOut) && (p.flags & kIn)) s += "inout ";
      else if (p.flags & kOut) s += "out ";
      else if (p.flags & kConst) s += "const in ";
      Type t = p.type;
      if (t.base == Base::Sampler && t.dim == Dim::Any) t.dim = p.sampler_dim;
      s += Decl(t, p.glsl);
    }
    return s + ")";
  }

  std::string Function(const FuncDecl& f) {
    std::string o = Signature(f) + "\n";
    Stm(*f.body, &o);
    return o + "\n";
  }

  // ---- Globals

  std::string GlobalInit() {
    std::string o;
    for (auto& g : u_.globals) {
      if (!g->used || g->cls != VarDecl::Class::Private) continue;
      o += "  " + g->glsl + " = " + (g->init ? Init(*g->init, g->type) : Zero(g->type)) + ";\n";
    }
    if (o.empty()) return "";
    has_init_ = true;
    return "void r_init() {\n" + o + "}\n\n";
  }

  // ---- The entry point

  // `flags`: interpolation modifiers of a pixel shader input.
  std::string Varying(Semantic s, bool out, Loc loc, uint32_t flags = 0) {
    int l = VaryingLocation(s);
    if (l < 0) Fail(loc, "semantic cannot be interpolated");
    std::string name = "r_v" + std::to_string(l);
    if (!declared_.insert(name).second) {
      if (out) Fail(loc, "two outputs with one semantic");
      return name;
    }
    std::string q = flags & kNoInterpolation ? "flat " : flags & kCentroid ? "centroid " : "";
    io_ += "layout(location = " + std::to_string(l) + ") " + q + (out ? "out" : "in") + " vec4 " +
           name + ";\n";
    out_->varyings.push_back(s);
    return name;
  }

  // A vec4 for an output register from a value of type t: missing
  // components are 0, and w 1.
  std::string Pad(const std::string& v, const Type& t, Loc loc) {
    if (!t.Numeric() || t.Matrix() || t.array) Fail(loc, "bad output type " + TypeName(t));
    Type ft = t.cols == 1 ? Type::Make(Base::Float) : Type::Vec(Base::Float, t.cols);
    std::string f = Convert(v, t, ft, loc);
    switch (t.cols) {
      case 1: return "vec4(" + f + ", 0.0, 0.0, 1.0)";
      case 2: return "vec4(" + f + ", 0.0, 1.0)";
      case 3: return "vec4(" + f + ", 1.0)";
      default: return f;
    }
  }

  // Code that reads input `sem` into `dst` of type t.
  std::string Input(const std::string& dst, const Type& t, const std::string& semantic,
                    uint32_t flags, Loc loc) {
    if (t.base == Base::Struct) {
      if (t.array) Fail(loc, "arrays of structs as inputs are not supported");
      std::string o;
      for (const auto& f : t.st->fields)
        o += Input(dst + "." + f->glsl, f->type, f->semantic, f->flags, f->loc);
      return o;
    }
    if (semantic.empty()) Fail(loc, "input without a semantic");
    Semantic s;
    if (!ParseSemantic(semantic, &s)) Fail(loc, "unknown semantic " + semantic);
    if (t.array) {
      std::string o;
      for (uint32_t i = 0; i < t.array; i++) {
        Semantic si{s.usage, s.index + i};
        o += "  " + dst + "[" + std::to_string(i) + "] = " + InputValue(si, t.Elem(), flags, loc) + ";\n";
      }
      return o;
    }
    return "  " + dst + " = " + InputValue(s, t, flags, loc) + ";\n";
  }

  std::string InputValue(Semantic s, const Type& t, uint32_t flags, Loc loc) {
    if (!t.Numeric() || t.Matrix()) Fail(loc, "bad input type " + TypeName(t));
    std::string v;
    const Type v4 = Type::Vec(Base::Float, 4);
    if (tg_.stage == Stage::Vertex) {
      size_t slot = 0;
      while (slot < out_->inputs.size() && !(out_->inputs[slot] == s)) slot++;
      if (slot == out_->inputs.size()) out_->inputs.push_back(s);
      v = "r_fetch(" + std::to_string(slot) + ")";
    } else if (s.usage == kVPos) {
      v = "vec4(r_vpos(), 0.0, 0.0)";
    } else if (s.usage == kVFace) {
      v = "vec4(r_vface())";
    } else if (s.usage == kPosition) {
      // Pixel shaders cannot read POSITION; the compiler accepts it when
      // the value is never used, which is the only way it can be here.
      v = "vec4(0.0)";
    } else {
      v = Varying(s, false, loc, flags);
    }
    return Convert(v, v4, t, loc);
  }

  // Code that writes the outputs in `src` of type t.
  std::string Output(const std::string& src, const Type& t, const std::string& semantic, Loc loc) {
    if (t.base == Base::Struct) {
      if (t.array) Fail(loc, "arrays of structs as outputs are not supported");
      std::string o;
      for (const auto& f : t.st->fields) o += Output(src + "." + f->glsl, f->type, f->semantic, f->loc);
      return o;
    }
    if (semantic.empty()) Fail(loc, "output without a semantic");
    Semantic s;
    if (!ParseSemantic(semantic, &s)) Fail(loc, "unknown semantic " + semantic);
    if (t.array) {
      std::string o;
      for (uint32_t i = 0; i < t.array; i++)
        o += OutputValue(src + "[" + std::to_string(i) + "]", Semantic{s.usage, s.index + i}, t.Elem(), loc);
      return o;
    }
    return OutputValue(src, s, t, loc);
  }

  std::string OutputValue(const std::string& v, Semantic s, const Type& t, Loc loc) {
    if (tg_.stage == Stage::Vertex) {
      if (s.usage == kPosition && s.index == 0) return "  gl_Position = r_position(" + Pad(v, t, loc) + ");\n";
      if (s.usage == kPSize) {
        out_->writes_point_size = true;
        return "  gl_PointSize = float(" + v + ");\n";
      }
      std::string p = Pad(v, t, loc);
      // Before shader model 3 the colour outputs are clamped to [0, 1].
      if (s.usage == kColor && tg_.major < 3) p = "clamp(" + p + ", 0.0, 1.0)";
      return "  " + Varying(s, true, loc) + " = " + p + ";\n";
    }
    if (s.usage == kDepth && s.index == 0) {
      out_->writes_depth = true;
      return "  gl_FragDepth = float(" + v + ");\n";
    }
    if (s.usage != kColor || s.index > 3) Fail(loc, "pixel shader outputs are COLOR0-3 and DEPTH");
    if (out_->color_outputs & (1u << s.index)) Fail(loc, "two outputs to one render target");
    out_->color_outputs |= 1u << s.index;
    std::string name = "r_c" + std::to_string(s.index);
    io_ += "layout(location = " + std::to_string(s.index) + ") out vec4 " + name + ";\n";
    return "  " + name + " = " + Pad(v, t, loc) + ";\n";
  }

  std::string Main() {
    std::string o = "void main() {\n";
    if (has_init_) o += "  r_init();\n";
    std::string call = entry_->glsl + "(";
    std::string outs;
    for (size_t i = 0; i < entry_->params.size(); i++) {
      const VarDecl& p = *entry_->params[i];
      if (p.flags & kUniform) Fail(p.loc, "uniform parameters of the entry point are not supported");
      std::string n = "r_p" + std::to_string(i);
      o += "  " + Decl(p.type, n) + ";\n";
      if (!(p.flags & kOut) || (p.flags & kIn)) o += Input(n, p.type, p.semantic, p.flags, p.loc);
      if (p.flags & kOut) outs += Output(n, p.type, p.semantic, p.loc);
      call += (i ? ", " : "") + n;
    }
    call += ")";
    if (entry_->ret.base == Base::Void) {
      o += "  " + call + ";\n";
    } else {
      o += "  " + Decl(entry_->ret, "r_ret") + " = " + call + ";\n";
      outs = Output("r_ret", entry_->ret, entry_->ret_semantic, entry_->loc) + outs;
    }
    // The backend's per-pixel work on the colour (the alpha test).
    if (out_->color_outputs & 1) outs += "  r_color0(r_c0);\n";
    return o + outs + "}\n";
  }

  Unit& u_;
  FuncDecl* entry_;
  Target tg_;
  Program* out_;
  int indent_ = 0;
  std::map<std::string, std::string> helpers_;
  std::vector<std::string> helper_order_;
  uint32_t max_c_ = 0;
  bool uses_int_ = false;
  bool has_init_ = false;
  std::map<uint32_t, Dim> samplers_;
  std::string io_;
  std::set<std::string> declared_;
};

}  // namespace

void EmitGlsl(Unit& unit, FuncDecl* entry, const Target& target, Program* out) {
  Emitter(unit, entry, target, out).Run();
}

}  // namespace recomp::hlsl

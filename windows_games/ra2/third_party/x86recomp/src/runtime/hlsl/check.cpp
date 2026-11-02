// Semantic analysis: resolves names, types every expression and makes
// HLSL's implicit conversions explicit (Convert nodes: scalar splat,
// vector/matrix truncation, base type changes), so the GLSL emitter never
// relies on GLSL's narrower implicit conversions. Only what the entry
// point reaches is checked, as only that is compiled; along the way it
// records which globals are read and written.

#include <algorithm>
#include <cstring>

#include "runtime/hlsl/ast.h"

namespace recomp::hlsl {
namespace {

struct IntrinsicName {
  const char* name;
  int id;
};
const IntrinsicName kIntrinsics[] = {
    {"abs", kAbs}, {"acos", kAcos}, {"all", kAll}, {"any", kAny}, {"asin", kAsin},
    {"atan", kAtan}, {"atan2", kAtan2}, {"ceil", kCeil}, {"clamp", kClamp}, {"clip", kClip},
    {"cos", kCos}, {"cosh", kCosh}, {"cross", kCross}, {"D3DCOLORtoUBYTE4", kD3DColorToUbyte4},
    {"ddx", kDdx}, {"ddy", kDdy}, {"degrees", kDegrees}, {"determinant", kDeterminant},
    {"distance", kDistance}, {"dot", kDot}, {"exp", kExp}, {"exp2", kExp2},
    {"faceforward", kFaceforward}, {"floor", kFloor}, {"fmod", kFmod}, {"frac", kFrac},
    {"frexp", kFrexp}, {"fwidth", kFwidth}, {"isfinite", kIsfinite}, {"isinf", kIsinf},
    {"isnan", kIsnan}, {"ldexp", kLdexp}, {"length", kLength}, {"lerp", kLerp}, {"lit", kLit},
    {"log", kLog}, {"log10", kLog10}, {"log2", kLog2}, {"max", kMax}, {"min", kMin},
    {"modf", kModf}, {"mul", kMul}, {"normalize", kNormalize}, {"pow", kPow},
    {"radians", kRadians}, {"reflect", kReflect}, {"refract", kRefract}, {"round", kRound},
    {"rsqrt", kRsqrt}, {"saturate", kSaturate}, {"sign", kSign}, {"sin", kSin},
    {"sincos", kSincos}, {"sinh", kSinh}, {"smoothstep", kSmoothstep}, {"sqrt", kSqrt},
    {"step", kStep}, {"tan", kTan}, {"tanh", kTanh}, {"transpose", kTranspose},
    {"trunc", kTrunc},
    {"tex1D", kTex1D}, {"tex1Dbias", kTex1Dbias}, {"tex1Dgrad", kTex1Dgrad},
    {"tex1Dlod", kTex1Dlod}, {"tex1Dproj", kTex1Dproj},
    {"tex2D", kTex2D}, {"tex2Dbias", kTex2Dbias}, {"tex2Dgrad", kTex2Dgrad},
    {"tex2Dlod", kTex2Dlod}, {"tex2Dproj", kTex2Dproj},
    {"tex3D", kTex3D}, {"tex3Dbias", kTex3Dbias}, {"tex3Dgrad", kTex3Dgrad},
    {"tex3Dlod", kTex3Dlod}, {"tex3Dproj", kTex3Dproj},
    {"texCUBE", kTexCube}, {"texCUBEbias", kTexCubebias}, {"texCUBEgrad", kTexCubegrad},
    {"texCUBElod", kTexCubelod}, {"texCUBEproj", kTexCubeproj},
};

int FindIntrinsic(const std::string& n) {
  for (const auto& i : kIntrinsics)
    if (n == i.name) return i.id;
  return -1;
}

bool SameGlsl(Base a, Base b) {
  auto f = [](Base x) { return x == Base::Half || x == Base::Double ? Base::Float : x; };
  return f(a) == f(b);
}

// Types equal once half and double are float, as in the GLSL.
bool SameTranslated(const Type& a, const Type& b) {
  return SameGlsl(a.base, b.base) && a.matrix == b.matrix && a.rows == b.rows &&
         a.cols == b.cols && a.array == b.array && a.dim == b.dim && a.st == b.st;
}

Type Floating(Type t) {
  if (!t.Floating()) t.base = Base::Float;
  return t;
}

class Checker {
 public:
  Checker(Unit& u, const Target& t) : u_(u), tg_(t) {}

  FuncDecl* Run(const std::string& entry) {
    for (auto& g : u_.globals) {
      if (globals_.count(g->name)) Fail(g->loc, "redefinition of " + g->name);
      globals_[g->name] = g.get();
    }
    for (auto& f : u_.funcs) funcs_[f->name].push_back(f.get());
    for (auto& [name, fs] : funcs_) LinkDefinitions(fs);
    auto it = funcs_.find(entry);
    if (it == funcs_.end()) Fail(Loc{}, "entrypoint not found: " + entry);
    FuncDecl* e = nullptr;
    for (FuncDecl* f : it->second) {
      FuncDecl* d = f->def ? f->def : f;
      if (e && e != d) Fail(f->loc, "entrypoint " + entry + " is overloaded");
      e = d;
    }
    if (!e->body) Fail(e->loc, "entrypoint " + entry + " has no body");
    CheckFunction(e);
    Classify();
    return e;
  }

 private:
  // ---- Functions

  static bool SameSignature(const FuncDecl& a, const FuncDecl& b) {
    if (a.params.size() != b.params.size()) return false;
    for (size_t i = 0; i < a.params.size(); i++)
      if (!(a.params[i]->type == b.params[i]->type)) return false;
    return true;
  }

  void LinkDefinitions(std::vector<FuncDecl*>& fs) {
    for (FuncDecl* f : fs) {
      for (FuncDecl* g : fs) {
        if (!g->body || !SameSignature(*f, *g)) continue;
        if (f->body && f != g) Fail(g->loc, "redefinition of function " + g->name);
        f->def = g;
      }
    }
  }

  void CheckFunction(FuncDecl* f) {
    if (f->state == FuncDecl::State::Checked) return;
    if (f->state == FuncDecl::State::Checking) Fail(f->loc, "recursive call to " + f->name);
    f->state = FuncDecl::State::Checking;
    FuncDecl* saved_fn = cur_;
    auto saved_scopes = std::move(scopes_);
    scopes_.clear();
    cur_ = f;
    Push();
    for (auto& p : f->params) {
      p->cls = VarDecl::Class::Param;
      if (p->type.base == Base::Struct) Use(p->type);
      Declare(p.get());
    }
    if (f->ret.base == Base::Struct) Use(f->ret);
    CheckStmt(*f->body);
    Pop();
    cur_ = saved_fn;
    scopes_ = std::move(saved_scopes);
    f->state = FuncDecl::State::Checked;
  }

  // ---- Scopes

  void Push() { scopes_.emplace_back(); }
  void Pop() { scopes_.pop_back(); }
  void Declare(VarDecl* v) {
    auto& s = scopes_.back();
    if (s.count(v->name)) Fail(v->loc, "redefinition of " + v->name);
    s[v->name] = v;
  }
  VarDecl* Lookup(const std::string& n) {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
      auto it = s->find(n);
      if (it != s->end()) return it->second;
    }
    auto it = globals_.find(n);
    return it == globals_.end() ? nullptr : it->second;
  }

  void Use(const Type& t) {
    if (t.base != Base::Struct || !t.st) return;
    auto* sd = const_cast<StructDecl*>(t.st);
    if (sd->used) return;
    sd->used = true;
    for (auto& f : sd->fields) Use(f->type);
  }

  void UseGlobal(VarDecl* v) {
    v->used = true;
    Use(v->type);
    if (v->checked) return;
    v->checked = true;
    if (!v->init) return;
    // The initialiser is checked in global scope.
    FuncDecl* saved_fn = cur_;
    auto saved_scopes = std::move(scopes_);
    scopes_.clear();
    cur_ = nullptr;
    CheckInit(v->init, v->type);
    cur_ = saved_fn;
    scopes_ = std::move(saved_scopes);
  }

  // ---- Statements

  void CheckStmt(Stmt& s) {
    switch (s.kind) {
      case Stmt::Block:
        Push();
        for (auto& b : s.body) CheckStmt(*b);
        Pop();
        break;
      case Stmt::Decl:
        for (auto& v : s.vars) {
          if (v->flags & (kUniform | kExtern | kShared))
            Fail(v->loc, "local variables cannot be uniform");
          if (v->type.base == Base::Void) Fail(v->loc, "void variable " + v->name);
          Use(v->type);
          if (v->init) CheckInit(v->init, v->type);
          v->cls = VarDecl::Class::Local;
          Declare(v.get());
        }
        break;
      case Stmt::Expression:
        CheckExpr(s.expr);
        break;
      case Stmt::If:
        Condition(s.cond);
        Push();
        CheckStmt(*s.then);
        Pop();
        if (s.els) {
          Push();
          CheckStmt(*s.els);
          Pop();
        }
        break;
      case Stmt::For:
        Push();
        if (s.init) CheckStmt(*s.init);
        if (s.cond) Condition(s.cond);
        if (s.step) CheckExpr(s.step);
        Push();
        CheckStmt(*s.then);
        Pop();
        Pop();
        break;
      case Stmt::While:
      case Stmt::Do:
        Condition(s.cond);
        Push();
        CheckStmt(*s.then);
        Pop();
        break;
      case Stmt::Return:
        if (!cur_) Fail(s.loc, "return outside a function");
        if (cur_->ret.base == Base::Void) {
          if (s.expr) Fail(s.loc, "void function returns a value");
        } else {
          if (!s.expr) Fail(s.loc, "function must return a value");
          CheckExpr(s.expr);
          Coerce(s.expr, cur_->ret, "return value");
        }
        break;
      case Stmt::Discard:
        if (tg_.stage != Stage::Pixel) Fail(s.loc, "discard in a vertex shader");
        break;
      case Stmt::Break:
      case Stmt::Continue:
      case Stmt::Empty:
        break;
    }
  }

  void Condition(ExprP& e) {
    CheckExpr(e);
    if (!e->type.Scalar()) Fail(e->loc, "condition must be a scalar, not " + TypeName(e->type));
    Coerce(e, Type::Make(Base::Bool), "condition");
  }

  // ---- Initialisers

  // Number of scalar components in a type (structs, arrays included).
  static int Flat(const Type& t) {
    int n;
    if (t.base == Base::Struct) {
      n = 0;
      for (const auto& f : t.st->fields) n += Flat(f->type);
    } else {
      n = t.Components();
    }
    return t.array ? n * int(t.array) : n;
  }

  void CheckInit(ExprP& init, const Type& t) {
    if (init->kind != Expr::InitList) {
      CheckExpr(init);
      // `float4 v = { x }` style lists are InitLists; a plain expression
      // converts like an assignment.
      Coerce(init, t, "initializer");
      return;
    }
    // Brace lists are flattened to scalars and refilled in order, as HLSL
    // does; check every leaf, then the count.
    int n = 0;
    CheckList(*init, &n);
    if (n != Flat(t))
      Fail(init->loc, "initializer has " + std::to_string(n) + " components, " + TypeName(t) +
                          " needs " + std::to_string(Flat(t)));
    init->type = t;
  }

  void CheckList(Expr& list, int* n) {
    for (auto& a : list.args) {
      if (a->kind == Expr::InitList) {
        CheckList(*a, n);
        continue;
      }
      CheckExpr(a);
      if (!a->type.Numeric() && a->type.base != Base::Struct)
        Fail(a->loc, "bad initializer element of type " + TypeName(a->type));
      *n += Flat(a->type);
    }
  }

  // ---- Conversions

  // Common type of two operands of an arithmetic or comparison operator.
  Type Common(const Type& a, const Type& b, Loc loc) {
    if (!a.Numeric() || !b.Numeric() || a.array || b.array)
      Fail(loc, "operands " + TypeName(a) + " and " + TypeName(b) + " are not numeric");
    Base base = std::max(a.base, b.base);
    Type t;
    if (a.Scalar()) {
      t = b;
    } else if (b.Scalar()) {
      t = a;
    } else if (a.Vector() && b.Vector()) {
      t = Type::Vec(base, std::min(a.cols, b.cols));
    } else if (a.Matrix() && b.Matrix()) {
      t = Type::Make(base, std::min(a.rows, b.rows), std::min(a.cols, b.cols), true);
    } else {
      Fail(loc, "incompatible operands " + TypeName(a) + " and " + TypeName(b));
    }
    t.base = base;
    return t;
  }

  static bool Convertible(const Type& from, const Type& to, bool explicit_cast) {
    if (from == to) return true;
    if (from.base == Base::Struct || to.base == Base::Struct)
      return from.st == to.st && from.array == to.array;
    if (from.base == Base::Sampler || to.base == Base::Sampler)
      return from.base == to.base && from.array == to.array &&
             (from.dim == to.dim || from.dim == Dim::Any || to.dim == Dim::Any);
    if (!from.Numeric() || !to.Numeric()) return false;
    if (from.array || to.array) return SameTranslated(from, to) || (from.Elem() == to.Elem() && from.array == to.array);
    if (from.Scalar()) return true;
    if (to.Scalar()) return true;  // first component
    if (from.Vector() && to.Vector()) return to.cols <= from.cols;
    if (from.Matrix() && to.Matrix()) return to.rows <= from.rows && to.cols <= from.cols;
    if (explicit_cast && from.Components() == to.Components()) return true;
    return false;
  }

  // Makes `e` have type `to`: retypes literals, wraps the rest.
  void Coerce(ExprP& e, const Type& to, const char* what) {
    const Type& from = e->type;
    if (from == to) return;
    if (!Convertible(from, to, false))
      Fail(e->loc, std::string("cannot convert ") + TypeName(from) + " to " + TypeName(to) +
                       " (" + what + ")");
    if (e->kind == Expr::Literal && to.Scalar()) {
      if (to.Floating() && !from.Floating()) e->fval = double(e->ival);
      if (!to.Floating() && from.Floating()) e->ival = int64_t(e->fval);
      if (to.base == Base::Bool) {
        e->ival = from.Floating() ? e->fval != 0 : e->ival != 0;
      }
      e->type = to;
      e->spec = to;
      return;
    }
    if (SameTranslated(from, to) && from.base != Base::Sampler) {
      e->type = to;  // half/float/double: the same GLSL type
      return;
    }
    if (from.base == Base::Sampler) return;  // a typed sampler for an untyped one
    auto c = std::make_unique<Expr>();
    c->kind = Expr::Convert;
    c->loc = e->loc;
    c->type = to;
    c->args.push_back(std::move(e));
    e = std::move(c);
  }

  // ---- Lvalues

  static VarDecl* Root(Expr* e) {
    while (e->kind == Expr::Member || e->kind == Expr::Index) e = e->args[0].get();
    return e->kind == Expr::Var ? e->var : nullptr;
  }

  void Written(Expr& e, Loc loc) {
    if (!e.lvalue) Fail(loc, "l-value required");
    VarDecl* v = Root(&e);
    if (!v) Fail(loc, "l-value required");
    if (v->flags & kConst) Fail(loc, "cannot write to const " + v->name);
    v->written = true;
  }

  // ---- Expressions

  void CheckExpr(ExprP& ep) {
    Expr& e = *ep;
    switch (e.kind) {
      case Expr::Literal:
        e.type = e.spec;
        break;
      case Expr::Var: {
        VarDecl* v = Lookup(e.name);
        if (!v) Fail(e.loc, "undeclared identifier '" + e.name + "'");
        if (v->global) UseGlobal(v);
        if (v->type.base == Base::String || v->type.base == Base::Texture)
          Fail(e.loc, "cannot use " + TypeName(v->type) + " " + v->name + " in an expression");
        e.var = v;
        e.type = v->type;
        e.lvalue = !(v->flags & kConst) && v->type.base != Base::Sampler;
        break;
      }
      case Expr::Unary:
        CheckUnary(e);
        break;
      case Expr::Binary:
        CheckBinary(e);
        break;
      case Expr::Assign:
        CheckAssign(e);
        break;
      case Expr::Ternary:
        CheckTernary(e);
        break;
      case Expr::Call:
        CheckCall(e);
        break;
      case Expr::Construct:
        CheckConstruct(e);
        break;
      case Expr::Cast: {
        CheckExpr(e.args[0]);
        Use(e.spec);
        const Type& from = e.args[0]->type;
        bool zero_struct = e.spec.base == Base::Struct && e.args[0]->kind == Expr::Literal &&
                           e.args[0]->type.Scalar();
        if (!zero_struct && !Convertible(from, e.spec, true))
          Fail(e.loc, "cannot cast " + TypeName(from) + " to " + TypeName(e.spec));
        e.type = e.spec;
        break;
      }
      case Expr::Member:
        CheckMember(e);
        break;
      case Expr::Index: {
        CheckExpr(e.args[0]);
        CheckExpr(e.args[1]);
        const Type& b = e.args[0]->type;
        if (!e.args[1]->type.Scalar()) Fail(e.loc, "index must be a scalar");
        Coerce(e.args[1], Type::Make(Base::Int), "index");
        if (b.array) {
          e.type = b.Elem();
        } else if (b.Vector()) {
          e.type = Type::Make(b.base);
        } else if (b.Matrix()) {
          e.type = Type::Vec(b.base, b.cols);
          if (b.cols == 1) e.type = Type::Make(b.base);
        } else {
          Fail(e.loc, "cannot index " + TypeName(b));
        }
        e.lvalue = e.args[0]->lvalue;
        break;
      }
      case Expr::Comma:
        for (auto& a : e.args) CheckExpr(a);
        e.type = e.args.back()->type;
        break;
      case Expr::InitList:
        Fail(e.loc, "initializer list outside an initializer");
      case Expr::Convert:
        break;
    }
  }

  void CheckUnary(Expr& e) {
    CheckExpr(e.args[0]);
    Type t = e.args[0]->type;
    if (!t.Numeric() || t.array) Fail(e.loc, std::string("bad operand for ") + OpText(e.op));
    switch (e.op) {
      case Op::Neg:
      case Op::Plus:
        if (t.base == Base::Bool) {
          t.base = Base::Int;
          Coerce(e.args[0], t, "operand");
        }
        e.type = t;
        break;
      case Op::Not:
        Coerce(e.args[0], t.WithBase(Base::Bool), "operand");
        e.type = t.WithBase(Base::Bool);
        break;
      case Op::BitNot:
        if (t.Floating() || t.base == Base::Bool) Fail(e.loc, "~ needs an integer operand");
        e.type = t;
        break;
      default:  // ++ / --
        Written(*e.args[0], e.loc);
        e.type = t;
        break;
    }
  }

  void CheckBinary(Expr& e) {
    CheckExpr(e.args[0]);
    CheckExpr(e.args[1]);
    Type t = Common(e.args[0]->type, e.args[1]->type, e.loc);
    switch (e.op) {
      case Op::And:
      case Op::Or:
        t.base = Base::Bool;
        Coerce(e.args[0], t, "operand");
        Coerce(e.args[1], t, "operand");
        e.type = t;
        return;
      case Op::Lt: case Op::Gt: case Op::Le: case Op::Ge: case Op::Eq: case Op::Ne:
        if (t.Matrix()) Fail(e.loc, "matrix comparisons are not supported");
        Coerce(e.args[0], t, "operand");
        Coerce(e.args[1], t, "operand");
        e.type = t.WithBase(Base::Bool);
        return;
      case Op::Shl: case Op::Shr: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        if (t.Floating()) Fail(e.loc, std::string(OpText(e.op)) + " needs integer operands");
        if (t.base == Base::Bool) t.base = Base::Int;
        break;
      default:
        if (t.base == Base::Bool) t.base = Base::Int;
        break;
    }
    Coerce(e.args[0], t, "operand");
    Coerce(e.args[1], t, "operand");
    e.type = t;
  }

  void CheckAssign(Expr& e) {
    CheckExpr(e.args[0]);
    CheckExpr(e.args[1]);
    Written(*e.args[0], e.loc);
    const Type& lt = e.args[0]->type;
    if (e.op == Op::None) {
      Coerce(e.args[1], lt, "assignment");
      e.type = lt;
      return;
    }
    Type c = Common(lt, e.args[1]->type, e.loc);
    if (c.base == Base::Bool) c.base = Base::Int;
    // The operation happens in the common base type; shapes beyond the
    // left side's are truncated, which is the same as converting the
    // right side to the left side's shape first.
    Type rt = lt.WithBase(c.base);
    Coerce(e.args[1], rt, "assignment");
    e.spec = rt;  // != lt: the emitter writes a = T(T'(a) op b)
    e.type = lt;
  }

  void CheckTernary(Expr& e) {
    for (auto& a : e.args) CheckExpr(a);
    const Type& a = e.args[1]->type;
    const Type& b = e.args[2]->type;
    Type t;
    if (a.base == Base::Struct || b.base == Base::Struct || a.array || b.array) {
      if (!(a == b)) Fail(e.loc, "?: operands differ: " + TypeName(a) + " and " + TypeName(b));
      t = a;
    } else {
      t = Common(a, b, e.loc);
    }
    const Type& c = e.args[0]->type;
    if (c.Scalar()) {
      Coerce(e.args[0], Type::Make(Base::Bool), "condition");
    } else {
      if (!t.Numeric() || t.array) Fail(e.loc, "vector condition with non-numeric operands");
      t = Common(t, c, e.loc).WithBase(t.base);
      Coerce(e.args[0], t.WithBase(Base::Bool), "condition");
    }
    Coerce(e.args[1], t, "operand");
    Coerce(e.args[2], t, "operand");
    e.type = t;
  }

  void CheckConstruct(Expr& e) {
    for (auto& a : e.args) CheckExpr(a);
    const Type& t = e.spec;
    if (!t.Numeric() || t.array) Fail(e.loc, "cannot construct " + TypeName(t));
    int n = 0;
    for (auto& a : e.args) {
      if (!a->type.Numeric() || a->type.array)
        Fail(a->loc, "bad constructor argument of type " + TypeName(a->type));
      n += a->type.Components();
    }
    bool splat = e.args.size() == 1 && e.args[0]->type.Scalar();
    if (!splat && n != t.Components())
      Fail(e.loc, TypeName(t) + " constructor needs " + std::to_string(t.Components()) +
                      " components, " + std::to_string(n) + " given");
    e.type = t;
  }

  void CheckMember(Expr& e) {
    CheckExpr(e.args[0]);
    const Type& b = e.args[0]->type;
    e.lvalue = e.args[0]->lvalue;
    if (b.base == Base::Struct && !b.array) {
      for (size_t i = 0; i < b.st->fields.size(); i++) {
        if (b.st->fields[i]->name == e.name) {
          e.field = int(i);
          e.type = b.st->fields[i]->type;
          return;
        }
      }
      Fail(e.loc, "'" + e.name + "' is not a member of " + TypeName(b));
    }
    if (!b.Numeric() || b.array) Fail(e.loc, "cannot select ." + e.name + " from " + TypeName(b));
    const std::string& s = e.name;
    if (b.Matrix()) {
      // _11.._44 (1-based) or _m00.._m33 (0-based), concatenated.
      size_t i = 0;
      while (i < s.size()) {
        if (s[i] != '_') Fail(e.loc, "bad matrix swizzle " + s);
        int r, c;
        if (i + 1 < s.size() && s[i + 1] == 'm') {
          if (i + 3 >= s.size()) Fail(e.loc, "bad matrix swizzle " + s);
          r = s[i + 2] - '0';
          c = s[i + 3] - '0';
          i += 4;
        } else {
          if (i + 2 >= s.size()) Fail(e.loc, "bad matrix swizzle " + s);
          r = s[i + 1] - '1';
          c = s[i + 2] - '1';
          i += 3;
        }
        if (r < 0 || c < 0 || r >= b.rows || c >= b.cols) Fail(e.loc, "bad matrix swizzle " + s);
        e.swz.push_back(uint8_t(r * 4 + c));
      }
    } else {
      const char* xyzw = "xyzw";
      const char* rgba = "rgba";
      const char* set = std::strchr(xyzw, s[0]) ? xyzw : rgba;
      for (char ch : s) {
        const char* p = std::strchr(set, ch);
        if (!p || ch == '\0') Fail(e.loc, "bad swizzle ." + s + " of " + TypeName(b));
        int k = int(p - set);
        if (k >= b.cols) Fail(e.loc, "swizzle ." + s + " out of range for " + TypeName(b));
        e.swz.push_back(uint8_t(k));
      }
    }
    if (e.swz.empty() || e.swz.size() > 4) Fail(e.loc, "bad swizzle ." + s);
    for (size_t i = 0; i < e.swz.size(); i++)
      for (size_t j = 0; j < i; j++)
        if (e.swz[i] == e.swz[j]) e.lvalue = false;
    e.type = e.swz.size() == 1 ? Type::Make(b.base) : Type::Vec(b.base, int(e.swz.size()));
  }

  // ---- Calls

  // Cost of passing `from` for `to`, -1 if impossible.
  static int Cost(const Type& from, const Type& to) {
    if (from == to) return 0;
    if (!Convertible(from, to, false)) return -1;
    if (SameTranslated(from, to)) return 1;
    if (from.base == Base::Sampler) return 1;
    bool same_shape = from.matrix == to.matrix && from.rows == to.rows && from.cols == to.cols &&
                      from.array == to.array;
    if (same_shape) return 2;
    if (from.Scalar()) return 3;
    return 4;
  }

  void CheckCall(Expr& e) {
    auto it = funcs_.find(e.name);
    if (it == funcs_.end()) {
      int id = FindIntrinsic(e.name);
      if (id < 0) Fail(e.loc, "unknown function " + e.name);
      for (auto& a : e.args) CheckExpr(a);
      e.intrinsic = id;
      CheckIntrinsic(e);
      return;
    }
    for (auto& a : e.args) CheckExpr(a);
    FuncDecl* best = nullptr;
    int best_cost = 0;
    for (FuncDecl* f : it->second) {
      FuncDecl* d = f->def ? f->def : f;
      if (e.args.size() > d->params.size()) continue;
      if (e.args.size() < d->params.size() && !d->params[e.args.size()]->init) continue;
      int total = 0;
      bool ok = true;
      for (size_t i = 0; i < e.args.size() && ok; i++) {
        const VarDecl& p = *d->params[i];
        int c = Cost(e.args[i]->type, p.type);
        if ((p.flags & kOut) && (!e.args[i]->lvalue || !SameTranslated(e.args[i]->type, p.type)))
          c = -1;
        if (c < 0) ok = false;
        total += c;
      }
      if (!ok) continue;
      if (!best || total < best_cost) {
        best = d;
        best_cost = total;
      }
    }
    if (!best) Fail(e.loc, "no overload of " + e.name + " matches these arguments");
    if (e.args.size() < best->params.size())
      Fail(e.loc, "default parameter values are not supported");
    for (size_t i = 0; i < e.args.size(); i++) {
      const VarDecl& p = *best->params[i];
      if (p.flags & kOut) {
        Written(*e.args[i], e.args[i]->loc);
        e.args[i]->type = p.type;  // half/float differences only
      } else {
        Coerce(e.args[i], p.type, "argument");
      }
    }
    if (!best->body) Fail(e.loc, "function " + e.name + " is declared but never defined");
    CheckFunction(best);
    if (cur_) cur_->callees.push_back(best);
    e.fn = best;
    e.type = best->ret;
  }

  void Args(Expr& e, size_t n) {
    if (e.args.size() != n)
      Fail(e.loc, e.name + " takes " + std::to_string(n) + " arguments, " +
                      std::to_string(e.args.size()) + " given");
    for (auto& a : e.args)
      if (!a->type.Numeric() || a->type.array)
        if (a->type.base != Base::Sampler || &a != &e.args[0])
          Fail(a->loc, "bad argument of type " + TypeName(a->type) + " for " + e.name);
  }

  void NoMatrix(Expr& e, const Type& t) {
    if (t.Matrix()) Fail(e.loc, e.name + " of a matrix is not supported");
  }

  void CheckIntrinsic(Expr& e) {
    auto& a = e.args;
    switch (e.intrinsic) {
      case kAcos: case kAsin: case kAtan: case kCeil: case kCos: case kCosh: case kDdx:
      case kDdy: case kDegrees: case kExp: case kExp2: case kFloor: case kFrac: case kFwidth:
      case kLog: case kLog10: case kLog2: case kRadians: case kRound: case kRsqrt:
      case kSaturate: case kSin: case kSinh: case kSqrt: case kTan: case kTanh: case kTrunc: {
        Args(e, 1);
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        if ((e.intrinsic == kDdx || e.intrinsic == kDdy || e.intrinsic == kFwidth) &&
            tg_.stage != Stage::Pixel)
          Fail(e.loc, e.name + " in a vertex shader");
        Coerce(a[0], t, "argument");
        e.type = t;
        return;
      }
      case kAbs: case kSign: {
        Args(e, 1);
        Type t = a[0]->type;
        NoMatrix(e, t);
        if (t.base == Base::Bool || t.base == Base::Uint) t.base = Base::Int;
        Coerce(a[0], t, "argument");
        e.type = t;
        return;
      }
      case kAtan2: case kFmod: case kPow: case kStep: case kLdexp: {
        Args(e, 2);
        Type t = Floating(Common(a[0]->type, a[1]->type, e.loc));
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        Coerce(a[1], t, "argument");
        e.type = t;
        return;
      }
      case kMin: case kMax: {
        Args(e, 2);
        Type t = Common(a[0]->type, a[1]->type, e.loc);
        NoMatrix(e, t);
        if (t.base == Base::Bool) t.base = Base::Int;
        Coerce(a[0], t, "argument");
        Coerce(a[1], t, "argument");
        e.type = t;
        return;
      }
      case kClamp: case kLerp: case kSmoothstep: {
        Args(e, 3);
        Type t = Common(Common(a[0]->type, a[1]->type, e.loc), a[2]->type, e.loc);
        if (e.intrinsic != kClamp) t = Floating(t);
        if (t.base == Base::Bool) t.base = Base::Int;
        NoMatrix(e, t);
        for (auto& x : a) Coerce(x, t, "argument");
        e.type = t;
        return;
      }
      case kDot: case kDistance: {
        Args(e, 2);
        Type t = Floating(Common(a[0]->type, a[1]->type, e.loc));
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        Coerce(a[1], t, "argument");
        e.type = Type::Make(t.base);
        return;
      }
      case kLength: case kNormalize: {
        Args(e, 1);
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        e.type = e.intrinsic == kLength ? Type::Make(t.base) : t;
        return;
      }
      case kReflect: case kRefract: case kFaceforward: {
        Args(e, e.intrinsic == kReflect ? 2 : 3);
        Type t = Floating(Common(a[0]->type, a[1]->type, e.loc));
        if (e.intrinsic == kFaceforward) t = Floating(Common(t, a[2]->type, e.loc));
        if (!t.Vector()) Fail(e.loc, e.name + " needs vectors");
        Coerce(a[0], t, "argument");
        Coerce(a[1], t, "argument");
        if (e.intrinsic == kRefract) Coerce(a[2], Type::Make(t.base), "argument");
        if (e.intrinsic == kFaceforward) Coerce(a[2], t, "argument");
        e.type = t;
        return;
      }
      case kCross: {
        Args(e, 2);
        Type t = Type::Vec(Base::Float, 3);
        Coerce(a[0], t, "argument");
        Coerce(a[1], t, "argument");
        e.type = t;
        return;
      }
      case kAll: case kAny: {
        Args(e, 1);
        NoMatrix(e, a[0]->type);
        e.type = Type::Make(Base::Bool);
        return;
      }
      case kIsnan: case kIsinf: case kIsfinite: {
        Args(e, 1);
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        e.type = t.WithBase(Base::Bool);
        return;
      }
      case kMul:
        CheckMul(e);
        return;
      case kTranspose: {
        Args(e, 1);
        const Type& t = a[0]->type;
        if (!t.Matrix()) Fail(e.loc, "transpose needs a matrix");
        e.type = Type::Make(t.base, t.cols, t.rows, true);
        return;
      }
      case kDeterminant: {
        Args(e, 1);
        Type t = Floating(a[0]->type);
        if (!t.Matrix() || t.rows != t.cols) Fail(e.loc, "determinant needs a square matrix");
        Coerce(a[0], t, "argument");
        e.type = Type::Make(t.base);
        return;
      }
      case kClip: {
        Args(e, 1);
        if (tg_.stage != Stage::Pixel) Fail(e.loc, "clip in a vertex shader");
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        e.type = Type::Make(Base::Void);
        return;
      }
      case kSincos: {
        Args(e, 3);
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        for (int i = 1; i < 3; i++) {
          Written(*a[i], a[i]->loc);
          if (!SameTranslated(a[i]->type, t)) Fail(a[i]->loc, "sincos outputs must be " + TypeName(t));
        }
        e.type = Type::Make(Base::Void);
        return;
      }
      case kModf: case kFrexp: {
        Args(e, 2);
        Type t = Floating(a[0]->type);
        NoMatrix(e, t);
        Coerce(a[0], t, "argument");
        Written(*a[1], a[1]->loc);
        if (!SameTranslated(a[1]->type, t)) Fail(a[1]->loc, e.name + " output must be " + TypeName(t));
        e.type = t;
        return;
      }
      case kLit: {
        Args(e, 3);
        for (auto& x : a) Coerce(x, Type::Make(Base::Float), "argument");
        e.type = Type::Vec(Base::Float, 4);
        return;
      }
      case kD3DColorToUbyte4: {
        Args(e, 1);
        Coerce(a[0], Type::Vec(Base::Float, 4), "argument");
        e.type = Type::Vec(Base::Int, 4);
        return;
      }
      default:
        CheckTexture(e);
        return;
    }
  }

  void CheckMul(Expr& e) {
    Args(e, 2);
    auto& a = e.args;
    Type x = a[0]->type, y = a[1]->type;
    Base base = std::max(x.base, y.base);
    if (base == Base::Bool) base = Base::Int;
    if (x.Scalar() || y.Scalar()) {
      Type t = Common(x, y, e.loc);
      t.base = base;
      Coerce(a[0], x.Scalar() ? Type::Make(base) : t, "argument");
      Coerce(a[1], y.Scalar() ? Type::Make(base) : t, "argument");
      e.type = t;
      return;
    }
    if (x.Vector() && y.Vector()) {
      int n = std::min(x.cols, y.cols);
      Coerce(a[0], Type::Vec(base, n), "argument");
      Coerce(a[1], Type::Vec(base, n), "argument");
      e.type = Type::Make(base);
      return;
    }
    if (x.Vector() && y.Matrix()) {
      if (x.cols < y.rows) Fail(e.loc, "mul: " + TypeName(x) + " is too short for " + TypeName(y));
      Coerce(a[0], Type::Vec(base, y.rows), "argument");
      Coerce(a[1], y.WithBase(base), "argument");
      e.type = y.cols == 1 ? Type::Make(base) : Type::Vec(base, y.cols);
      return;
    }
    if (x.Matrix() && y.Vector()) {
      if (y.cols < x.cols) Fail(e.loc, "mul: " + TypeName(y) + " is too short for " + TypeName(x));
      Coerce(a[0], x.WithBase(base), "argument");
      Coerce(a[1], Type::Vec(base, x.cols), "argument");
      e.type = x.rows == 1 ? Type::Make(base) : Type::Vec(base, x.rows);
      return;
    }
    if (x.Matrix() && y.Matrix()) {
      if (x.cols != y.rows) Fail(e.loc, "mul: " + TypeName(x) + " by " + TypeName(y));
      Coerce(a[0], x.WithBase(base), "argument");
      Coerce(a[1], y.WithBase(base), "argument");
      e.type = Type::Make(base, x.rows, y.cols, true);
      return;
    }
    Fail(e.loc, "mul of " + TypeName(x) + " and " + TypeName(y));
  }

  void CheckTexture(Expr& e) {
    int k = e.intrinsic - kTex1D;
    Dim dim = Dim(1 + k / 5);  // D1, D2, D3, Cube
    int variant = k % 5;       // plain, bias, grad, lod, proj
    auto& a = e.args;
    bool grads = variant == 2 || (variant == 0 && a.size() == 4);
    Args(e, grads ? 4 : 2);
    if (a[0]->type.base != Base::Sampler) Fail(a[0]->loc, e.name + " needs a sampler");
    // The sampler: a sampler variable or a constant element of an array.
    Expr* s = a[0].get();
    if (s->kind == Expr::Index) s = s->args[0].get();
    if (s->kind != Expr::Var) Fail(a[0]->loc, "sampler argument must be a variable");
    VarDecl* v = s->var;
    Dim& d = v->sampler_dim;
    if (d == Dim::Any) d = v->type.dim;
    if (d == Dim::Any) d = dim;
    if (d != dim) Fail(e.loc, e.name + " with a sampler of another dimension");
    if (tg_.stage == Stage::Vertex && variant != 3)
      Fail(e.loc, "vertex shaders can only use the lod variant (" + e.name + ")");
    int n = dim == Dim::D1 ? 1 : dim == Dim::D2 ? 2 : 3;
    Coerce(a[1], variant == 1 || variant == 3 || variant == 4 ? Type::Vec(Base::Float, 4)
                 : n == 1 ? Type::Make(Base::Float) : Type::Vec(Base::Float, n),
           "coordinates");
    if (grads) {
      Type g = n == 1 ? Type::Make(Base::Float) : Type::Vec(Base::Float, n);
      Coerce(a[2], g, "gradient");
      Coerce(a[3], g, "gradient");
    }
    e.type = Type::Vec(Base::Float, 4);
  }

  // ---- After checking: storage of the globals the entry point reaches.

  static bool ConstantExpr(const Expr& e) {
    switch (e.kind) {
      case Expr::Literal:
        return true;
      case Expr::Var:
        return e.var && e.var->cls == VarDecl::Class::Const;
      case Expr::Unary:
        if (e.op != Op::Neg && e.op != Op::Plus && e.op != Op::Not && e.op != Op::BitNot) return false;
        [[fallthrough]];
      case Expr::Binary: case Expr::Ternary: case Expr::Construct: case Expr::Cast:
      case Expr::InitList: case Expr::Convert: case Expr::Member: case Expr::Index:
        for (const auto& a : e.args)
          if (!ConstantExpr(*a)) return false;
        return true;
      default:
        return false;
    }
  }

  void Classify() {
    for (auto& g : u_.globals) {
      if (!g->used) continue;
      if (g->flags & kStatic) {
        g->cls = (g->flags & kConst) && g->init && ConstantExpr(*g->init) ? VarDecl::Class::Const
                                                                         : VarDecl::Class::Private;
      } else if (g->written) {
        // Writes make a uniform a private copy (D3DX's compiler accepts
        // this); it starts at zero here, not at the register's value.
        g->cls = VarDecl::Class::Private;
      } else {
        g->cls = VarDecl::Class::Uniform;
      }
    }
  }

  Unit& u_;
  Target tg_;
  std::map<std::string, VarDecl*> globals_;
  std::map<std::string, std::vector<FuncDecl*>> funcs_;
  std::vector<std::map<std::string, VarDecl*>> scopes_;
  FuncDecl* cur_ = nullptr;
};

}  // namespace

FuncDecl* Check(Unit& unit, const std::string& entry, const Target& target) {
  return Checker(unit, target).Run(entry);
}

}  // namespace recomp::hlsl

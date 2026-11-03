// Recursive-descent parser for the D3D9 HLSL dialect: structs, typedefs,
// globals (storage classes, semantics, register bindings, annotations,
// initialisers), functions and statements. Effect syntax (technique
// blocks, sampler_state initialisers) is skipped: D3DXCompileShader
// compiles one entry point and ignores it.

#include <cctype>
#include <cstdlib>

#include "runtime/hlsl/ast.h"

namespace recomp::hlsl {

const char* OpText(Op op) {
  switch (op) {
    case Op::Neg: return "-";
    case Op::Plus: return "+";
    case Op::Not: return "!";
    case Op::BitNot: return "~";
    case Op::PreInc: case Op::PostInc: return "++";
    case Op::PreDec: case Op::PostDec: return "--";
    case Op::Mul: return "*";
    case Op::Div: return "/";
    case Op::Mod: return "%";
    case Op::Add: return "+";
    case Op::Sub: return "-";
    case Op::Shl: return "<<";
    case Op::Shr: return ">>";
    case Op::Lt: return "<";
    case Op::Gt: return ">";
    case Op::Le: return "<=";
    case Op::Ge: return ">=";
    case Op::Eq: return "==";
    case Op::Ne: return "!=";
    case Op::BitAnd: return "&";
    case Op::BitXor: return "^";
    case Op::BitOr: return "|";
    case Op::And: return "&&";
    case Op::Or: return "||";
    case Op::None: break;
  }
  return "";
}

std::string TypeName(const Type& t) {
  std::string s;
  switch (t.base) {
    case Base::Void: s = "void"; break;
    case Base::Bool: s = "bool"; break;
    case Base::Int: s = "int"; break;
    case Base::Uint: s = "uint"; break;
    case Base::Half: s = "half"; break;
    case Base::Float: s = "float"; break;
    case Base::Double: s = "double"; break;
    case Base::String: s = "string"; break;
    case Base::Sampler: s = "sampler"; break;
    case Base::Texture: s = "texture"; break;
    case Base::Struct: s = t.st ? "struct " + t.st->name : "struct"; break;
  }
  if (t.base == Base::Sampler || t.base == Base::Texture) {
    static const char* const kDims[] = {"", "1D", "2D", "3D", "CUBE"};
    s += kDims[int(t.dim)];
  } else if (t.matrix) {
    s += std::to_string(t.rows) + "x" + std::to_string(t.cols);
  } else if (t.cols > 1) {
    s += std::to_string(t.cols);
  }
  if (t.array) s += "[" + std::to_string(t.array) + "]";
  return s;
}

namespace {

struct Modifier {
  const char* name;
  uint32_t flag;
};
const Modifier kModifiers[] = {
    {"static", kStatic}, {"uniform", kUniform}, {"extern", kExtern}, {"shared", kShared},
    {"const", kConst}, {"volatile", kVolatile}, {"in", kIn}, {"out", kOut},
    {"inout", kIn | kOut}, {"row_major", kRowMajor}, {"column_major", kColumnMajor},
    {"nointerpolation", kNoInterpolation}, {"centroid", kCentroid}, {"linear", 0},
    {"noperspective", 0}, {"inline", 0}, {"precise", 0},
};

bool ScalarBase(const std::string& s, Base* b) {
  static const std::pair<const char*, Base> kBases[] = {
      {"bool", Base::Bool}, {"int", Base::Int}, {"uint", Base::Uint}, {"dword", Base::Uint},
      {"half", Base::Half}, {"float", Base::Float}, {"double", Base::Double},
  };
  for (const auto& [n, base] : kBases)
    if (s == n) {
      *b = base;
      return true;
    }
  return false;
}

// float, float3, float4x4, ... (not vector<>/matrix<>).
bool BuiltinNumeric(const std::string& s, Type* out) {
  size_t i = 0;
  while (i < s.size() && std::isalpha(uint8_t(s[i]))) i++;
  Base b;
  if (!ScalarBase(s.substr(0, i), &b)) return false;
  std::string rest = s.substr(i);
  if (rest.empty()) {
    *out = Type::Make(b);
    return true;
  }
  auto dig = [](char c) { return c >= '1' && c <= '4'; };
  if (rest.size() == 1 && dig(rest[0])) {
    *out = Type::Vec(b, rest[0] - '0');
    return true;
  }
  if (rest.size() == 3 && dig(rest[0]) && rest[1] == 'x' && dig(rest[2])) {
    *out = Type::Make(b, rest[0] - '0', rest[2] - '0', true);
    return true;
  }
  return false;
}

bool SamplerOrTexture(const std::string& s, Type* out) {
  std::string l;
  for (char c : s) l += char(std::tolower(uint8_t(c)));
  static const std::pair<const char*, Dim> kDims[] = {
      {"", Dim::Any}, {"1d", Dim::D1}, {"2d", Dim::D2}, {"3d", Dim::D3}, {"cube", Dim::Cube},
  };
  for (const auto& [suffix, dim] : kDims) {
    std::string d = suffix;
    if (l == "sampler" + d || l == "texture" + d) {
      // HLSL spells these sampler2D, samplerCUBE, texture2D, Texture, ...
      out->base = l[0] == 's' ? Base::Sampler : Base::Texture;
      out->dim = dim;
      return true;
    }
  }
  return false;
}

class Parser {
 public:
  Parser(std::vector<Token> toks, uint32_t flags) : t_(std::move(toks)) {
    Token end;
    end.kind = Tok::End;
    end.loc = t_.empty() ? Loc{1, 1} : t_.back().loc;
    t_.push_back(end);
    if (flags & kPackMatrixRowMajor) pack_ = kRowMajor;
  }

  Unit Run() {
    while (Cur().kind != Tok::End) TopLevel();
    return std::move(u_);
  }

 private:
  // ---- Token helpers

  const Token& Cur() const { return t_[p_]; }
  const Token& Ahead(size_t k) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
  bool Is(const char* s) const {
    return (Cur().kind == Tok::Punct || Cur().kind == Tok::Ident) && Cur().text == s;
  }
  bool Accept(const char* s) {
    if (!Is(s)) return false;
    p_++;
    return true;
  }
  void Expect(const char* s) {
    if (!Accept(s)) Fail(Cur().loc, std::string("expected '") + s + "', found " + Describe(Cur()));
  }
  static std::string Describe(const Token& t) {
    return t.kind == Tok::End ? "end of file" : "'" + t.text + "'";
  }
  std::string Ident() {
    if (Cur().kind != Tok::Ident) Fail(Cur().loc, "expected a name, found " + Describe(Cur()));
    return t_[p_++].text;
  }
  // Skips a balanced { } (Cur() is the opening brace).
  void SkipBraces() {
    Loc start = Cur().loc;
    Expect("{");
    int depth = 1;
    while (depth) {
      if (Cur().kind == Tok::End) Fail(start, "unbalanced {");
      if (Is("{")) depth++;
      if (Is("}")) depth--;
      p_++;
    }
  }
  // Annotations: < ... >
  void SkipAnnotations() {
    if (!Is("<")) return;
    Loc start = Cur().loc;
    p_++;
    while (!Is(">")) {
      if (Cur().kind == Tok::End) Fail(start, "unterminated annotation");
      p_++;
    }
    p_++;
  }

  // ---- Types

  bool IsTypeName(const Token& t) const {
    if (t.kind != Tok::Ident) return false;
    Type ty;
    return BuiltinNumeric(t.text, &ty) || SamplerOrTexture(t.text, &ty) || t.text == "void" ||
           t.text == "string" || t.text == "vector" || t.text == "matrix" ||
           t.text == "struct" || structs_.count(t.text) || typedefs_.count(t.text);
  }
  bool IsModifier(const Token& t) const {
    if (t.kind != Tok::Ident) return false;
    for (const auto& m : kModifiers)
      if (t.text == m.name) return true;
    return false;
  }

  uint32_t Modifiers() {
    uint32_t f = 0;
    while (IsModifier(Cur())) {
      for (const auto& m : kModifiers)
        if (Cur().text == m.name) f |= m.flag;
      p_++;
    }
    return f;
  }

  Type ParseType() {
    Loc loc = Cur().loc;
    std::string n = Ident();
    Type t;
    if (BuiltinNumeric(n, &t) || SamplerOrTexture(n, &t)) return t;
    if (n == "void") return Type::Make(Base::Void);
    if (n == "string") return Type::Make(Base::String);
    if (n == "vector" || n == "matrix") {
      bool mat = n == "matrix";
      if (!Accept("<")) return mat ? Type::Make(Base::Float, 4, 4, true) : Type::Vec(Base::Float, 4);
      Base b;
      std::string bn = Ident();
      if (!ScalarBase(bn, &b)) Fail(loc, "bad " + n + " element type " + bn);
      Expect(",");
      int r = int(ConstInt());
      int c = 1;
      if (mat) {
        Expect(",");
        c = int(ConstInt());
      }
      Expect(">");
      if (r < 1 || r > 4 || c < 1 || c > 4) Fail(loc, "bad " + n + " size");
      return mat ? Type::Make(b, r, c, true) : (r == 1 ? Type::Make(b) : Type::Vec(b, r));
    }
    if (n == "struct") {
      if (Cur().kind == Tok::Ident && !Ahead(1).text.empty() && Ahead(1).text != "{") {
        auto it = structs_.find(Cur().text);
        if (it != structs_.end()) {
          p_++;
          Type s;
          s.base = Base::Struct;
          s.st = it->second;
          return s;
        }
      }
      return StructBody(loc);
    }
    if (auto it = structs_.find(n); it != structs_.end()) {
      Type s;
      s.base = Base::Struct;
      s.st = it->second;
      return s;
    }
    if (auto it = typedefs_.find(n); it != typedefs_.end()) return it->second;
    Fail(loc, "unknown type " + n);
  }

  // After `struct`: [Name] { fields }
  Type StructBody(Loc loc) {
    auto sd = std::make_unique<StructDecl>();
    sd->loc = loc;
    if (Cur().kind == Tok::Ident) sd->name = Ident();
    Expect("{");
    while (!Accept("}")) {
      uint32_t f = Modifiers();
      Type base = ParseType();
      do {
        auto v = std::make_unique<VarDecl>();
        v->loc = Cur().loc;
        v->name = Ident();
        v->flags = f;
        v->type = ArraySuffix(base);
        PostDeclarator(v.get());
        sd->fields.push_back(std::move(v));
      } while (Accept(","));
      Expect(";");
    }
    if (sd->fields.empty()) Fail(loc, "empty struct");
    Type s;
    s.base = Base::Struct;
    s.st = sd.get();
    if (!sd->name.empty()) structs_[sd->name] = sd.get();
    u_.structs.push_back(std::move(sd));
    return s;
  }

  Type ArraySuffix(Type t) {
    if (Accept("[")) {
      Loc loc = Cur().loc;
      int64_t n = ConstInt();
      Expect("]");
      if (n < 1) Fail(loc, "array size must be positive");
      if (t.array) Fail(loc, "arrays of arrays are not supported");
      t.array = uint32_t(n);
      if (Is("[")) Fail(Cur().loc, "multi-dimensional arrays are not supported");
    }
    return t;
  }

  // Semantics, register bindings and annotations after a declarator.
  void PostDeclarator(VarDecl* v) {
    for (;;) {
      if (Is("<")) {
        SkipAnnotations();
        continue;
      }
      if (!Accept(":")) return;
      Loc loc = Cur().loc;
      std::string s = Ident();
      if (s == "register") {
        Expect("(");
        std::string r = Ident();
        if (Accept(",")) Fail(loc, "profile-specific register bindings are not supported");
        Expect(")");
        char set = char(std::tolower(uint8_t(r[0])));
        if (!(set == 'c' || set == 'i' || set == 'b' || set == 's') || r.size() < 2)
          Fail(loc, "bad register " + r);
        char* end = nullptr;
        unsigned long idx = std::strtoul(r.c_str() + 1, &end, 10);
        if (*end) Fail(loc, "bad register " + r);
        v->reg_set = set;
        v->reg = uint32_t(idx);
      } else if (s == "packoffset") {
        Fail(loc, "packoffset is not supported");
      } else {
        for (char& c : s) c = char(std::toupper(uint8_t(c)));
        // TEXCOORD0_centroid: centroid interpolation of that input.
        const std::string kCentroidSuffix = "_CENTROID";
        if (s.size() > kCentroidSuffix.size() &&
            s.compare(s.size() - kCentroidSuffix.size(), kCentroidSuffix.size(), kCentroidSuffix) == 0) {
          s.resize(s.size() - kCentroidSuffix.size());
          v->flags |= kCentroid;
        }
        v->semantic = s;
      }
    }
  }

  // ---- Constant integer expressions (array sizes, vector<> sizes)

  int64_t ConstInt() {
    ExprP e = Conditional();
    return EvalInt(*e);
  }

  int64_t EvalInt(const Expr& e) {
    switch (e.kind) {
      case Expr::Literal:
        return e.spec.Floating() ? int64_t(e.fval) : e.ival;
      case Expr::Unary: {
        int64_t a = EvalInt(*e.args[0]);
        if (e.op == Op::Neg) return -a;
        if (e.op == Op::Plus) return a;
        if (e.op == Op::Not) return !a;
        if (e.op == Op::BitNot) return ~a;
        break;
      }
      case Expr::Binary: {
        int64_t a = EvalInt(*e.args[0]), b = EvalInt(*e.args[1]);
        switch (e.op) {
          case Op::Add: return a + b;
          case Op::Sub: return a - b;
          case Op::Mul: return a * b;
          case Op::Div:
          case Op::Mod:
            if (!b) Fail(e.loc, "division by zero");
            return e.op == Op::Div ? a / b : a % b;
          case Op::Shl: return a << b;
          case Op::Shr: return a >> b;
          case Op::BitAnd: return a & b;
          case Op::BitOr: return a | b;
          case Op::BitXor: return a ^ b;
          default: break;
        }
        break;
      }
      case Expr::Var:
        for (const auto& g : u_.globals)
          if (g->name == e.name && (g->flags & kStatic) && (g->flags & kConst) && g->init)
            return EvalInt(*g->init);
        break;
      default:
        break;
    }
    Fail(e.loc, "expected a constant integer expression");
  }

  // ---- Top level

  void TopLevel() {
    const Token& t = Cur();
    if (t.kind == Tok::Pragma) {
      Pragma(t);
      p_++;
      return;
    }
    if (Accept(";")) return;
    if (t.kind == Tok::Ident &&
        (t.text == "technique" || t.text == "technique9" || t.text == "technique10")) {
      p_++;
      if (Cur().kind == Tok::Ident) p_++;
      SkipAnnotations();
      SkipBraces();
      return;
    }
    if (Accept("typedef")) {
      uint32_t f = Modifiers();
      (void)f;
      Type base = ParseType();
      do {
        std::string n = Ident();
        typedefs_[n] = ArraySuffix(base);
      } while (Accept(","));
      Expect(";");
      return;
    }
    Loc loc = t.loc;
    uint32_t f = Modifiers();
    Type base = ParseType();
    if (base.base == Base::Struct && Accept(";")) return;  // struct definition
    std::string name = Ident();
    if (Is("(")) {
      Function(loc, f, base, name);
      return;
    }
    for (;;) {
      auto v = std::make_unique<VarDecl>();
      v->loc = loc;
      v->name = name;
      v->flags = f;
      v->global = true;
      v->type = ArraySuffix(base);
      ApplyPacking(v.get());
      PostDeclarator(v.get());
      if (Accept("=")) v->init = Initializer();
      u_.globals.push_back(std::move(v));
      if (!Accept(",")) break;
      loc = Cur().loc;
      name = Ident();
    }
    Expect(";");
  }

  void Pragma(const Token& t) {
    if (t.text.rfind("pack_matrix", 0) == 0) {
      if (t.text.find("row_major") != std::string::npos) pack_ = kRowMajor;
      else if (t.text.find("column_major") != std::string::npos) pack_ = kColumnMajor;
      else Fail(t.loc, "bad #pragma pack_matrix");
    }
    // Other pragmas (warning control, def) do not change the translation.
  }

  void ApplyPacking(VarDecl* v) const {
    if (!(v->flags & (kRowMajor | kColumnMajor))) v->flags |= pack_ ? pack_ : kColumnMajor;
  }

  ExprP Initializer() {
    if (Is("sampler_state")) {
      p_++;
      SkipBraces();
      return nullptr;
    }
    if (Is("{")) return InitList();
    return Assignment();
  }

  ExprP InitList() {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::InitList;
    e->loc = Cur().loc;
    Expect("{");
    while (!Accept("}")) {
      e->args.push_back(Is("{") ? InitList() : Assignment());
      if (!Accept(",")) {
        Expect("}");
        break;
      }
    }
    return e;
  }

  void Function(Loc loc, uint32_t flags, Type ret, std::string name) {
    (void)flags;
    auto fn = std::make_unique<FuncDecl>();
    fn->loc = loc;
    fn->name = std::move(name);
    fn->ret = ret;
    Expect("(");
    if (Is("void") && Ahead(1).text == ")") p_++;
    while (!Accept(")")) {
      auto prm = std::make_unique<VarDecl>();
      prm->loc = Cur().loc;
      prm->flags = Modifiers();
      prm->type = ParseType();
      prm->param = true;
      prm->name = Ident();
      prm->type = ArraySuffix(prm->type);
      ApplyPacking(prm.get());
      PostDeclarator(prm.get());
      if (Accept("=")) prm->init = Assignment();
      fn->params.push_back(std::move(prm));
      if (!Accept(",")) {
        Expect(")");
        break;
      }
    }
    if (Accept(":")) {
      std::string s = Ident();
      for (char& c : s) c = char(std::toupper(uint8_t(c)));
      fn->ret_semantic = s;
    }
    if (!Accept(";")) fn->body = Block();
    u_.funcs.push_back(std::move(fn));
  }

  // ---- Statements

  StmtP NewStmt(Stmt::Kind k) {
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->loc = Cur().loc;
    return s;
  }

  StmtP Block() {
    auto s = NewStmt(Stmt::Block);
    Expect("{");
    while (!Accept("}")) {
      if (Cur().kind == Tok::End) Fail(s->loc, "unterminated block");
      s->body.push_back(Statement());
    }
    return s;
  }

  bool StartsDeclaration() const {
    if (IsModifier(Cur())) return true;
    if (!IsTypeName(Cur())) return false;
    // `float4(...)` starts an expression; `float4 x` a declaration.
    if (Cur().text == "struct") return true;
    if (Cur().text == "vector" || Cur().text == "matrix") return Ahead(1).text != "(";
    return Ahead(1).kind == Tok::Ident;
  }

  StmtP Declaration() {
    auto s = NewStmt(Stmt::Decl);
    uint32_t f = Modifiers();
    Type base = ParseType();
    do {
      auto v = std::make_unique<VarDecl>();
      v->loc = Cur().loc;
      v->name = Ident();
      v->flags = f;
      v->type = ArraySuffix(base);
      ApplyPacking(v.get());
      PostDeclarator(v.get());
      if (Accept("=")) v->init = Initializer();
      s->vars.push_back(std::move(v));
    } while (Accept(","));
    Expect(";");
    return s;
  }

  StmtP Statement() {
    // [unroll], [loop], [branch], [flatten], [unroll(n)], ...: hints only.
    while (Is("[") && Ahead(1).kind == Tok::Ident &&
           (Ahead(2).text == "]" || Ahead(2).text == "(")) {
      p_ += 2;
      if (Accept("(")) {
        while (!Accept(")")) p_++;
      }
      Expect("]");
    }
    if (Is("{")) return Block();
    if (Accept(";")) return NewStmt(Stmt::Empty);
    if (Is("if")) {
      auto s = NewStmt(Stmt::If);
      p_++;
      Expect("(");
      s->cond = Expression();
      Expect(")");
      s->then = Statement();
      if (Accept("else")) s->els = Statement();
      return s;
    }
    if (Is("for")) {
      auto s = NewStmt(Stmt::For);
      p_++;
      Expect("(");
      if (!Is(";")) {
        if (StartsDeclaration()) {
          s->init = Declaration();
        } else {
          s->init = NewStmt(Stmt::Expression);
          s->init->expr = Expression();
          Expect(";");
        }
      } else {
        p_++;
      }
      if (!Is(";")) s->cond = Expression();
      Expect(";");
      if (!Is(")")) s->step = Expression();
      Expect(")");
      s->then = Statement();
      return s;
    }
    if (Is("while")) {
      auto s = NewStmt(Stmt::While);
      p_++;
      Expect("(");
      s->cond = Expression();
      Expect(")");
      s->then = Statement();
      return s;
    }
    if (Is("do")) {
      auto s = NewStmt(Stmt::Do);
      p_++;
      s->then = Statement();
      Expect("while");
      Expect("(");
      s->cond = Expression();
      Expect(")");
      Expect(";");
      return s;
    }
    if (Is("return")) {
      auto s = NewStmt(Stmt::Return);
      p_++;
      if (!Is(";")) s->expr = Expression();
      Expect(";");
      return s;
    }
    if (Is("break") || Is("continue") || Is("discard")) {
      auto s = NewStmt(Is("break") ? Stmt::Break : Is("continue") ? Stmt::Continue : Stmt::Discard);
      p_++;
      Expect(";");
      return s;
    }
    if (Is("switch")) Fail(Cur().loc, "switch is not supported");
    if (StartsDeclaration()) return Declaration();
    auto s = NewStmt(Stmt::Expression);
    s->expr = Expression();
    Expect(";");
    return s;
  }

  // ---- Expressions

  ExprP NewExpr(Expr::Kind k, Loc loc) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->loc = loc;
    return e;
  }

  ExprP Expression() {
    ExprP e = Assignment();
    if (!Is(",")) return e;
    ExprP c = NewExpr(Expr::Comma, e->loc);
    c->args.push_back(std::move(e));
    while (Accept(",")) c->args.push_back(Assignment());
    return c;
  }

  ExprP Assignment() {
    ExprP lhs = Conditional();
    static const std::pair<const char*, Op> kOps[] = {
        {"=", Op::None}, {"+=", Op::Add}, {"-=", Op::Sub}, {"*=", Op::Mul}, {"/=", Op::Div},
        {"%=", Op::Mod}, {"<<=", Op::Shl}, {">>=", Op::Shr}, {"&=", Op::BitAnd},
        {"|=", Op::BitOr}, {"^=", Op::BitXor},
    };
    for (const auto& [s, op] : kOps) {
      if (Cur().kind == Tok::Punct && Cur().text == s) {
        Loc loc = Cur().loc;
        p_++;
        ExprP e = NewExpr(Expr::Assign, loc);
        e->op = op;
        e->args.push_back(std::move(lhs));
        e->args.push_back(Assignment());
        return e;
      }
    }
    return lhs;
  }

  ExprP Conditional() {
    ExprP c = Binary(0);
    if (!Is("?")) return c;
    ExprP e = NewExpr(Expr::Ternary, Cur().loc);
    p_++;
    e->args.push_back(std::move(c));
    e->args.push_back(Assignment());
    Expect(":");
    e->args.push_back(Assignment());
    return e;
  }

  ExprP Binary(int level) {
    static const std::vector<std::vector<std::pair<const char*, Op>>> kLevels = {
        {{"||", Op::Or}},
        {{"&&", Op::And}},
        {{"|", Op::BitOr}},
        {{"^", Op::BitXor}},
        {{"&", Op::BitAnd}},
        {{"==", Op::Eq}, {"!=", Op::Ne}},
        {{"<", Op::Lt}, {">", Op::Gt}, {"<=", Op::Le}, {">=", Op::Ge}},
        {{"<<", Op::Shl}, {">>", Op::Shr}},
        {{"+", Op::Add}, {"-", Op::Sub}},
        {{"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Mod}},
    };
    if (level == int(kLevels.size())) return Unary();
    ExprP a = Binary(level + 1);
    for (;;) {
      Op op = Op::None;
      if (Cur().kind == Tok::Punct)
        for (const auto& [s, o] : kLevels[level])
          if (Cur().text == s) op = o;
      if (op == Op::None) return a;
      ExprP e = NewExpr(Expr::Binary, Cur().loc);
      p_++;
      e->op = op;
      e->args.push_back(std::move(a));
      e->args.push_back(Binary(level + 1));
      a = std::move(e);
    }
  }

  ExprP Unary() {
    static const std::pair<const char*, Op> kOps[] = {
        {"-", Op::Neg}, {"+", Op::Plus}, {"!", Op::Not}, {"~", Op::BitNot},
        {"++", Op::PreInc}, {"--", Op::PreDec},
    };
    for (const auto& [s, op] : kOps) {
      if (Cur().kind == Tok::Punct && Cur().text == s) {
        ExprP e = NewExpr(Expr::Unary, Cur().loc);
        p_++;
        e->op = op;
        e->args.push_back(Unary());
        return e;
      }
    }
    // (type) cast
    if (Is("(") && IsTypeName(Ahead(1))) {
      size_t save = p_;
      Loc loc = Cur().loc;
      p_++;
      Type t = ParseType();
      t = ArraySuffix(t);
      if (Accept(")")) {
        ExprP e = NewExpr(Expr::Cast, loc);
        e->spec = t;
        e->args.push_back(Unary());
        return e;
      }
      p_ = save;
    }
    return Postfix(Primary());
  }

  ExprP Postfix(ExprP e) {
    for (;;) {
      Loc loc = Cur().loc;
      if (Accept("[")) {
        ExprP i = NewExpr(Expr::Index, loc);
        i->args.push_back(std::move(e));
        i->args.push_back(Expression());
        Expect("]");
        e = std::move(i);
      } else if (Accept(".")) {
        ExprP m = NewExpr(Expr::Member, loc);
        m->name = Ident();
        m->args.push_back(std::move(e));
        e = std::move(m);
      } else if (Is("++") || Is("--")) {
        ExprP u = NewExpr(Expr::Unary, loc);
        u->op = Is("++") ? Op::PostInc : Op::PostDec;
        p_++;
        u->args.push_back(std::move(e));
        e = std::move(u);
      } else {
        return e;
      }
    }
  }

  void Arguments(Expr* e) {
    Expect("(");
    while (!Accept(")")) {
      e->args.push_back(Assignment());
      if (!Accept(",")) {
        Expect(")");
        break;
      }
    }
  }

  ExprP Primary() {
    const Token& t = Cur();
    Loc loc = t.loc;
    if (Accept("(")) {
      ExprP e = Expression();
      Expect(")");
      return e;
    }
    if (t.kind == Tok::Number) return Number();
    if (t.kind == Tok::String) {
      // Only for string globals (annotations, descriptions): never used.
      ExprP e = NewExpr(Expr::Literal, loc);
      e->spec = Type::Make(Base::String);
      p_++;
      return e;
    }
    if (t.kind == Tok::Ident && (t.text == "true" || t.text == "false")) {
      ExprP e = NewExpr(Expr::Literal, loc);
      e->spec = Type::Make(Base::Bool);
      e->ival = t.text == "true";
      p_++;
      return e;
    }
    if (IsTypeName(t) && t.text != "struct") {
      ExprP e = NewExpr(Expr::Construct, loc);
      e->spec = ParseType();
      Arguments(e.get());
      return e;
    }
    if (t.kind == Tok::Ident) {
      std::string n = Ident();
      if (Is("(")) {
        ExprP e = NewExpr(Expr::Call, loc);
        e->name = n;
        Arguments(e.get());
        return e;
      }
      ExprP e = NewExpr(Expr::Var, loc);
      e->name = n;
      return e;
    }
    Fail(loc, "unexpected " + Describe(t));
  }

  ExprP Number() {
    const Token& t = t_[p_++];
    ExprP e = NewExpr(Expr::Literal, t.loc);
    std::string s = t.text;
    bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    bool flt = !hex && (s.find('.') != std::string::npos || s.find('e') != std::string::npos ||
                        s.find('E') != std::string::npos);
    Base b = Base::Int;
    // Suffixes: f/F (float), h/H (half), l/L (long / double), u/U.
    while (!s.empty()) {
      char c = s.back();
      if (!hex && (c == 'f' || c == 'F')) {
        b = Base::Float;
        flt = true;
      } else if (c == 'h' || c == 'H') {
        b = Base::Half;
        flt = true;
      } else if (c == 'u' || c == 'U') {
        b = Base::Uint;
      } else if (c == 'l' || c == 'L') {
        if (flt) b = Base::Double;
      } else {
        break;
      }
      s.pop_back();
    }
    char* end = nullptr;
    if (flt) {
      if (b == Base::Int || b == Base::Uint) b = Base::Float;
      e->fval = std::strtod(s.c_str(), &end);
    } else {
      e->ival = int64_t(std::strtoull(s.c_str(), &end, 0));
    }
    if (*end) Fail(t.loc, "bad number " + t.text);
    e->spec = Type::Make(b);
    return e;
  }

  std::vector<Token> t_;
  size_t p_ = 0;
  Unit u_;
  std::map<std::string, StructDecl*> structs_;
  std::map<std::string, Type> typedefs_;
  uint32_t pack_ = 0;
};

}  // namespace

Unit Parse(std::vector<Token> tokens, uint32_t compile_flags) {
  return Parser(std::move(tokens), compile_flags).Run();
}

}  // namespace recomp::hlsl

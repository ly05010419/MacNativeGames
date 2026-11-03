#pragma once
// The HLSL translator's internals: tokens, types and the syntax tree that
// the parser builds, the checker annotates and the GLSL emitter walks.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/hlsl/hlsl.h"

namespace recomp::hlsl {

struct Loc {
  int line = 0, col = 0;
};

// Translation stops at the first error: Fail throws it to Compile.
struct Error {
  Loc loc;
  std::string msg;
};
[[noreturn]] void Fail(Loc loc, std::string msg);

// ---- Tokens (preprocess.cpp)

enum class Tok : uint8_t {
  End,
  Ident,
  Number,  // pp-number: classified by the parser
  String,
  Punct,
  Pragma,  // a #pragma line the parser may act on (text: its tokens)
};

struct Token {
  Tok kind = Tok::End;
  std::string text;
  Loc loc;
  bool space = false;  // whitespace before it (for # in macros)
};

std::vector<Token> Preprocess(std::string_view src, const std::vector<Define>& defines);

// ---- Types

// Numeric kinds first, in conversion rank order.
enum class Base : uint8_t {
  Void, Bool, Int, Uint, Half, Float, Double, String, Sampler, Texture, Struct,
};
enum class Dim : uint8_t { Any, D1, D2, D3, Cube };  // samplers, textures

struct StructDecl;

struct Type {
  Base base = Base::Void;
  bool matrix = false;
  uint8_t rows = 1, cols = 1;  // vector: rows 1; float1 is a scalar
  uint32_t array = 0;          // elements, 0 if not an array
  Dim dim = Dim::Any;
  const StructDecl* st = nullptr;

  bool Numeric() const { return base >= Base::Bool && base <= Base::Double; }
  bool Scalar() const { return Numeric() && !matrix && cols == 1 && !array; }
  bool Vector() const { return Numeric() && !matrix && cols > 1 && !array; }
  bool Matrix() const { return Numeric() && matrix && !array; }
  bool Floating() const { return base == Base::Half || base == Base::Float || base == Base::Double; }
  int Components() const { return rows * cols; }
  Type Elem() const {
    Type t = *this;
    t.array = 0;
    return t;
  }
  Type WithBase(Base b) const {
    Type t = *this;
    t.base = b;
    return t;
  }
  bool operator==(const Type& o) const {
    return base == o.base && matrix == o.matrix && rows == o.rows && cols == o.cols &&
           array == o.array && dim == o.dim && st == o.st;
  }
  static Type Make(Base b, int rows = 1, int cols = 1, bool matrix = false) {
    Type t;
    t.base = b;
    t.rows = uint8_t(rows);
    t.cols = uint8_t(cols);
    t.matrix = matrix;
    return t;
  }
  static Type Vec(Base b, int n) { return Make(b, 1, n); }
};

std::string TypeName(const Type& t);  // HLSL spelling, for messages

// ---- Syntax tree

struct VarDecl;
struct FuncDecl;
struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

enum class Op : uint8_t {
  None,
  // unary
  Neg, Plus, Not, BitNot, PreInc, PreDec, PostInc, PostDec,
  // binary
  Mul, Div, Mod, Add, Sub, Shl, Shr, Lt, Gt, Le, Ge, Eq, Ne,
  BitAnd, BitXor, BitOr, And, Or,
};
const char* OpText(Op op);

struct Expr {
  enum Kind : uint8_t {
    Literal,    // ival / fval, type set by the parser
    Var,        // name
    Unary,      // op args[0]
    Binary,     // args[0] op args[1]
    Assign,     // args[0] (op)= args[1]; op None for plain =
    Ternary,    // args[0] ? args[1] : args[2]
    Call,       // name(args): a function or an intrinsic
    Construct,  // spec(args)
    Cast,       // (spec)args[0]
    Member,     // args[0].name: struct field or swizzle
    Index,      // args[0][args[1]]
    Comma,      // args, left to right
    InitList,   // { args }
    Convert,    // inserted by the checker: args[0] converted to `type`
  };
  Kind kind = Literal;
  Loc loc;
  Op op = Op::None;
  std::string name;
  std::vector<ExprP> args;
  Type spec;
  double fval = 0;
  int64_t ival = 0;

  // Filled in by the checker.
  Type type;
  VarDecl* var = nullptr;    // Var
  FuncDecl* fn = nullptr;    // Call of a user function
  int intrinsic = -1;        // Call of an intrinsic
  int field = -1;            // Member: struct field, or -1 for a swizzle
  std::vector<uint8_t> swz;  // swizzle: vector components, or row * 4 + col
  bool lvalue = false;
};

struct Stmt {
  enum Kind : uint8_t {
    Block, Decl, Expression, If, For, While, Do, Return, Break, Continue, Discard, Empty,
  };
  Kind kind = Empty;
  Loc loc;
  std::vector<StmtP> body;                     // Block
  std::vector<std::unique_ptr<VarDecl>> vars;  // Decl
  ExprP cond, expr, step;  // If/For/While/Do: cond; Expression/Return: expr; For: step
  StmtP init, then, els;   // For: init; If: then/els; loops: then is the body
};

// Storage classes and modifiers (VarDecl::flags, FuncDecl params).
enum : uint32_t {
  kStatic = 0x1, kUniform = 0x2, kExtern = 0x4, kShared = 0x8, kConst = 0x10,
  kVolatile = 0x20, kIn = 0x40, kOut = 0x80, kRowMajor = 0x100, kColumnMajor = 0x200,
  kNoInterpolation = 0x400, kCentroid = 0x800,
};

struct VarDecl {
  std::string name;
  Loc loc;
  Type type;
  uint32_t flags = 0;
  std::string semantic;  // as written (upper-cased)
  char reg_set = 0;      // register(...) letter: 'c', 'i', 'b' or 's'; 0 if none
  uint32_t reg = 0;
  ExprP init;
  bool global = false, param = false;

  // Filled in by the checker.
  enum class Class : uint8_t { Local, Param, Uniform, Private, Const } cls = Class::Local;
  bool used = false, written = false, checked = false;
  std::string glsl;  // emitted name
  RegisterSet set = RegisterSet::Float4;
  uint32_t reg_index = 0, reg_count = 0;  // allocated registers (uniforms)
  Dim sampler_dim = Dim::Any;             // samplers: from declaration or use
};

struct StructDecl {
  std::string name;
  Loc loc;
  std::vector<std::unique_ptr<VarDecl>> fields;
  std::string glsl;
  bool used = false;
};

struct FuncDecl {
  std::string name;
  Loc loc;
  Type ret;
  std::string ret_semantic;
  std::vector<std::unique_ptr<VarDecl>> params;
  StmtP body;  // null for a prototype

  // Filled in by the checker.
  FuncDecl* def = nullptr;  // the definition this declaration resolves to
  enum class State : uint8_t { Unchecked, Checking, Checked } state = State::Unchecked;
  std::vector<FuncDecl*> callees;
  std::string glsl;
};

struct Unit {
  std::vector<std::unique_ptr<StructDecl>> structs;
  std::vector<std::unique_ptr<VarDecl>> globals;  // in declaration order
  std::vector<std::unique_ptr<FuncDecl>> funcs;   // in declaration order
};

Unit Parse(std::vector<Token> tokens, uint32_t compile_flags);

// ---- Checking and emission

struct Target {
  Stage stage = Stage::Vertex;
  uint32_t major = 3, minor = 0;
};

// Intrinsic functions (check.cpp), referenced by Expr::intrinsic.
enum Intrinsic : int {
  kAbs, kAcos, kAll, kAny, kAsin, kAtan, kAtan2, kCeil, kClamp, kClip, kCos, kCosh,
  kCross, kD3DColorToUbyte4, kDdx, kDdy, kDegrees, kDeterminant, kDistance, kDot, kExp,
  kExp2, kFaceforward, kFloor, kFmod, kFrac, kFrexp, kFwidth, kIsfinite, kIsinf, kIsnan,
  kLdexp, kLength, kLerp, kLit, kLog, kLog10, kLog2, kMax, kMin, kModf, kMul, kNormalize,
  kPow, kRadians, kReflect, kRefract, kRound, kRsqrt, kSaturate, kSign, kSin, kSincos,
  kSinh, kSmoothstep, kSqrt, kStep, kTan, kTanh, kTranspose, kTrunc,
  // Texture sampling: dimension x variant.
  kTex1D, kTex1Dbias, kTex1Dgrad, kTex1Dlod, kTex1Dproj,
  kTex2D, kTex2Dbias, kTex2Dgrad, kTex2Dlod, kTex2Dproj,
  kTex3D, kTex3Dbias, kTex3Dgrad, kTex3Dlod, kTex3Dproj,
  kTexCube, kTexCubebias, kTexCubegrad, kTexCubelod, kTexCubeproj,
  kIntrinsicCount,
};

// Resolves names and types, inserts conversions, finds what the entry
// point reaches. Returns the entry's definition.
FuncDecl* Check(Unit& unit, const std::string& entry, const Target& target);

// "TEXCOORD3" -> {kTexcoord, 3}; also VPOS, VFACE and the SV_ spellings.
bool ParseSemantic(const std::string& s, Semantic* out);

// Registers for the uniforms the entry point reads, and the constant
// table.
void AllocateRegisters(Unit& unit, const Target& target, std::vector<Constant>* table);

void EmitGlsl(Unit& unit, FuncDecl* entry, const Target& target, Program* out);

}  // namespace recomp::hlsl

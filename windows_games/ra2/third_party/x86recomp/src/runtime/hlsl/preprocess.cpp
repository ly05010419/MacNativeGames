// The C preprocessor as D3DX's HLSL compiler runs it: object-like and
// function-like macros (# and ##), #if/#ifdef/#ifndef/#elif/#else/#endif,
// #undef, #error, #line (ignored) and #pragma (passed to the parser).
// #include needs an include handler, which is not supported.

#include <cctype>
#include <cstdlib>
#include <deque>
#include <map>

#include "runtime/hlsl/ast.h"

namespace recomp::hlsl {

void Fail(Loc loc, std::string msg) { throw Error{loc, std::move(msg)}; }

namespace {

// ---- Lexing into logical lines

const char* const kPuncts[] = {
    "<<=", ">>=", "...", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=",
    "-=",  "*=",  "/=",  "%=", "&=", "|=", "^=", "->", "::", "##",
};

bool IdentStart(char c) { return std::isalpha(uint8_t(c)) || c == '_'; }
bool IdentChar(char c) { return std::isalnum(uint8_t(c)) || c == '_'; }

class Lexer {
 public:
  explicit Lexer(std::string_view s) : s_(s) {}

  // The next logical line's tokens; false at the end of the source.
  bool Line(std::vector<Token>* out) {
    out->clear();
    if (pos_ >= s_.size()) return false;
    bool space = false;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == '\n') {
        Newline();
        return true;
      }
      if (c == '\\' && (Peek(1) == '\n' || (Peek(1) == '\r' && Peek(2) == '\n'))) {
        pos_ += Peek(1) == '\r' ? 2 : 1;
        Newline();
        space = true;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
        Advance();
        space = true;
        continue;
      }
      if (c == '/' && Peek(1) == '/') {
        while (pos_ < s_.size() && s_[pos_] != '\n') Advance();
        continue;
      }
      if (c == '/' && Peek(1) == '*') {
        Loc start = Here();
        Advance(2);
        while (pos_ < s_.size() && !(s_[pos_] == '*' && Peek(1) == '/')) {
          if (s_[pos_] == '\n')
            Newline();
          else
            Advance();
        }
        if (pos_ >= s_.size()) Fail(start, "unterminated comment");
        Advance(2);
        space = true;
        continue;
      }
      Token t;
      t.loc = Here();
      t.space = space;
      space = false;
      size_t start = pos_;
      if (IdentStart(c)) {
        while (pos_ < s_.size() && IdentChar(s_[pos_])) Advance();
        t.kind = Tok::Ident;
      } else if (std::isdigit(uint8_t(c)) || (c == '.' && std::isdigit(uint8_t(Peek(1))))) {
        // pp-number
        Advance();
        while (pos_ < s_.size()) {
          char d = s_[pos_];
          if ((d == '+' || d == '-') &&
              (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E') && !IsHex(start)) {
            Advance();
          } else if (IdentChar(d) || d == '.') {
            Advance();
          } else {
            break;
          }
        }
        t.kind = Tok::Number;
      } else if (c == '"' || c == '\'') {
        Advance();
        while (pos_ < s_.size() && s_[pos_] != c && s_[pos_] != '\n') {
          if (s_[pos_] == '\\') Advance();
          Advance();
        }
        if (pos_ >= s_.size() || s_[pos_] != c) Fail(t.loc, "unterminated string");
        Advance();
        t.kind = Tok::String;
      } else {
        t.kind = Tok::Punct;
        size_t len = 1;
        for (const char* p : kPuncts) {
          size_t n = std::char_traits<char>::length(p);
          if (s_.compare(pos_, n, p) == 0) {
            len = n;
            break;
          }
        }
        Advance(int(len));
      }
      t.text = std::string(s_.substr(start, pos_ - start));
      out->push_back(std::move(t));
    }
    return true;
  }

 private:
  char Peek(size_t k) const { return pos_ + k < s_.size() ? s_[pos_ + k] : '\0'; }
  bool IsHex(size_t start) const {
    return s_[start] == '0' && start + 1 < s_.size() && (s_[start + 1] == 'x' || s_[start + 1] == 'X');
  }
  void Advance(int n = 1) {
    pos_ += n;
    col_ += n;
  }
  void Newline() {
    pos_++;
    line_++;
    col_ = 1;
  }
  Loc Here() const { return Loc{line_, col_}; }

  std::string_view s_;
  size_t pos_ = 0;
  int line_ = 1, col_ = 1;
};

// ---- Macros

struct Macro {
  bool function = false;
  std::vector<std::string> params;
  std::vector<Token> body;
};

// A token on its way through expansion, with the macros it came out of
// (its hide set: those are not expanded again in it).
struct PTok {
  Token t;
  std::vector<std::string> hide;
  bool Hidden(const std::string& n) const {
    for (const auto& h : hide)
      if (h == n) return true;
    return false;
  }
};

bool Same(const Token& a, const Token& b) { return a.kind == b.kind && a.text == b.text; }

class Preprocessor {
 public:
  explicit Preprocessor(const std::vector<Define>& defines) {
    for (const auto& d : defines) {
      Macro m;
      Lexer lx(d.value.empty() ? std::string_view("1") : std::string_view(d.value));
      std::vector<Token> line;
      while (lx.Line(&line)) m.body.insert(m.body.end(), line.begin(), line.end());
      macros_[d.name] = std::move(m);
    }
  }

  std::vector<Token> Run(std::string_view src) {
    Lexer lx(src);
    std::vector<Token> line;
    std::deque<PTok> text;  // non-directive lines not expanded yet
    while (lx.Line(&line)) {
      if (!line.empty() && line[0].kind == Tok::Punct && line[0].text == "#") {
        Flush(&text);
        Directive(line);
        continue;
      }
      if (!Active()) continue;
      for (auto& t : line) text.push_back(PTok{std::move(t), {}});
    }
    Flush(&text);
    if (!conds_.empty()) Fail(conds_.back().loc, "#if without #endif");
    return std::move(out_);
  }

 private:
  struct Cond {
    Loc loc;
    bool active;   // this branch's tokens are kept
    bool taken;    // some branch of this #if was taken
    bool in_else;
    bool outer;    // the enclosing region is active
  };

  bool Active() const { return conds_.empty() || conds_.back().active; }

  void Flush(std::deque<PTok>* text) {
    if (text->empty()) return;
    for (auto& p : Expand(std::move(*text))) out_.push_back(std::move(p.t));
    text->clear();
  }

  void Directive(const std::vector<Token>& line) {
    if (line.size() == 1) return;  // null directive
    const Token& d = line[1];
    const std::string& n = d.text;
    std::vector<Token> rest(line.begin() + 2, line.end());
    if (n == "if" || n == "ifdef" || n == "ifndef") {
      bool outer = Active();
      bool v = false;
      if (outer) {
        if (n == "if") {
          v = EvalIf(rest, d.loc);
        } else {
          if (rest.empty() || rest[0].kind != Tok::Ident) Fail(d.loc, "#" + n + " needs a name");
          v = macros_.count(rest[0].text) != 0;
          if (n == "ifndef") v = !v;
        }
      }
      conds_.push_back(Cond{d.loc, outer && v, v, false, outer});
      return;
    }
    if (n == "elif" || n == "else") {
      if (conds_.empty() || conds_.back().in_else) Fail(d.loc, "#" + n + " without #if");
      Cond& c = conds_.back();
      bool v = false;
      if (n == "else") {
        c.in_else = true;
        v = true;
      } else if (c.outer && !c.taken) {
        v = EvalIf(rest, d.loc);
      }
      c.active = c.outer && !c.taken && v;
      c.taken = c.taken || c.active;
      return;
    }
    if (n == "endif") {
      if (conds_.empty()) Fail(d.loc, "#endif without #if");
      conds_.pop_back();
      return;
    }
    if (!Active()) return;
    if (n == "define") {
      Define(rest, d.loc);
    } else if (n == "undef") {
      if (rest.empty() || rest[0].kind != Tok::Ident) Fail(d.loc, "#undef needs a name");
      macros_.erase(rest[0].text);
    } else if (n == "error") {
      std::string msg;
      for (const auto& t : rest) msg += (t.space && !msg.empty() ? " " : "") + t.text;
      Fail(d.loc, "#error " + msg);
    } else if (n == "pragma") {
      Token p;
      p.kind = Tok::Pragma;
      p.loc = d.loc;
      for (const auto& t : rest) p.text += t.text;
      out_.push_back(std::move(p));
    } else if (n == "line") {
      // Line numbers in messages stay those of the source as given.
    } else if (n == "include") {
      Fail(d.loc, "#include is not supported (no include handler)");
    } else {
      Fail(d.loc, "unknown directive #" + n);
    }
  }

  void Define(const std::vector<Token>& rest, Loc loc) {
    if (rest.empty() || rest[0].kind != Tok::Ident) Fail(loc, "#define needs a name");
    Macro m;
    size_t i = 1;
    if (rest.size() > 1 && rest[1].text == "(" && !rest[1].space) {
      m.function = true;
      i = 2;
      while (i < rest.size() && rest[i].text != ")") {
        if (rest[i].kind != Tok::Ident) Fail(rest[i].loc, "bad macro parameter list");
        m.params.push_back(rest[i].text);
        i++;
        if (i < rest.size() && rest[i].text == ",") i++;
      }
      if (i >= rest.size()) Fail(loc, "unterminated macro parameter list");
      i++;
    }
    m.body.assign(rest.begin() + i, rest.end());
    macros_[rest[0].text] = std::move(m);
  }

  int ParamIndex(const Macro& m, const Token& t) const {
    if (t.kind != Tok::Ident) return -1;
    for (size_t i = 0; i < m.params.size(); i++)
      if (m.params[i] == t.text) return int(i);
    return -1;
  }

  // Macro expansion with hide sets (Prosser's algorithm, simplified: a
  // function-like expansion's hide set is the name token's plus the macro).
  std::vector<PTok> Expand(std::deque<PTok> in) {
    std::vector<PTok> out;
    while (!in.empty()) {
      PTok t = std::move(in.front());
      in.pop_front();
      if (t.t.kind != Tok::Ident || t.Hidden(t.t.text)) {
        out.push_back(std::move(t));
        continue;
      }
      if (t.t.text == "__LINE__") {
        t.t.kind = Tok::Number;
        t.t.text = std::to_string(t.t.loc.line);
        out.push_back(std::move(t));
        continue;
      }
      auto it = macros_.find(t.t.text);
      if (it == macros_.end()) {
        out.push_back(std::move(t));
        continue;
      }
      const Macro& m = it->second;
      std::vector<std::vector<PTok>> args;
      if (m.function) {
        if (in.empty() || in.front().t.text != "(") {
          out.push_back(std::move(t));  // the name alone is not a call
          continue;
        }
        in.pop_front();
        args.emplace_back();
        int depth = 0;
        for (;;) {
          if (in.empty()) Fail(t.t.loc, "unterminated call of macro " + t.t.text);
          PTok a = std::move(in.front());
          in.pop_front();
          if (a.t.text == ")" && depth == 0) break;
          if (a.t.text == "(") depth++;
          if (a.t.text == ")") depth--;
          if (a.t.text == "," && depth == 0) {
            args.emplace_back();
            continue;
          }
          args.back().push_back(std::move(a));
        }
        if (m.params.empty() && args.size() == 1 && args[0].empty()) args.clear();
        if (args.size() != m.params.size())
          Fail(t.t.loc, "macro " + t.t.text + " takes " + std::to_string(m.params.size()) +
                            " arguments, " + std::to_string(args.size()) + " given");
      }
      std::vector<std::string> hide = t.hide;
      hide.push_back(t.t.text);
      std::vector<PTok> repl = Substitute(m, args, hide, t.t);
      for (auto r = repl.rbegin(); r != repl.rend(); ++r) in.push_front(std::move(*r));
    }
    return out;
  }

  std::vector<PTok> Substitute(const Macro& m, const std::vector<std::vector<PTok>>& args,
                               const std::vector<std::string>& hide, const Token& at) {
    std::vector<PTok> out;
    const auto& b = m.body;
    for (size_t i = 0; i < b.size(); i++) {
      bool paste_before = i > 0 && b[i - 1].text == "##";
      bool paste_after = i + 1 < b.size() && b[i + 1].text == "##";
      if (b[i].text == "##") continue;
      if (m.function && b[i].text == "#" && i + 1 < b.size() && ParamIndex(m, b[i + 1]) >= 0) {
        std::string s = "\"";
        for (const auto& a : args[ParamIndex(m, b[i + 1])]) {
          if (a.t.space && s.size() > 1) s += ' ';
          for (char c : a.t.text) {
            if (c == '"' || c == '\\') s += '\\';
            s += c;
          }
        }
        s += '"';
        Token t{Tok::String, s, at.loc, b[i].space};
        out.push_back(PTok{t, hide});
        i++;
        continue;
      }
      std::vector<PTok> piece;
      int p = m.function ? ParamIndex(m, b[i]) : -1;
      if (p >= 0) {
        if (paste_before || paste_after) {
          piece = args[p];
        } else {
          piece = Expand(std::deque<PTok>(args[p].begin(), args[p].end()));
        }
        for (auto& x : piece) x.t.loc = at.loc;
        if (!piece.empty()) piece[0].t.space = b[i].space;
      } else {
        Token t = b[i];
        t.loc = at.loc;
        piece.push_back(PTok{t, {}});
      }
      if (paste_before && !out.empty() && !piece.empty()) {
        // Glue the previous token and this piece's first into one.
        std::string glued = out.back().t.text + piece[0].t.text;
        Lexer lx(glued);
        std::vector<Token> toks;
        lx.Line(&toks);
        if (toks.size() != 1) Fail(at.loc, "## does not form a single token: " + glued);
        out.back().t.text = toks[0].text;
        out.back().t.kind = toks[0].kind;
        piece.erase(piece.begin());
      }
      for (auto& x : piece) {
        for (const auto& h : hide)
          if (!x.Hidden(h)) x.hide.push_back(h);
        out.push_back(std::move(x));
      }
    }
    if (!out.empty()) out[0].t.space = at.space;
    return out;
  }

  // ---- #if expressions

  bool EvalIf(const std::vector<Token>& toks, Loc loc) {
    std::deque<PTok> in;
    for (size_t i = 0; i < toks.size(); i++) {
      if (toks[i].kind == Tok::Ident && toks[i].text == "defined") {
        size_t j = i + 1;
        bool paren = j < toks.size() && toks[j].text == "(";
        if (paren) j++;
        if (j >= toks.size() || toks[j].kind != Tok::Ident) Fail(toks[i].loc, "defined needs a name");
        Token t{Tok::Number, macros_.count(toks[j].text) ? "1" : "0", toks[i].loc, true};
        if (paren) {
          j++;
          if (j >= toks.size() || toks[j].text != ")") Fail(toks[i].loc, "defined( without )");
        }
        in.push_back(PTok{t, {}});
        i = j;
        continue;
      }
      in.push_back(PTok{toks[i], {}});
    }
    std::vector<PTok> ex = Expand(std::move(in));
    expr_.clear();
    for (auto& p : ex) expr_.push_back(std::move(p.t));
    if (expr_.empty()) Fail(loc, "#if with no expression");
    epos_ = 0;
    int64_t v = Ternary();
    if (epos_ != expr_.size()) Fail(expr_[epos_].loc, "unexpected '" + expr_[epos_].text + "' in #if");
    return v != 0;
  }

  const Token* Cur() const { return epos_ < expr_.size() ? &expr_[epos_] : nullptr; }
  bool Accept(const char* s) {
    if (Cur() && Cur()->kind == Tok::Punct && Cur()->text == s) {
      epos_++;
      return true;
    }
    return false;
  }

  int64_t Ternary() {
    int64_t c = Binary(0);
    if (Accept("?")) {
      int64_t a = Ternary();
      if (!Accept(":")) Fail(Cur() ? Cur()->loc : Loc{}, "expected : in #if");
      int64_t b = Ternary();
      return c ? a : b;
    }
    return c;
  }

  int64_t Binary(int level) {
    static const std::vector<std::vector<const char*>> kLevels = {
        {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">="},
        {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
    };
    if (level == int(kLevels.size())) return UnaryE();
    int64_t a = Binary(level + 1);
    for (;;) {
      const char* op = nullptr;
      for (const char* o : kLevels[level])
        if (Accept(o)) op = o;
      if (!op) return a;
      int64_t b = Binary(level + 1);
      std::string s = op;
      Loc l = Cur() ? Cur()->loc : Loc{};
      if (s == "||") a = a || b;
      else if (s == "&&") a = a && b;
      else if (s == "|") a |= b;
      else if (s == "^") a ^= b;
      else if (s == "&") a &= b;
      else if (s == "==") a = a == b;
      else if (s == "!=") a = a != b;
      else if (s == "<") a = a < b;
      else if (s == ">") a = a > b;
      else if (s == "<=") a = a <= b;
      else if (s == ">=") a = a >= b;
      else if (s == "<<") a <<= b;
      else if (s == ">>") a >>= b;
      else if (s == "+") a += b;
      else if (s == "-") a -= b;
      else if (s == "*") a *= b;
      else {
        if (b == 0) Fail(l, "division by zero in #if");
        a = s == "/" ? a / b : a % b;
      }
    }
  }

  int64_t UnaryE() {
    if (Accept("-")) return -UnaryE();
    if (Accept("+")) return UnaryE();
    if (Accept("!")) return !UnaryE();
    if (Accept("~")) return ~UnaryE();
    if (Accept("(")) {
      int64_t v = Ternary();
      if (!Accept(")")) Fail(Cur() ? Cur()->loc : Loc{}, "expected ) in #if");
      return v;
    }
    const Token* t = Cur();
    if (!t) Fail(Loc{}, "unexpected end of #if expression");
    epos_++;
    if (t->kind == Tok::Ident) return t->text == "true" ? 1 : 0;
    if (t->kind != Tok::Number) Fail(t->loc, "unexpected '" + t->text + "' in #if");
    std::string s = t->text;
    while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' || s.back() == 'L'))
      s.pop_back();
    char* end = nullptr;
    int64_t v = std::strtoll(s.c_str(), &end, 0);
    if (*end) Fail(t->loc, "bad number in #if: " + t->text);
    return v;
  }

  std::map<std::string, Macro> macros_;
  std::vector<Cond> conds_;
  std::vector<Token> out_;
  std::vector<Token> expr_;
  size_t epos_ = 0;
};

}  // namespace

std::vector<Token> Preprocess(std::string_view src, const std::vector<Define>& defines) {
  return Preprocessor(defines).Run(src);
}

}  // namespace recomp::hlsl

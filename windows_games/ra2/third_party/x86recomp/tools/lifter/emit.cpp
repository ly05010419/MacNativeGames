#include "emit.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>

namespace lifter {

namespace {
// Set while emitting a function whose body is wrapped in try/catch (SEH
// frames, setjmp callers): a guaranteed tail call cannot leave a try
// block.
thread_local bool t_in_try = false;
// Tail calls between lifted functions must not grow the host stack (a
// loop that crosses function entries would otherwise recurse per
// iteration).
const char* TailAttr() { return t_in_try ? "" : "LIFT_MUSTTAIL "; }

__attribute__((format(printf, 1, 2))) std::string Fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

// printf's %llX for a VA (uint64_t is unsigned long on LP64 Linux).
unsigned long long U(uint64_t v) { return v; }

std::string Hex(uint64_t v) { return Fmt("0x%llXu", U(v)); }

const char* Gpr32Name(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_EAX: return "eax";
    case ZYDIS_REGISTER_ECX: return "ecx";
    case ZYDIS_REGISTER_EDX: return "edx";
    case ZYDIS_REGISTER_EBX: return "ebx";
    case ZYDIS_REGISTER_ESP: return "esp";
    case ZYDIS_REGISTER_EBP: return "ebp";
    case ZYDIS_REGISTER_ESI: return "esi";
    case ZYDIS_REGISTER_EDI: return "edi";
    default: return nullptr;
  }
}

// x64 general-purpose registers, by their 64-bit name (lifted code keeps
// all of them in uint64_t locals).
const char* Gpr64Name(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_RAX: return "rax";
    case ZYDIS_REGISTER_RCX: return "rcx";
    case ZYDIS_REGISTER_RDX: return "rdx";
    case ZYDIS_REGISTER_RBX: return "rbx";
    case ZYDIS_REGISTER_RSP: return "rsp";
    case ZYDIS_REGISTER_RBP: return "rbp";
    case ZYDIS_REGISTER_RSI: return "rsi";
    case ZYDIS_REGISTER_RDI: return "rdi";
    case ZYDIS_REGISTER_R8: return "r8";
    case ZYDIS_REGISTER_R9: return "r9";
    case ZYDIS_REGISTER_R10: return "r10";
    case ZYDIS_REGISTER_R11: return "r11";
    case ZYDIS_REGISTER_R12: return "r12";
    case ZYDIS_REGISTER_R13: return "r13";
    case ZYDIS_REGISTER_R14: return "r14";
    case ZYDIS_REGISTER_R15: return "r15";
    default: return nullptr;
  }
}

bool IsHi8(ZydisRegister r) {
  return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH ||
         r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

// Segment register reads return the selectors a Win32 process sees.
const char* SegValue(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_CS: return "0x1Bu";
    case ZYDIS_REGISTER_DS:
    case ZYDIS_REGISTER_ES:
    case ZYDIS_REGISTER_SS: return "0x23u";
    case ZYDIS_REGISTER_FS: return "0x3Bu";
    case ZYDIS_REGISTER_GS: return "0x0u";
    default: return nullptr;
  }
}

// The selectors an x64 Windows process sees.
const char* SegValue64(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_CS: return "0x33u";
    case ZYDIS_REGISTER_DS:
    case ZYDIS_REGISTER_ES:
    case ZYDIS_REGISTER_SS:
    case ZYDIS_REGISTER_GS: return "0x2Bu";
    case ZYDIS_REGISTER_FS: return "0x53u";
    default: return nullptr;
  }
}

int StIndex(ZydisRegister r) {
  if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7)
    return r - ZYDIS_REGISTER_ST0;
  return -1;
}

const char* CondExpr(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO:
      return "F.of";
    case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO:
    case ZYDIS_MNEMONIC_CMOVNO:
      return "!F.of";
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB:
    case ZYDIS_MNEMONIC_FCMOVB:
      return "F.cf";
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_FCMOVNB:
      return "!F.cf";
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ:
    case ZYDIS_MNEMONIC_FCMOVE:
      return "F.zf";
    case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ:
    case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_FCMOVNE:
      return "!F.zf";
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE:
    case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_FCMOVBE:
      return "(F.cf || F.zf)";
    case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_FCMOVNBE:
      return "!(F.cf || F.zf)";
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS:
      return "F.sf";
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS:
    case ZYDIS_MNEMONIC_CMOVNS:
      return "!F.sf";
    case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP:
    case ZYDIS_MNEMONIC_FCMOVU:
      return "F.pf";
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_FCMOVNU:
      return "!F.pf";
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL:
      return "(F.sf != F.of)";
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL:
    case ZYDIS_MNEMONIC_CMOVNL:
      return "(F.sf == F.of)";
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE:
    case ZYDIS_MNEMONIC_CMOVLE:
      return "(F.zf || F.sf != F.of)";
    case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE:
    case ZYDIS_MNEMONIC_CMOVNLE:
      return "(!F.zf && F.sf == F.of)";
    default:
      return nullptr;
  }
}

const char* MemType(int bytes) {
  switch (bytes) {
    case 1: return "uint8_t";
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    case 8: return "uint64_t";
    default: return nullptr;
  }
}

const char* X87OpName(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP:
    case ZYDIS_MNEMONIC_FIADD:
      return "Add";
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP:
    case ZYDIS_MNEMONIC_FIMUL:
      return "Mul";
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP:
    case ZYDIS_MNEMONIC_FISUB:
      return "Sub";
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP:
    case ZYDIS_MNEMONIC_FISUBR:
      return "SubR";
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP:
    case ZYDIS_MNEMONIC_FIDIV:
      return "Div";
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP:
    case ZYDIS_MNEMONIC_FIDIVR:
      return "DivR";
    default:
      return nullptr;
  }
}

// SSE/SSE2 instructions, lifted onto runtime/sse.h. Their MMX-register
// forms share the ISA sets and are rejected in EmitSse.
bool IsSse(const ZydisDecodedInstruction& d) {
  switch (d.meta.isa_set) {
    case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2:
    case ZYDIS_ISA_SET_SSEMXCSR: case ZYDIS_ISA_SET_SSE_PREFETCH:
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSE4:
      return true;
    default:
      return false;
  }
}

bool IsFcmov(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE:
    case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVNB:
    case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE:
    case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU:
      return true;
    default:
      return false;
  }
}

// Zydis files FCMOVcc under ZYDIS_CATEGORY_CMOV with the integer CMOVcc.
bool IsX87(const ZydisDecodedInstruction& d) {
  return d.meta.category == ZYDIS_CATEGORY_X87_ALU || d.mnemonic == ZYDIS_MNEMONIC_FWAIT ||
         IsFcmov(d.mnemonic);
}

// Arithmetic that pops (FADDP...) and integer-operand arithmetic (FIADD...).
bool X87Pops(ZydisMnemonic m) {
  return m == ZYDIS_MNEMONIC_FADDP || m == ZYDIS_MNEMONIC_FMULP ||
         m == ZYDIS_MNEMONIC_FSUBP || m == ZYDIS_MNEMONIC_FSUBRP ||
         m == ZYDIS_MNEMONIC_FDIVP || m == ZYDIS_MNEMONIC_FDIVRP;
}
bool X87Integer(ZydisMnemonic m) {
  return m == ZYDIS_MNEMONIC_FIADD || m == ZYDIS_MNEMONIC_FIMUL ||
         m == ZYDIS_MNEMONIC_FISUB || m == ZYDIS_MNEMONIC_FISUBR ||
         m == ZYDIS_MNEMONIC_FIDIV || m == ZYDIS_MNEMONIC_FIDIVR;
}

// The x87::Const of FLD1, FLDZ, FLDPI, ...
const char* X87ConstName(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_FLD1: return "One";
    case ZYDIS_MNEMONIC_FLDZ: return "Zero";
    case ZYDIS_MNEMONIC_FLDPI: return "Pi";
    case ZYDIS_MNEMONIC_FLDL2E: return "L2E";
    case ZYDIS_MNEMONIC_FLDL2T: return "L2T";
    case ZYDIS_MNEMONIC_FLDLG2: return "LG2";
    case ZYDIS_MNEMONIC_FLDLN2: return "LN2";
    default: return nullptr;
  }
}

// ---- x87 runs ---------------------------------------------------------------
//
// A run is a straight-line stretch of a basic block whose x87 instructions
// cannot fault given a guard checked once at its start (see "x87 runs" in
// ARCHITECTURE.md). Inside it the register stack lives in locals x<k>, one
// per entry-relative slot k (st(i) at entry is slot i; pushes go to -1, -2,
// ...), the stack positions are resolved here, and the state is written
// back at the end. X87Sim tracks the slots symbolically; the same pass
// sizes the run (dry) and emits it. Under the control word 0x027F a run
// first tries the same instructions on host doubles d<k> (x87.h, "host
// doubles"); see EmitX87Run.
class X87Sim {
 public:
  enum class Slot : uint8_t { Untouched, Valid, Empty };

  explicit X87Sim(char prefix = 'x') : prefix_(prefix) {}

  bool ok = true;
  int off = 0;               // st(i) is slot off + i
  uint32_t need_valid = 0;   // entry-relative (k & 7) masks for the guard
  uint32_t need_empty = 0;
  bool c1_written = false, cc_written = false;
  std::map<int, Slot> slots;
  std::set<int> loaded;      // slots read from the entry state
  std::set<int> written;     // slots assigned in the run
  std::set<int> touched;     // slots whose tag may change
  int lo = 0, hi = 0;        // slot range seen (at most 8 physical regs)

  std::string Var(int k) const { return Fmt("%c%d", prefix_, k + 16); }

  // st(i) as an operand that must hold a value.
  std::string Read(int i) {
    int k = off + i;
    Range(k);
    Slot& s = slots[k];
    if (s == Slot::Empty) ok = false;  // stack underflow: not a run
    if (s == Slot::Untouched) {
      need_valid |= 1u << (k & 7);
      loaded.insert(k);
      s = Slot::Valid;
    }
    return Var(k);
  }
  // st(i) as a destination.
  std::string Write(int i) {
    int k = off + i;
    Range(k);
    slots[k] = Slot::Valid;
    written.insert(k);
    touched.insert(k);
    return Var(k);
  }
  // A push: the new st0.
  std::string Push() {
    --off;
    int k = off;
    Range(k);
    Slot& s = slots[k];
    if (s == Slot::Valid) ok = false;  // stack overflow
    if (s == Slot::Untouched) need_empty |= 1u << (k & 7);
    s = Slot::Valid;
    written.insert(k);
    touched.insert(k);
    return Var(k);
  }
  void Pop() {
    slots[off] = Slot::Empty;
    touched.insert(off);
    ++off;
  }
  // The run's sticky status word so far: entry flags plus rfl, with C1 and
  // C3/C2/C0 from the run's locals once it has written them.
  std::string StatusExpr() const {
    std::string sw = "c.fpu.sw | rfl";
    if (c1_written) sw = Fmt("((%s) & ~0x200u) | (rc1 ? 0x200u : 0u)", sw.c_str());
    if (cc_written) sw = Fmt("((%s) & ~0x4500u) | rcc", sw.c_str());
    return sw;
  }

 private:
  void Range(int k) {
    lo = std::min(lo, k);
    hi = std::max(hi, k);
    if (hi - lo > 7) ok = false;  // two slots on one physical register
  }
  char prefix_;
};

// Emits the code for one instruction. Each instruction gets its own { }
// scope so temporaries never clash.
class InsnEmitter {
 public:
  InsnEmitter(const Program& p, const Function& f, const Insn& in,
              std::string* out, std::set<uint64_t>* refs, EmitStats* stats)
      : p_(p), f_(f), in_(in), d_(in.d), ops_(in.ops), out_(*out),
        refs_(*refs), stats_(*stats) {}

  void Emit();
  // The instruction inside an x87 run (see X87Sim); false if it cannot be
  // part of one. Non-x87 instructions are emitted as usual.
  bool EmitInRun(X87Sim& sim);
  // The same on the run's host doubles: `fail` is the statement that
  // leaves for the exact path when a value cannot be held or an operation
  // cannot be done exactly in doubles; *can_fail is set if it was emitted.
  // False if the instruction has no double form.
  bool EmitInRunDouble(X87Sim& sim, const std::string& fail, bool* can_fail);

 private:
  bool EmitX87Fast(X87Sim& sim);
  bool EmitX87Double(X87Sim& sim, const std::string& fail, bool* can_fail);

  // ---- output -----------------------------------------------------------
  void L(const std::string& s) { out_ += "    " + s + "\n"; }
  void Unimpl() {
    stats_.unimplemented[ZydisMnemonicGetString(d_.mnemonic)]++;
    L(Fmt("LIFT_SPILL; ::recomp::Unimplemented(c, %s, \"%s\");",
          Hex(in_.va).c_str(), ZydisMnemonicGetString(d_.mnemonic)));
  }

  // ---- operands -----------------------------------------------------------
  // x64 images: registers are uint64_t locals rax..r15, addresses 64-bit.
  bool X64() const { return p_.x64; }
  // The C++ type holding an operand of `bytes` (see lift::W<N>::T).
  static const char* VT(int bytes) { return bytes == 8 ? "uint64_t" : "uint32_t"; }
  int Nvis() const { return d_.operand_count_visible; }
  const ZydisDecodedOperand& Op(int i) const { return ops_[i]; }
  int Bytes(int i) const { return Op(i).size / 8; }
  bool IsReg(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_REGISTER; }
  bool IsMem(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_MEMORY; }
  bool IsImm(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_IMMEDIATE; }
  // x87: st(i) index of register operand i; the memory operand's index
  // (-1 if none) and a read of it.
  int St(int i) const { return StIndex(Op(i).reg.value); }
  int MemIndex() const {
    int mem = -1;
    for (int i = 0; i < Nvis(); i++)
      if (IsMem(i)) mem = i;
    return mem;
  }
  std::string RdMem(int mem, int bytes) const {
    return Fmt("::recomp::lift::Rd<%s>(B, m%d)", MemType(bytes), mem);
  }
  // The accumulator (al/ax/eax/rax) for an operand size.
  static ZydisRegister Acc(int bytes) {
    return bytes == 8   ? ZYDIS_REGISTER_RAX
           : bytes == 4 ? ZYDIS_REGISTER_EAX
           : bytes == 2 ? ZYDIS_REGISTER_AX
                        : ZYDIS_REGISTER_AL;
  }

  // Effective address of a memory operand (segment base included, except
  // for LEA, which never adds one).
  std::string AddrExpr(int i, bool segment = true) const {
    if (X64()) return AddrExpr64(i, segment);
    const auto& m = Op(i).mem;
    std::string s;
    auto add = [&](const std::string& t) {
      if (!s.empty()) s += " + ";
      s += t;
    };
    if (segment && m.segment == ZYDIS_REGISTER_FS) add("FS");
    if (m.base != ZYDIS_REGISTER_NONE) add(Gpr32Name(m.base));
    if (m.index != ZYDIS_REGISTER_NONE)
      add(m.scale > 1 ? Fmt("%s * %du", Gpr32Name(m.index), m.scale)
                      : std::string(Gpr32Name(m.index)));
    if (m.disp.has_displacement || s.empty())
      add(Hex(static_cast<uint32_t>(m.disp.value)));
    return "uint32_t(" + s + ")";
  }

  // x64: RIP-relative operands are absolute at lift time; gs is the TEB.
  // With an address-size prefix the sum wraps at 32 bits.
  std::string AddrExpr64(int i, bool segment) const {
    const auto& m = Op(i).mem;
    if (m.base == ZYDIS_REGISTER_RIP)
      return Hex(in_.next() + uint64_t(m.disp.value));
    std::string s;
    auto add = [&](const std::string& t) {
      if (!s.empty()) s += " + ";
      s += t;
    };
    if (m.base != ZYDIS_REGISTER_NONE) add(ReadReg(m.base));
    if (m.index != ZYDIS_REGISTER_NONE)
      add(m.scale > 1 ? Fmt("%s * %du", ReadReg(m.index).c_str(), m.scale)
                      : ReadReg(m.index));
    if (s.empty()) {
      s = Hex(uint64_t(m.disp.value));
    } else if (m.disp.value < 0) {
      s += " - " + Hex(uint64_t(-m.disp.value));
    } else if (m.disp.value > 0) {
      s += " + " + Hex(uint64_t(m.disp.value));
    }
    s = d_.address_width == 32 ? "uint64_t(uint32_t(" + s + "))" : "uint64_t(" + s + ")";
    if (segment && m.segment == ZYDIS_REGISTER_GS) s = "GS + " + s;
    return s;
  }

  bool AddrOk(int i) const {
    const auto& m = Op(i).mem;
    if (X64()) {
      auto gpr = [](ZydisRegister r) {
        return r == ZYDIS_REGISTER_NONE ||
               Gpr64Name(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r));
      };
      if (m.base != ZYDIS_REGISTER_RIP && !gpr(m.base)) return false;
      if (!gpr(m.index)) return false;
      return m.segment == ZYDIS_REGISTER_NONE || m.segment == ZYDIS_REGISTER_DS ||
             m.segment == ZYDIS_REGISTER_ES || m.segment == ZYDIS_REGISTER_SS ||
             m.segment == ZYDIS_REGISTER_CS || m.segment == ZYDIS_REGISTER_GS;
    }
    if (m.base != ZYDIS_REGISTER_NONE && !Gpr32Name(m.base)) return false;
    if (m.index != ZYDIS_REGISTER_NONE && !Gpr32Name(m.index)) return false;
    return m.segment == ZYDIS_REGISTER_NONE || m.segment == ZYDIS_REGISTER_DS ||
           m.segment == ZYDIS_REGISTER_ES || m.segment == ZYDIS_REGISTER_SS ||
           m.segment == ZYDIS_REGISTER_CS || m.segment == ZYDIS_REGISTER_FS;
  }

  // Declares m<i> = address for every explicit memory operand.
  bool PrepareMem() {
    for (int i = 0; i < Nvis(); i++) {
      if (!IsMem(i)) continue;
      if (!AddrOk(i)) return false;
      L(Fmt("const %s m%d = %s;", X64() ? "uint64_t" : "uint32_t", i, AddrExpr(i).c_str()));
    }
    return true;
  }

  std::string ReadReg(ZydisRegister r) const {
    if (X64()) return ReadReg64(r);
    if (const char* n = Gpr32Name(r)) return n;
    if (const char* sv = SegValue(r)) return sv;
    ZydisRegister big =
        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const char* n = Gpr32Name(big);
    if (!n) return "/*bad reg*/0u";
    int w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    if (w == 16) return Fmt("(%s & 0xFFFFu)", n);
    if (IsHi8(r)) return Fmt("((%s >> 8) & 0xFFu)", n);
    return Fmt("(%s & 0xFFu)", n);
  }

  // x64: reads of 8/16/32-bit registers are uint32_t, of 64-bit ones
  // uint64_t; 32-bit writes zero-extend, 8/16-bit ones merge.
  std::string ReadReg64(ZydisRegister r) const {
    if (const char* sv = SegValue64(r)) return sv;
    const char* n =
        Gpr64Name(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r));
    if (!n) return "/*bad reg*/0u";
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r)) {
      case 64: return n;
      case 32: return Fmt("uint32_t(%s)", n);
      case 16: return Fmt("(uint32_t(%s) & 0xFFFFu)", n);
      default:
        return IsHi8(r) ? Fmt("((uint32_t(%s) >> 8) & 0xFFu)", n)
                        : Fmt("(uint32_t(%s) & 0xFFu)", n);
    }
  }
  std::string WriteReg64(ZydisRegister r, const std::string& v) const {
    if (SegValue64(r)) return "/* segment register write ignored */";
    const char* n =
        Gpr64Name(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r));
    if (!n) return "/*bad reg*/;";
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r)) {
      case 64: return Fmt("%s = uint64_t(%s);", n, v.c_str());
      case 32: return Fmt("%s = uint64_t(uint32_t(%s));", n, v.c_str());
      case 16:
        return Fmt("%s = ::recomp::lift::SetLo16(%s, uint64_t(%s));", n, n, v.c_str());
      default:
        return Fmt("%s = ::recomp::lift::Set%s8(%s, uint64_t(%s));", n,
                   IsHi8(r) ? "Hi" : "Lo", n, v.c_str());
    }
  }

  std::string WriteReg(ZydisRegister r, const std::string& v) const {
    if (X64()) return WriteReg64(r, v);
    if (const char* n = Gpr32Name(r)) return Fmt("%s = uint32_t(%s);", n, v.c_str());
    if (SegValue(r)) return "/* segment register write ignored */";
    ZydisRegister big =
        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const char* n = Gpr32Name(big);
    int w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    if (w == 16)
      return Fmt("%s = ::recomp::lift::SetLo16(%s, %s);", n, n, v.c_str());
    if (IsHi8(r))
      return Fmt("%s = ::recomp::lift::SetHi8(%s, %s);", n, n, v.c_str());
    return Fmt("%s = ::recomp::lift::SetLo8(%s, %s);", n, n, v.c_str());
  }

  // Value of operand i, zero-extended to uint32_t, masked to `bytes`.
  std::string Read(int i, int bytes = 0) const {
    if (!bytes) bytes = Bytes(i);
    const auto& o = Op(i);
    switch (o.type) {
      case ZYDIS_OPERAND_TYPE_REGISTER:
        return ReadReg(o.reg.value);
      case ZYDIS_OPERAND_TYPE_MEMORY:
        if (bytes == 8 && X64()) return Fmt("::recomp::lift::Rd<uint64_t>(B, m%d)", i);
        return Fmt("uint32_t(::recomp::lift::Rd<%s>(B, m%d))", MemType(bytes), i);
      case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        uint64_t v = o.imm.value.u;
        if (bytes == 8 && X64()) return Hex(v);  // sign-extended by Zydis
        if (bytes == 1) v &= 0xFF;
        if (bytes == 2) v &= 0xFFFF;
        return Hex(static_cast<uint32_t>(v));
      }
      default:
        return "/*bad operand*/0u";
    }
  }

  std::string Write(int i, const std::string& v, int bytes = 0) const {
    if (!bytes) bytes = Bytes(i);
    const auto& o = Op(i);
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) return WriteReg(o.reg.value, v);
    return Fmt("::recomp::lift::Wr<%s>(B, m%d, %s(%s));", MemType(bytes), i,
               MemType(bytes), v.c_str());
  }

  // ---- control flow -------------------------------------------------------
  std::string TailCall(uint64_t t) {
    refs_.insert(t);
    return Fmt("{ LIFT_SPILL; %sreturn %s(c); }", TailAttr(), FnName(t).c_str());
  }
  std::string GotoOrTail(uint64_t t) {
    if (IsOtherEntry(p_, f_, t)) return TailCall(t);
    if (!f_.insns.count(t))  // target outside .text: never decoded
      return Fmt("{ LIFT_SPILL; ::recomp::BadJump(c, %s, %s); }",
                 Hex(in_.va).c_str(), Hex(t).c_str());
    return Fmt("goto L_%08llX;", U(t));
  }
  void Push32(const std::string& v) {
    L(Fmt("esp -= 4; ::recomp::lift::Wr<uint32_t>(B, esp, %s);", v.c_str()));
  }
  void Push64(const std::string& v) {
    L(Fmt("rsp -= 8; ::recomp::lift::Wr<uint64_t>(B, rsp, uint64_t(%s));", v.c_str()));
  }

  // ---- SSE -----------------------------------------------------------------
  static int XmmIndex(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15 ? r - ZYDIS_REGISTER_XMM0
                                                                 : -1;
  }
  bool IsXmm(int i) const { return IsReg(i) && XmmIndex(Op(i).reg.value) >= 0; }
  std::string XmmRef(int i) const {
    return Fmt("c.xmm[%d]", XmmIndex(Op(i).reg.value));
  }
  // Operand i as an Xmm: the register itself, or its memory loaded into
  // s<i> (zero-extended). Legacy-SSE m128 operands must be aligned unless
  // the instruction is an unaligned move.
  std::string XmmSrc(int i, bool aligned) {
    if (IsXmm(i)) return XmmRef(i);
    if (aligned && Bytes(i) == 16)
      L(Fmt("::recomp::sse::CheckAligned(c, m%d, %s);", i, Hex(in_.va).c_str()));
    L(Fmt("const ::recomp::Xmm s%d = ::recomp::sse::Ld(B, m%d, %d);", i, i, Bytes(i)));
    return Fmt("s%d", i);
  }

  void EmitAlu();
  void EmitShift();
  enum StringKind { Movs, Stos, Lods, Scas, Cmps };
  void EmitString();
  void EmitString64(int n, StringKind kind);
  void EmitX87();
  void EmitSse();
  void EmitMmx();
  bool IsMmxInsn() const;
  // ---- MMX: MMn is the significand of x87 physical register Rn ----
  bool IsMm(int i) const {
    return IsReg(i) && ZydisRegisterGetClass(Op(i).reg.value) == ZYDIS_REGCLASS_MMX;
  }
  int MmIndex(int i) const { return Op(i).reg.value - ZYDIS_REGISTER_MM0; }
  void EmitAvx();
  // ---- AVX: VEX operands by 128-bit half (h = 0: c.xmm, 1: c.ymm_hi) ----
  static int VecIndex(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) return r - ZYDIS_REGISTER_XMM0;
    if (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM15) return r - ZYDIS_REGISTER_YMM0;
    return -1;
  }
  bool IsVec(int i) const { return IsReg(i) && VecIndex(Op(i).reg.value) >= 0; }
  std::string Half(int i, int h) const {
    return Fmt("c.%s[%d]", h ? "ymm_hi" : "xmm", VecIndex(Op(i).reg.value));
  }
  // Half h of operand i as a value: the register, or `bytes` of memory
  // from half h's offset loaded into a local.
  std::string HalfSrc(int i, int h, int bytes = 16) {
    if (IsVec(i)) return Half(i, h);
    L(Fmt("const ::recomp::Xmm s%d_%d = ::recomp::sse::Ld(B, m%d + %du, %d);", i, h, i,
          16 * h, bytes));
    return Fmt("s%d_%d", i, h);
  }
  void ZeroUpper(int i) { L(Half(i, 1) + " = ::recomp::Xmm{};"); }
  void EmitLocked();
  void Emit64();

  const Program& p_;
  const Function& f_;
  const Insn& in_;
  const ZydisDecodedInstruction& d_;
  const ZydisDecodedOperand* ops_;
  std::string& out_;
  std::set<uint64_t>& refs_;
  EmitStats& stats_;
};

void InsnEmitter::EmitAlu() {
  int n = Bytes(0);
  std::string a = Read(0), b = Nvis() > 1 ? Read(1, n) : "";
  const char* fn = nullptr;
  bool store = true;
  std::string r;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_ADD: fn = "Add"; break;
    case ZYDIS_MNEMONIC_ADC: fn = "Adc"; break;
    case ZYDIS_MNEMONIC_SUB: fn = "Sub"; break;
    case ZYDIS_MNEMONIC_SBB: fn = "Sbb"; break;
    case ZYDIS_MNEMONIC_CMP: fn = "Sub"; store = false; break;
    case ZYDIS_MNEMONIC_AND:
      r = Fmt("::recomp::lift::Logic<%d>(F, %s & %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_OR:
      r = Fmt("::recomp::lift::Logic<%d>(F, %s | %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_XOR:
      r = Fmt("::recomp::lift::Logic<%d>(F, %s ^ %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_TEST:
      r = Fmt("::recomp::lift::Logic<%d>(F, %s & %s)", n, a.c_str(), b.c_str());
      store = false;
      break;
    case ZYDIS_MNEMONIC_INC: r = Fmt("::recomp::lift::Inc<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_DEC: r = Fmt("::recomp::lift::Dec<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_NEG: r = Fmt("::recomp::lift::Neg<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_NOT: r = Fmt("(~%s)", a.c_str()); break;
    default: break;
  }
  if (fn) r = Fmt("::recomp::lift::%s<%d>(F, %s, %s)", fn, n, a.c_str(), b.c_str());
  if (store) {
    L(std::string("const ") + VT(n) + " r = " + r + ";");
    L(Write(0, "r"));
  } else {
    L("(void)" + r + ";");
  }
}

void InsnEmitter::EmitShift() {
  int n = Bytes(0);
  std::string cnt = Nvis() > 1 ? Read(1, 1) : "1u";
  const char* fn = nullptr;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_SHL: fn = "Shl"; break;
    case ZYDIS_MNEMONIC_SHR: fn = "Shr"; break;
    case ZYDIS_MNEMONIC_SAR: fn = "Sar"; break;
    case ZYDIS_MNEMONIC_ROL: fn = "Rol"; break;
    case ZYDIS_MNEMONIC_ROR: fn = "Ror"; break;
    case ZYDIS_MNEMONIC_RCL: fn = "Rcl"; break;
    case ZYDIS_MNEMONIC_RCR: fn = "Rcr"; break;
    default: break;
  }
  L(Fmt("const %s r = ::recomp::lift::%s<%d>(F, %s, %s);", VT(n), fn, n,
        Read(0).c_str(), cnt.c_str()));
  L(Write(0, "r"));
}

void InsnEmitter::EmitString() {
  int n = 0;
  StringKind kind = Movs;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: n = 1; kind = Movs; break;
    case ZYDIS_MNEMONIC_MOVSW: n = 2; kind = Movs; break;
    case ZYDIS_MNEMONIC_MOVSD: n = 4; kind = Movs; break;
    case ZYDIS_MNEMONIC_STOSB: n = 1; kind = Stos; break;
    case ZYDIS_MNEMONIC_STOSW: n = 2; kind = Stos; break;
    case ZYDIS_MNEMONIC_STOSD: n = 4; kind = Stos; break;
    case ZYDIS_MNEMONIC_LODSB: n = 1; kind = Lods; break;
    case ZYDIS_MNEMONIC_LODSW: n = 2; kind = Lods; break;
    case ZYDIS_MNEMONIC_LODSD: n = 4; kind = Lods; break;
    case ZYDIS_MNEMONIC_SCASB: n = 1; kind = Scas; break;
    case ZYDIS_MNEMONIC_SCASW: n = 2; kind = Scas; break;
    case ZYDIS_MNEMONIC_SCASD: n = 4; kind = Scas; break;
    case ZYDIS_MNEMONIC_CMPSB: n = 1; kind = Cmps; break;
    case ZYDIS_MNEMONIC_CMPSW: n = 2; kind = Cmps; break;
    case ZYDIS_MNEMONIC_CMPSD: n = 4; kind = Cmps; break;
    case ZYDIS_MNEMONIC_MOVSQ: n = 8; kind = Movs; break;
    case ZYDIS_MNEMONIC_STOSQ: n = 8; kind = Stos; break;
    case ZYDIS_MNEMONIC_LODSQ: n = 8; kind = Lods; break;
    case ZYDIS_MNEMONIC_SCASQ: n = 8; kind = Scas; break;
    case ZYDIS_MNEMONIC_CMPSQ: n = 8; kind = Cmps; break;
    default: break;
  }
  if (!n) return Unimpl();
  if (X64()) return EmitString64(n, kind);
  const char* t = MemType(n);
  std::string acc = ReadReg(Acc(n));
  std::string step = Fmt("const uint32_t d = F.df ? uint32_t(-%d) : %du;", n, n);
  std::string body;
  // esi is DS:esi, edi is ES:edi; both flat. An fs: override on the source
  // (never seen in this binary) is not supported.
  switch (kind) {
    case Movs:
      body = Fmt("::recomp::lift::Wr<%s>(B, edi, ::recomp::lift::Rd<%s>(B, esi)); "
                 "esi += d; edi += d;", t, t);
      break;
    case Stos:
      body = Fmt("::recomp::lift::Wr<%s>(B, edi, %s(%s)); edi += d;", t, t,
                 acc.c_str());
      break;
    case Lods:
      body = Fmt("{ const uint32_t v = ::recomp::lift::Rd<%s>(B, esi); %s } "
                 "esi += d;", t, WriteReg(Acc(n), "v").c_str());
      break;
    case Scas:
      body = Fmt("(void)::recomp::lift::Sub<%d>(F, %s, ::recomp::lift::Rd<%s>(B, edi)); "
                 "edi += d;", n, acc.c_str(), t);
      break;
    case Cmps:
      body = Fmt("(void)::recomp::lift::Sub<%d>(F, ::recomp::lift::Rd<%s>(B, esi), "
                 "::recomp::lift::Rd<%s>(B, edi)); esi += d; edi += d;", n, t, t);
      break;
  }

  bool rep = d_.attributes & ZYDIS_ATTRIB_HAS_REP;
  bool repe = d_.attributes & ZYDIS_ATTRIB_HAS_REPE;
  bool repne = d_.attributes & ZYDIS_ATTRIB_HAS_REPNE;
  L(step);
  if (kind == Scas || kind == Cmps) {
    if (repe)
      L("while (ecx) { " + body + " ecx--; if (!F.zf) break; }");
    else if (repne)
      L("while (ecx) { " + body + " ecx--; if (F.zf) break; }");
    else
      L(body);
  } else if (rep || repe || repne) {
    L("while (ecx) { " + body + " ecx--; }");
  } else {
    L(body);
  }
}

// x64 string instructions: rsi, rdi and rcx (64-bit addressing; the
// 32-bit form with an address-size prefix is not supported).
void InsnEmitter::EmitString64(int n, StringKind kind) {
  if (d_.address_width != 64) return Unimpl();
  const char* t = MemType(n);
  std::string acc = ReadReg(Acc(n));
  std::string body;
  switch (kind) {
    case Movs:
      body = Fmt("::recomp::lift::Wr<%s>(B, rdi, ::recomp::lift::Rd<%s>(B, rsi)); "
                 "rsi += d; rdi += d;", t, t);
      break;
    case Stos:
      body = Fmt("::recomp::lift::Wr<%s>(B, rdi, %s(%s)); rdi += d;", t, t, acc.c_str());
      break;
    case Lods:
      body = Fmt("{ const %s v = ::recomp::lift::Rd<%s>(B, rsi); %s } rsi += d;", VT(n), t,
                 WriteReg(Acc(n), "v").c_str());
      break;
    case Scas:
      body = Fmt("(void)::recomp::lift::Sub<%d>(F, %s, ::recomp::lift::Rd<%s>(B, rdi)); "
                 "rdi += d;", n, acc.c_str(), t);
      break;
    case Cmps:
      body = Fmt("(void)::recomp::lift::Sub<%d>(F, ::recomp::lift::Rd<%s>(B, rsi), "
                 "::recomp::lift::Rd<%s>(B, rdi)); rsi += d; rdi += d;", n, t, t);
      break;
  }
  bool rep = d_.attributes & ZYDIS_ATTRIB_HAS_REP;
  bool repe = d_.attributes & ZYDIS_ATTRIB_HAS_REPE;
  bool repne = d_.attributes & ZYDIS_ATTRIB_HAS_REPNE;
  L(Fmt("const uint64_t d = F.df ? uint64_t(-%d) : %du;", n, n));
  if (kind == Scas || kind == Cmps) {
    if (repe)
      L("while (rcx) { " + body + " rcx--; if (!F.zf) break; }");
    else if (repne)
      L("while (rcx) { " + body + " rcx--; if (F.zf) break; }");
    else
      L(body);
  } else if (rep || repe || repne) {
    L("while (rcx) { " + body + " rcx--; }");
  } else {
    L(body);
  }
}

// lock-prefixed read-modify-write on memory: compare-and-swap loop so the
// update is atomic on any host (and ordered: seq_cst, see ARCHITECTURE.md
// memory-ordering notes).
void InsnEmitter::EmitLocked() {
  int n = Bytes(0);
  const char* t = MemType(n);
  std::string src = Nvis() > 1 ? Read(1, n) : "0u";
  std::string calc;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_XADD: calc = Fmt("::recomp::lift::Add<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_ADC: calc = Fmt("::recomp::lift::Adc<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_SUB: calc = Fmt("::recomp::lift::Sub<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_SBB: calc = Fmt("::recomp::lift::Sbb<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_AND: calc = Fmt("::recomp::lift::Logic<%d>(G, a & b)", n); break;
    case ZYDIS_MNEMONIC_OR: calc = Fmt("::recomp::lift::Logic<%d>(G, a | b)", n); break;
    case ZYDIS_MNEMONIC_XOR: calc = Fmt("::recomp::lift::Logic<%d>(G, a ^ b)", n); break;
    case ZYDIS_MNEMONIC_INC: calc = Fmt("::recomp::lift::Inc<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_DEC: calc = Fmt("::recomp::lift::Dec<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_NEG: calc = Fmt("::recomp::lift::Neg<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_NOT: calc = Fmt("(%s)(~a)", t); break;
    case ZYDIS_MNEMONIC_CMPXCHG: {
      // One compare-exchange: on a match it stores the source, otherwise
      // the accumulator gets the old value; flags as cmp acc, old.
      L(Fmt("%s* p = ::recomp::lift::AtomicPtr<%s>(B, m0);", t, t));
      L(Fmt("%s a = %s(%s);", t, t, ReadReg(Acc(n)).c_str()));
      L(Fmt("const %s e = a;", t));
      L(Fmt("const bool ok = __atomic_compare_exchange_n(p, &a, %s(%s), false, "
            "__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);", t, src.c_str()));
      L(Fmt("(void)::recomp::lift::Sub<%d>(F, e, a);", n));
      L("if (!ok) { " + WriteReg(Acc(n), "a") + " }");
      return;
    }
    default:
      Unimpl();
      return;
  }
  L(Fmt("%s* p = ::recomp::lift::AtomicPtr<%s>(B, m0);", t, t));
  L(Fmt("const %s b = %s;", VT(n), src.c_str()));
  L(Fmt("%s a = __atomic_load_n(p, __ATOMIC_SEQ_CST);", t));
  L(Fmt("%s r; ::recomp::Flags G;", t));
  L(Fmt("do { G = F; r = %s(%s); } while (!__atomic_compare_exchange_n("
        "p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));",
        t, calc.c_str()));
  L("F = G;");
  if (d_.mnemonic == ZYDIS_MNEMONIC_XADD) L(Write(1, "a"));
}

void InsnEmitter::EmitX87() {
  const auto m = d_.mnemonic;
  auto pop = [&]() { L("::recomp::x87::Pop(c.fpu);"); };
  const int mem = MemIndex();
  auto rd_mem = [&](int bytes) { return RdMem(mem, bytes); };

  if (const char* op = X87OpName(m)) {
    std::string o = Fmt("::recomp::x87::Op::%s", op);
    if (mem >= 0) {
      int b = Bytes(mem);
      if (X87Integer(m))
        L(Fmt("::recomp::x87::ArithI%d(c.fpu, %s, int%d_t(%s));", b * 8,
              o.c_str(), b * 8, rd_mem(b).c_str()));
      else
        L(Fmt("::recomp::x87::ArithF%d(c.fpu, %s, %s);", b * 8, o.c_str(),
              rd_mem(b).c_str()));
      return;
    }
    int dst = Nvis() >= 2 ? St(0) : 1;
    int src = Nvis() >= 2 ? St(1) : 0;
    if (dst == 0)
      L(Fmt("::recomp::x87::ArithSt0Sti(c.fpu, %s, %d);", o.c_str(), src));
    else
      L(Fmt("::recomp::x87::ArithStiSt0(c.fpu, %s, %d);", o.c_str(), dst));
    if (X87Pops(m)) pop();
    return;
  }

  if (const char* k = X87ConstName(m)) {
    L(Fmt("::recomp::x87::LoadConst(c.fpu, ::recomp::x87::Const::%s);", k));
    return;
  }

  switch (m) {
    case ZYDIS_MNEMONIC_FLD:
      if (mem < 0) { L(Fmt("::recomp::x87::LoadSt(c.fpu, %d);", St(0))); return; }
      switch (Bytes(mem)) {
        case 4: L(Fmt("::recomp::x87::LoadF32(c.fpu, %s);", rd_mem(4).c_str())); return;
        case 8: L(Fmt("::recomp::x87::LoadF64(c.fpu, %s);", rd_mem(8).c_str())); return;
        case 10: L(Fmt("::recomp::x87::LoadF80(c.fpu, ::recomp::lift::RdF80(B, m%d));", mem)); return;
      }
      break;
    case ZYDIS_MNEMONIC_FILD:
      L(Fmt("::recomp::x87::LoadI%d(c.fpu, int%d_t(%s));", Bytes(mem) * 8,
            Bytes(mem) * 8, rd_mem(Bytes(mem)).c_str()));
      return;
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP:
      if (mem < 0) {
        L(Fmt("::recomp::x87::StoreSt(c.fpu, %d);", St(0)));
      } else {
        switch (Bytes(mem)) {
          case 4: L(Fmt("::recomp::lift::Wr<uint32_t>(B, m%d, ::recomp::x87::StoreF32(c.fpu));", mem)); break;
          case 8: L(Fmt("::recomp::lift::Wr<uint64_t>(B, m%d, ::recomp::x87::StoreF64(c.fpu));", mem)); break;
          case 10: L(Fmt("::recomp::lift::WrF80(B, m%d, ::recomp::x87::StoreF80(c.fpu));", mem)); break;
          default: Unimpl(); return;
        }
      }
      if (m == ZYDIS_MNEMONIC_FSTP) pop();
      return;
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP: {
      int b = Bytes(mem);
      L(Fmt("::recomp::lift::Wr<uint%d_t>(B, m%d, uint%d_t(::recomp::x87::StoreI%d(c.fpu)));",
            b * 8, mem, b * 8, b * 8));
      if (m == ZYDIS_MNEMONIC_FISTP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FXCH:
      L(Fmt("::recomp::x87::Xch(c.fpu, %d);", Nvis() ? St(Nvis() - 1) : 1));
      return;
    case ZYDIS_MNEMONIC_FCOM:
    case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOM:
    case ZYDIS_MNEMONIC_FUCOMP: {
      bool quiet = m == ZYDIS_MNEMONIC_FUCOM || m == ZYDIS_MNEMONIC_FUCOMP;
      if (mem >= 0) {
        if (Bytes(mem) == 4) L(Fmt("::recomp::x87::CompareF32(c.fpu, %s);", rd_mem(4).c_str()));
        else L(Fmt("::recomp::x87::CompareF64(c.fpu, %s);", rd_mem(8).c_str()));
      } else {
        // Forms: "fcom" (st1), "fcom St(i)", "fcom st0, St(i)".
        int i = Nvis() ? St(Nvis() - 1) : 1;
        L(Fmt("::recomp::x87::CompareSt(c.fpu, %d, %s);", i, quiet ? "true" : "false"));
      }
      if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOMPP:
      L(Fmt("::recomp::x87::CompareSt(c.fpu, 1, %s);",
            m == ZYDIS_MNEMONIC_FUCOMPP ? "true" : "false"));
      pop();
      pop();
      return;
    case ZYDIS_MNEMONIC_FICOM:
    case ZYDIS_MNEMONIC_FICOMP:
      L(Fmt("::recomp::x87::CompareI%d(c.fpu, int%d_t(%s));", Bytes(mem) * 8,
            Bytes(mem) * 8, rd_mem(Bytes(mem)).c_str()));
      if (m == ZYDIS_MNEMONIC_FICOMP) pop();
      return;
    case ZYDIS_MNEMONIC_FCOMI:
    case ZYDIS_MNEMONIC_FCOMIP:
    case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP: {
      bool quiet = m == ZYDIS_MNEMONIC_FUCOMI || m == ZYDIS_MNEMONIC_FUCOMIP;
      L(Fmt("const auto r = ::recomp::x87::CompareStEFlags(c.fpu, %d, %s);",
            St(Nvis() - 1), quiet ? "true" : "false"));
      L("F.zf = r.zf; F.pf = r.pf; F.cf = r.cf; F.of = F.sf = F.af = false;");
      if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FTST: L("::recomp::x87::Test(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FXAM: L("::recomp::x87::Examine(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FCHS: L("::recomp::x87::Chs(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FABS: L("::recomp::x87::Abs(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSQRT: L("::recomp::x87::Sqrt(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSIN: L("::recomp::x87::Sin(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FCOS: L("::recomp::x87::Cos(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSINCOS: L("::recomp::x87::SinCos(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPTAN: L("::recomp::x87::Ptan(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPATAN: L("::recomp::x87::Patan(c.fpu);"); return;
    case ZYDIS_MNEMONIC_F2XM1: L("::recomp::x87::F2xm1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FYL2X: L("::recomp::x87::Yl2x(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FYL2XP1: L("::recomp::x87::Yl2xp1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSCALE: L("::recomp::x87::Scale(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPREM: L("::recomp::x87::Prem(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPREM1: L("::recomp::x87::Prem1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FRNDINT: L("::recomp::x87::RndInt(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FXTRACT: L("::recomp::x87::Xtract(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNSTSW:
      if (mem >= 0)
        L(Fmt("::recomp::lift::Wr<uint16_t>(B, m%d, ::recomp::x87::StatusWord(c.fpu));", mem));
      else
        L("eax = ::recomp::lift::SetLo16(eax, ::recomp::x87::StatusWord(c.fpu));");
      return;
    case ZYDIS_MNEMONIC_FNSTCW:
      L(Fmt("::recomp::lift::Wr<uint16_t>(B, m%d, ::recomp::x87::ControlWord(c.fpu));", mem));
      return;
    case ZYDIS_MNEMONIC_FLDCW:
      L(Fmt("::recomp::x87::SetControlWord(c.fpu, %s);", rd_mem(2).c_str()));
      return;
    case ZYDIS_MNEMONIC_FNCLEX: L("::recomp::x87::ClearExceptions(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNINIT: L("::recomp::x87::Init(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FFREE: L(Fmt("::recomp::x87::Free(c.fpu, %d);", St(0))); return;
    case ZYDIS_MNEMONIC_FDECSTP: L("::recomp::x87::DecStp(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FINCSTP: L("::recomp::x87::IncStp(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FWAIT:
      return;
    case ZYDIS_MNEMONIC_FNSTENV:
      L(Fmt("::recomp::x87::StoreEnv(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FLDENV:
      L(Fmt("::recomp::x87::LoadEnv(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FNSAVE:
      L(Fmt("::recomp::x87::Save(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FRSTOR:
      L(Fmt("::recomp::x87::Restore(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE:
    case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVNB:
    case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE:
    case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU:
      L(Fmt("if (%s) ::recomp::x87::CopySt(c.fpu, %d);", CondExpr(m),
            St(Nvis() - 1)));
      return;
    default:
      break;
  }
  Unimpl();
}

bool InsnEmitter::EmitInRun(X87Sim& sim) {
  if (!IsX87(d_)) {
    Emit();
    return true;
  }
  if (!PrepareMem()) return false;
  return EmitX87Fast(sim);
}

bool InsnEmitter::EmitInRunDouble(X87Sim& sim, const std::string& fail, bool* can_fail) {
  *can_fail = false;
  if (!IsX87(d_)) {
    Emit();
    return true;
  }
  if (!PrepareMem()) return false;
  return EmitX87Double(sim, fail, can_fail);
}

// Fast forms: the same operations as EmitX87 on the run's locals, through
// the value-level recomp::x87::run functions. C1 and C3/C2/C0 follow the
// State-based operations (see x87.h): loads, register copies, fxch, fchs
// and fabs clear C1; arithmetic, sqrt and stores set it to "rounded up";
// compares set C3/C2/C0 and clear C1.
bool InsnEmitter::EmitX87Fast(X87Sim& sim) {
  const auto m = d_.mnemonic;
  const int mem = MemIndex();
  auto rd_mem = [&](int bytes) { return RdMem(mem, bytes); };
  const char* R = "::recomp::x87::run";
  auto c1 = [&](const std::string& v) {
    L("rc1 = " + v + ";");
    sim.c1_written = true;
  };
  // A register result r: value into dst, flags, C1.
  auto take = [&](const std::string& dst) {
    L(Fmt("%s = %s::Val(r); rfl |= %s::Flags(r);", dst.c_str(), R, R));
    c1(Fmt("%s::Up(r)", R));
  };
  auto stored = [&]() {
    L(Fmt("rfl |= %s::Flags(r);", R));
    c1(Fmt("%s::Up(r)", R));
  };

  if (const char* op = X87OpName(m)) {
    std::string o = Fmt("::recomp::x87::Op::%s", op);
    if (mem >= 0) {
      int b = Bytes(mem);
      std::string x = sim.Read(0);
      if (X87Integer(m)) {
        if (b != 2 && b != 4) return false;
        L(Fmt("const auto r = %s::ArithI%d(rcw, %s, %s, int%d_t(%s));", R, b * 8,
              o.c_str(), x.c_str(), b * 8, rd_mem(b).c_str()));
      } else {
        if (b != 4 && b != 8) return false;
        L(Fmt("const auto r = %s::ArithF%dFast(rcw, %s, %s, %s);", R, b * 8, o.c_str(),
              x.c_str(), rd_mem(b).c_str()));
      }
      sim.Write(0);
      take(x);
      return sim.ok;
    }
    int dst = Nvis() >= 2 ? St(0) : 1;
    int src = Nvis() >= 2 ? St(1) : 0;
    std::string a = sim.Read(dst), b = sim.Read(src);
    L(Fmt("const auto r = %s::ArithFast(rcw, %s, %s, %s);", R, o.c_str(), a.c_str(), b.c_str()));
    sim.Write(dst);
    take(a);
    if (X87Pops(m)) sim.Pop();
    return sim.ok;
  }

  switch (m) {
    case ZYDIS_MNEMONIC_FLD: {
      if (mem < 0) {
        std::string src = sim.Read(St(0));
        std::string dst = sim.Push();
        L(dst + " = " + src + ";");
        c1("false");
        return sim.ok;
      }
      int b = Bytes(mem);
      if (b == 10) {
        std::string dst = sim.Push();
        L(Fmt("%s = %s::ToV(::recomp::lift::RdF80(B, m%d));", dst.c_str(), R, mem));
      } else if (b == 4 || b == 8) {
        L(Fmt("const auto r = %s::LoadF%dFast(%s);", R, b * 8, rd_mem(b).c_str()));
        std::string dst = sim.Push();
        L(Fmt("%s = %s::Val(r); rfl |= %s::Flags(r);", dst.c_str(), R, R));
      } else {
        return false;
      }
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FILD: {
      int b = Bytes(mem);
      if (b != 2 && b != 4 && b != 8) return false;
      std::string dst = sim.Push();
      L(Fmt("%s = %s::FromInt(int%d_t(%s));", dst.c_str(), R, b * 8, rd_mem(b).c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FLD1: case ZYDIS_MNEMONIC_FLDZ: case ZYDIS_MNEMONIC_FLDPI:
    case ZYDIS_MNEMONIC_FLDL2E: case ZYDIS_MNEMONIC_FLDL2T: case ZYDIS_MNEMONIC_FLDLG2:
    case ZYDIS_MNEMONIC_FLDLN2: {
      std::string dst = sim.Push();
      L(Fmt("%s = %s::ConstValue(rcw, ::recomp::x87::Const::%s);", dst.c_str(), R,
            X87ConstName(m)));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP: {
      std::string x = sim.Read(0);
      if (mem < 0) {
        std::string dst = sim.Write(St(0));
        L(dst + " = " + x + ";");
        c1("false");
      } else {
        switch (Bytes(mem)) {
          case 4:
          case 8:
            L(Fmt("const auto r = %s::StoreF%dFast(rcw, %s);", R, Bytes(mem) * 8, x.c_str()));
            L(Fmt("::recomp::lift::Wr<%s>(B, m%d, %s(r.bits));", MemType(Bytes(mem)), mem,
                  MemType(Bytes(mem))));
            stored();
            break;
          case 10:
            L(Fmt("::recomp::lift::WrF80(B, m%d, %s::ToF80(%s));", mem, R, x.c_str()));
            c1("false");
            break;
          default:
            return false;
        }
      }
      if (m == ZYDIS_MNEMONIC_FSTP) sim.Pop();
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP: {
      int b = Bytes(mem);
      if (b != 2 && b != 4 && b != 8) return false;
      std::string x = sim.Read(0);
      L(Fmt("const auto r = %s::StoreI%d(rcw, %s);", R, b * 8, x.c_str()));
      L(Fmt("::recomp::lift::Wr<uint%d_t>(B, m%d, uint%d_t(r.bits));", b * 8, mem, b * 8));
      stored();
      if (m == ZYDIS_MNEMONIC_FISTP) sim.Pop();
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FXCH: {
      int i = Nvis() ? St(Nvis() - 1) : 1;
      std::string a = sim.Read(0), b = sim.Read(i);
      sim.Write(0);
      sim.Write(i);
      if (i != 0)
        L(Fmt("const %s::V t = %s; %s = %s; %s = t;", R, a.c_str(), a.c_str(),
              b.c_str(), b.c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FCHS:
    case ZYDIS_MNEMONIC_FABS: {
      std::string x = sim.Read(0);
      sim.Write(0);
      L(x + (m == ZYDIS_MNEMONIC_FCHS ? ".se ^= 0x8000;" : ".se &= 0x7FFF;"));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FSQRT: {
      std::string x = sim.Read(0);
      L(Fmt("const auto r = %s::Sqrt(rcw, %s);", R, x.c_str()));
      sim.Write(0);
      take(x);
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP:
    case ZYDIS_MNEMONIC_FTST: {
      bool quiet = m == ZYDIS_MNEMONIC_FUCOM || m == ZYDIS_MNEMONIC_FUCOMP ||
                   m == ZYDIS_MNEMONIC_FUCOMPP;
      std::string a = sim.Read(0);
      if (m == ZYDIS_MNEMONIC_FTST) {
        L(Fmt("const uint32_t r = %s::CompareFast(%s, %s::V{0, 0}, false);", R, a.c_str(), R));
      } else if (mem >= 0) {
        int b = Bytes(mem);
        if (b != 4 && b != 8) return false;
        L(Fmt("const uint32_t r = %s::CompareF%dFast(%s, %s);", R, b * 8, a.c_str(),
              rd_mem(b).c_str()));
      } else {
        int i = (m == ZYDIS_MNEMONIC_FCOMPP || m == ZYDIS_MNEMONIC_FUCOMPP)
                    ? 1 : (Nvis() ? St(Nvis() - 1) : 1);
        std::string b = sim.Read(i);
        L(Fmt("const uint32_t r = %s::CompareFast(%s, %s, %s);", R, a.c_str(), b.c_str(),
              quiet ? "true" : "false"));
      }
      L("rcc = r & 0x4500u; rfl |= r >> 16;");
      sim.cc_written = true;
      c1("false");
      if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP) sim.Pop();
      if (m == ZYDIS_MNEMONIC_FCOMPP || m == ZYDIS_MNEMONIC_FUCOMPP) {
        sim.Pop();
        sim.Pop();
      }
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FNSTSW: {
      std::string v = Fmt("%s::StatusWordOf(%s, (rtop + %du) & 7u, rcw)", R,
                          sim.StatusExpr().c_str(), unsigned(sim.off & 7));
      if (mem >= 0)
        L(Fmt("::recomp::lift::Wr<uint16_t>(B, m%d, %s);", mem, v.c_str()));
      else
        L(Fmt("eax = ::recomp::lift::SetLo16(eax, %s);", v.c_str()));
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FWAIT:
      return sim.ok;
    default:
      return false;
  }
}

// Double forms: the operations of EmitX87Fast on the run's doubles,
// through the recomp::x87::run "host doubles" functions, with the same C1
// and C3/C2/C0 updates. Every check comes before the instruction's side
// effects, so `fail` leaves with the instruction not yet done.
bool InsnEmitter::EmitX87Double(X87Sim& sim, const std::string& fail, bool* can_fail) {
  const auto m = d_.mnemonic;
  const int mem = MemIndex();
  auto rd_mem = [&](int bytes) { return RdMem(mem, bytes); };
  const char* R = "::recomp::x87::run";
  auto c1 = [&](const std::string& v) {
    L("rc1 = " + v + ";");
    sim.c1_written = true;
  };
  auto check = [&](const std::string& cond) {
    L(Fmt("if (!%s) [[unlikely]] %s", cond.c_str(), fail.c_str()));
    *can_fail = true;
  };
  // An arithmetic result into x, with PE and C1.
  auto arith = [&](const std::string& o, const std::string& x, const std::string& src) {
    check(Fmt("%s::ArithD(%s, %s, %s, &%s, &rfl, &rc1)", R, o.c_str(), x.c_str(), src.c_str(),
              x.c_str()));
    sim.c1_written = true;
  };

  if (const char* op = X87OpName(m)) {
    std::string o = Fmt("::recomp::x87::Op::%s", op);
    if (mem >= 0) {
      int b = Bytes(mem);
      std::string x = sim.Read(0);
      if (X87Integer(m)) {
        if (b != 2 && b != 4) return false;
        L(Fmt("const double v = double(int%d_t(%s));", b * 8, rd_mem(b).c_str()));
      } else {
        if (b != 4 && b != 8) return false;
        L("double v;");
        check(Fmt("%s::OperandF%d(%s, &v)", R, b * 8, rd_mem(b).c_str()));
      }
      arith(o, x, "v");
      sim.Write(0);
      return sim.ok;
    }
    int dst = Nvis() >= 2 ? St(0) : 1;
    int src = Nvis() >= 2 ? St(1) : 0;
    std::string a = sim.Read(dst), b = sim.Read(src);
    arith(o, a, b);
    sim.Write(dst);
    if (X87Pops(m)) sim.Pop();
    return sim.ok;
  }

  switch (m) {
    case ZYDIS_MNEMONIC_FLD: {
      if (mem < 0) {
        std::string src = sim.Read(St(0));
        std::string dst = sim.Push();
        L(dst + " = " + src + ";");
        c1("false");
        return sim.ok;
      }
      int b = Bytes(mem);
      if (b == 10) {
        std::string dst = sim.Push();
        check(Fmt("%s::ToD(%s::ToV(::recomp::lift::RdF80(B, m%d)), &%s)", R, R, mem,
                  dst.c_str()));
      } else if (b == 4 || b == 8) {
        std::string dst = sim.Push();
        check(Fmt("%s::OperandF%d(%s, &%s)", R, b * 8, rd_mem(b).c_str(), dst.c_str()));
      } else {
        return false;
      }
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FILD: {
      int b = Bytes(mem);
      if (b != 2 && b != 4 && b != 8) return false;
      std::string dst = sim.Push();
      if (b == 8)
        check(Fmt("%s::OperandI64(int64_t(%s), &%s)", R, rd_mem(8).c_str(), dst.c_str()));
      else
        L(Fmt("%s = double(int%d_t(%s));", dst.c_str(), b * 8, rd_mem(b).c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FLD1: case ZYDIS_MNEMONIC_FLDZ: case ZYDIS_MNEMONIC_FLDPI:
    case ZYDIS_MNEMONIC_FLDL2E: case ZYDIS_MNEMONIC_FLDL2T: case ZYDIS_MNEMONIC_FLDLG2:
    case ZYDIS_MNEMONIC_FLDLN2: {
      std::string dst = sim.Push();
      if (m == ZYDIS_MNEMONIC_FLD1 || m == ZYDIS_MNEMONIC_FLDZ)
        L(dst + (m == ZYDIS_MNEMONIC_FLD1 ? " = 1.0;" : " = 0.0;"));
      else  // the others have 64 significand bits: always the exact path
        check(Fmt("%s::ToD(%s::ConstValue(rcw, ::recomp::x87::Const::%s), &%s)", R, R,
                  X87ConstName(m), dst.c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP: {
      std::string x = sim.Read(0);
      if (mem < 0) {
        std::string dst = sim.Write(St(0));
        L(dst + " = " + x + ";");
        c1("false");
      } else {
        switch (Bytes(mem)) {
          case 4:
            L(Fmt("const auto r = %s::StoreF32D(rcw, %s);", R, x.c_str()));
            L(Fmt("::recomp::lift::Wr<uint32_t>(B, m%d, uint32_t(r.bits));", mem));
            L(Fmt("rfl |= %s::Flags(r);", R));
            c1(Fmt("%s::Up(r)", R));
            break;
          case 8:
            L(Fmt("::recomp::lift::Wr<uint64_t>(B, m%d, %s::StoreF64D(%s));", mem, R,
                  x.c_str()));
            c1("false");
            break;
          case 10:
            L(Fmt("::recomp::lift::WrF80(B, m%d, %s::ToF80(%s::FromD(%s)));", mem, R, R,
                  x.c_str()));
            c1("false");
            break;
          default:
            return false;
        }
      }
      if (m == ZYDIS_MNEMONIC_FSTP) sim.Pop();
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP: {
      int b = Bytes(mem);
      if (b != 2 && b != 4 && b != 8) return false;
      std::string x = sim.Read(0);
      L(Fmt("const auto r = %s::StoreI%d(rcw, %s::FromD(%s));", R, b * 8, R, x.c_str()));
      L(Fmt("::recomp::lift::Wr<uint%d_t>(B, m%d, uint%d_t(r.bits));", b * 8, mem, b * 8));
      L(Fmt("rfl |= %s::Flags(r);", R));
      c1(Fmt("%s::Up(r)", R));
      if (m == ZYDIS_MNEMONIC_FISTP) sim.Pop();
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FXCH: {
      int i = Nvis() ? St(Nvis() - 1) : 1;
      std::string a = sim.Read(0), b = sim.Read(i);
      sim.Write(0);
      sim.Write(i);
      if (i != 0)
        L(Fmt("const double t = %s; %s = %s; %s = t;", a.c_str(), a.c_str(), b.c_str(),
              b.c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FCHS:
    case ZYDIS_MNEMONIC_FABS: {
      std::string x = sim.Read(0);
      sim.Write(0);
      L(m == ZYDIS_MNEMONIC_FCHS ? Fmt("%s = -%s;", x.c_str(), x.c_str())
                                 : Fmt("%s = %s::AbsD(%s);", x.c_str(), R, x.c_str()));
      c1("false");
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FSQRT: {
      std::string x = sim.Read(0);
      check(Fmt("%s::SqrtD(%s, &%s, &rfl, &rc1)", R, x.c_str(), x.c_str()));
      sim.c1_written = true;
      sim.Write(0);
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP:
    case ZYDIS_MNEMONIC_FTST: {
      std::string a = sim.Read(0);
      if (m == ZYDIS_MNEMONIC_FTST) {
        L(Fmt("rcc = %s::CompareD(%s, 0.0);", R, a.c_str()));
      } else if (mem >= 0) {
        int b = Bytes(mem);
        if (b != 4 && b != 8) return false;
        // Any zero or normal operand compares exactly (no range needed).
        L("double v;");
        check(Fmt("%s::%s(%s, &v)", R, b == 4 ? "OperandF32" : "CompareOperandF64",
                  rd_mem(b).c_str()));
        L(Fmt("rcc = %s::CompareD(%s, v);", R, a.c_str()));
      } else {
        int i = (m == ZYDIS_MNEMONIC_FCOMPP || m == ZYDIS_MNEMONIC_FUCOMPP)
                    ? 1 : (Nvis() ? St(Nvis() - 1) : 1);
        std::string b = sim.Read(i);
        L(Fmt("rcc = %s::CompareD(%s, %s);", R, a.c_str(), b.c_str()));
      }
      sim.cc_written = true;
      c1("false");
      if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP) sim.Pop();
      if (m == ZYDIS_MNEMONIC_FCOMPP || m == ZYDIS_MNEMONIC_FUCOMPP) {
        sim.Pop();
        sim.Pop();
      }
      return sim.ok;
    }
    case ZYDIS_MNEMONIC_FNSTSW:
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FWAIT:
      return EmitX87Fast(sim);  // no register values involved
    default:
      return false;
  }
}

// An instruction on MMX registers (any operand MM0-MM7), or EMMS.
bool InsnEmitter::IsMmxInsn() const {
  if (d_.mnemonic == ZYDIS_MNEMONIC_EMMS) return true;
  for (int i = 0; i < Nvis(); i++)
    if (IsMm(i)) return true;
  return false;
}

// MMX, lifted onto runtime/mmx.h over the x87 register file (c.fpu). Only
// the forms implemented there; anything else stays unimplemented.
void InsnEmitter::EmitMmx() {
  const auto m = d_.mnemonic;
  if (m == ZYDIS_MNEMONIC_EMMS) {
    L("::recomp::mmx::Emms(c.fpu);");
    return;
  }
  if (Nvis() != 2) return Unimpl();
  if (!PrepareMem()) return Unimpl();
  auto mm = [&](int i) { return Fmt("::recomp::mmx::Get(c.fpu, %d)", MmIndex(i)); };
  auto set0 = [&](const std::string& v) {
    L(Fmt("::recomp::mmx::Set(c.fpu, %d, %s);", MmIndex(0), v.c_str()));
  };
  // A 64-bit source: an MMX register or m64.
  auto src64 = [&](int i) -> std::string {
    if (IsMm(i)) return mm(i);
    if (IsMem(i) && Bytes(i) == 8) return Fmt("::recomp::lift::Rd<uint64_t>(B, m%d)", i);
    return "";
  };
  L("::recomp::mmx::Enter(c.fpu);");
  switch (m) {
    case ZYDIS_MNEMONIC_MOVD:
      if (IsMm(0)) {  // movd mm, r/m32: zero-extended
        set0(Fmt("uint64_t(%s)", Read(1, 4).c_str()));
      } else if (IsMm(1)) {  // movd r/m32, mm: the low dword
        L(Write(0, Fmt("uint32_t(%s)", mm(1).c_str()), 4));
      } else {
        return Unimpl();
      }
      return;
    case ZYDIS_MNEMONIC_MOVQ:
      if (IsMm(0)) {
        std::string s = src64(1);
        if (s.empty()) return Unimpl();
        set0(s);
      } else if (IsMem(0) && Bytes(0) == 8 && IsMm(1)) {
        L(Fmt("::recomp::lift::Wr<uint64_t>(B, m0, %s);", mm(1).c_str()));
      } else {
        return Unimpl();
      }
      return;
    default:
      break;
  }
  if (!IsMm(0)) return Unimpl();
  std::string s;
  if (m == ZYDIS_MNEMONIC_PUNPCKLBW && IsMem(1)) {
    // The memory form reads 32 bits (SDM: PUNPCKLBW mm, mm/m32).
    s = Fmt("uint64_t(::recomp::lift::Rd<uint32_t>(B, m1))");
  } else if ((m == ZYDIS_MNEMONIC_PSRLW || m == ZYDIS_MNEMONIC_PSRAD) && IsImm(1)) {
    s = Hex(uint32_t(Op(1).imm.value.u & 0xFF));
  } else {
    s = src64(1);
  }
  if (s.empty()) return Unimpl();
  const char* fn = nullptr;
  switch (m) {
    case ZYDIS_MNEMONIC_PXOR:
      set0(Fmt("(%s ^ %s)", mm(0).c_str(), s.c_str()));
      return;
    case ZYDIS_MNEMONIC_PSRLW: fn = "Psrlw"; break;
    case ZYDIS_MNEMONIC_PSRAD: fn = "Psrad"; break;
    case ZYDIS_MNEMONIC_PUNPCKLBW: fn = "Punpcklbw"; break;
    case ZYDIS_MNEMONIC_PUNPCKHWD: fn = "Punpckhwd"; break;
    case ZYDIS_MNEMONIC_PACKUSWB: fn = "Packuswb"; break;
    case ZYDIS_MNEMONIC_PACKSSDW: fn = "Packssdw"; break;
    case ZYDIS_MNEMONIC_PMULHW: fn = "Pmulhw"; break;
    case ZYDIS_MNEMONIC_PMULLW: fn = "Pmullw"; break;
    case ZYDIS_MNEMONIC_PADDUSB: fn = "Paddusb"; break;
    default: return Unimpl();
  }
  set0(Fmt("::recomp::mmx::%s(%s, %s)", fn, mm(0).c_str(), s.c_str()));
}

void InsnEmitter::EmitSse() {
  using namespace std::string_literals;
  const auto m = d_.mnemonic;
  for (int i = 0; i < Nvis(); i++)
    if (IsReg(i) && ZydisRegisterGetClass(Op(i).reg.value) == ZYDIS_REGCLASS_MMX)
      return Unimpl();
  if (!PrepareMem()) return Unimpl();
  const std::string va = Hex(in_.va);
  const bool aligned = m != ZYDIS_MNEMONIC_MOVUPS && m != ZYDIS_MNEMONIC_MOVUPD &&
                       m != ZYDIS_MNEMONIC_MOVDQU;
  auto src = [&](int i) { return XmmSrc(i, aligned); };
  auto imm8 = [&](int i) { return Hex(uint32_t(Op(i).imm.value.u & 0xFF)); };
  auto b = [](bool v) { return v ? "true" : "false"; };

  struct Arith {
    ZydisMnemonic m;
    const char* op;
    bool dbl;
    int lanes;
  };
  static const Arith kArith[] = {
      {ZYDIS_MNEMONIC_ADDPS, "kAdd", false, 4}, {ZYDIS_MNEMONIC_ADDPD, "kAdd", true, 2},
      {ZYDIS_MNEMONIC_ADDSS, "kAdd", false, 1}, {ZYDIS_MNEMONIC_ADDSD, "kAdd", true, 1},
      {ZYDIS_MNEMONIC_SUBPS, "kSub", false, 4}, {ZYDIS_MNEMONIC_SUBPD, "kSub", true, 2},
      {ZYDIS_MNEMONIC_SUBSS, "kSub", false, 1}, {ZYDIS_MNEMONIC_SUBSD, "kSub", true, 1},
      {ZYDIS_MNEMONIC_MULPS, "kMul", false, 4}, {ZYDIS_MNEMONIC_MULPD, "kMul", true, 2},
      {ZYDIS_MNEMONIC_MULSS, "kMul", false, 1}, {ZYDIS_MNEMONIC_MULSD, "kMul", true, 1},
      {ZYDIS_MNEMONIC_DIVPS, "kDiv", false, 4}, {ZYDIS_MNEMONIC_DIVPD, "kDiv", true, 2},
      {ZYDIS_MNEMONIC_DIVSS, "kDiv", false, 1}, {ZYDIS_MNEMONIC_DIVSD, "kDiv", true, 1},
  };
  for (const Arith& a : kArith)
    if (a.m == m) {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::Arith(c, ::recomp::sse::%s, %s, %d, %s, %s, %s);", a.op,
            b(a.dbl), a.lanes, XmmRef(0).c_str(), s.c_str(), va.c_str()));
      return;
    }

  switch (m) {
    case ZYDIS_MNEMONIC_CMPPS: case ZYDIS_MNEMONIC_CMPPD:
    case ZYDIS_MNEMONIC_CMPSS: case ZYDIS_MNEMONIC_CMPSD: {
      if (!IsXmm(0) || Nvis() < 3 || !IsImm(2)) return Unimpl();
      bool dbl = m == ZYDIS_MNEMONIC_CMPPD || m == ZYDIS_MNEMONIC_CMPSD;
      int lanes = m == ZYDIS_MNEMONIC_CMPPS ? 4 : m == ZYDIS_MNEMONIC_CMPPD ? 2 : 1;
      std::string s = src(1);
      L(Fmt("::recomp::sse::Cmp(c, %s, %d, %s, %s, %s, %s);", b(dbl), lanes,
            XmmRef(0).c_str(), s.c_str(), imm8(2).c_str(), va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_UCOMISD: {
      if (!IsXmm(0)) return Unimpl();
      bool dbl = m == ZYDIS_MNEMONIC_COMISD || m == ZYDIS_MNEMONIC_UCOMISD;
      bool ordered = m == ZYDIS_MNEMONIC_COMISS || m == ZYDIS_MNEMONIC_COMISD;
      std::string s = src(1);
      L(Fmt("const int r = ::recomp::sse::Comi(c, %s, %s, %s, %s, %s);", b(dbl),
            b(ordered), XmmRef(0).c_str(), s.c_str(), va.c_str()));
      L("F.zf = r >= 2; F.pf = r == 3; F.cf = r == 1 || r == 3;");
      L("F.of = false; F.sf = false; F.af = false;");
      return;
    }
    case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD: case ZYDIS_MNEMONIC_PAND:
    case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD: case ZYDIS_MNEMONIC_PANDN:
    case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD: case ZYDIS_MNEMONIC_POR:
    case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD: case ZYDIS_MNEMONIC_PXOR: {
      if (!IsXmm(0)) return Unimpl();
      const char* fn =
          m == ZYDIS_MNEMONIC_ANDPS || m == ZYDIS_MNEMONIC_ANDPD || m == ZYDIS_MNEMONIC_PAND
              ? "And"
          : m == ZYDIS_MNEMONIC_ANDNPS || m == ZYDIS_MNEMONIC_ANDNPD ||
                  m == ZYDIS_MNEMONIC_PANDN
              ? "AndNot"
          : m == ZYDIS_MNEMONIC_ORPS || m == ZYDIS_MNEMONIC_ORPD || m == ZYDIS_MNEMONIC_POR
              ? "Or"
              : "Xor";
      std::string s = src(1);
      L(Fmt("::recomp::sse::%s(%s, %s);", fn, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVAPD: case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVUPD: case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
      if (IsXmm(0)) {
        std::string s = src(1);
        L(XmmRef(0) + " = " + s + ";");
      } else {
        if (aligned) L(Fmt("::recomp::sse::CheckAligned(c, m0, %s);", va.c_str()));
        L(Fmt("::recomp::sse::St(B, m0, %s, 16);", XmmRef(1).c_str()));
      }
      return;
    case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD: {
      const char* t = m == ZYDIS_MNEMONIC_MOVSS ? "uint32_t" : "uint64_t";
      int n = m == ZYDIS_MNEMONIC_MOVSS ? 4 : 8;
      if (IsXmm(0) && IsXmm(1))
        L(Fmt("::recomp::sse::SetLane<%s>(%s, 0, ::recomp::sse::Lane<%s>(%s, 0));", t,
              XmmRef(0).c_str(), t, XmmRef(1).c_str()));
      else if (IsXmm(0))
        L(Fmt("%s = ::recomp::sse::Ld(B, m1, %d);", XmmRef(0).c_str(), n));
      else
        L(Fmt("::recomp::sse::St(B, m0, %s, %d);", XmmRef(1).c_str(), n));
      return;
    }
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD:
      if (IsXmm(0))
        L(Fmt("::recomp::sse::SetLane<uint64_t>(%s, 0, ::recomp::lift::Rd<uint64_t>(B, m1));",
              XmmRef(0).c_str()));
      else
        L(Fmt("::recomp::sse::St(B, m0, %s, 8);", XmmRef(1).c_str()));
      return;
    case ZYDIS_MNEMONIC_MOVD:
      if (IsXmm(0)) {
        L(Fmt("const uint32_t v = %s;", Read(1).c_str()));
        L(XmmRef(0) + " = ::recomp::Xmm{};");
        L(Fmt("::recomp::sse::SetLane<uint32_t>(%s, 0, v);", XmmRef(0).c_str()));
      } else {
        L(Write(0, Fmt("::recomp::sse::Lane<uint32_t>(%s, 0)", XmmRef(1).c_str())));
      }
      return;
    case ZYDIS_MNEMONIC_MOVQ:
      // x64: movq between an XMM and a 64-bit register.
      if (IsXmm(0) && IsReg(1) && !IsXmm(1)) {
        L(Fmt("const uint64_t v = %s;", Read(1).c_str()));
        L(XmmRef(0) + " = ::recomp::Xmm{};");
        L(Fmt("::recomp::sse::SetLane<uint64_t>(%s, 0, v);", XmmRef(0).c_str()));
        return;
      }
      if (IsReg(0) && !IsXmm(0)) {
        L(Write(0, Fmt("::recomp::sse::Lane<uint64_t>(%s, 0)", XmmRef(1).c_str())));
        return;
      }
      if (IsXmm(0)) {
        std::string s = src(1);
        L(Fmt("const uint64_t v = ::recomp::sse::Lane<uint64_t>(%s, 0);", s.c_str()));
        L(XmmRef(0) + " = ::recomp::Xmm{};");
        L(Fmt("::recomp::sse::SetLane<uint64_t>(%s, 0, v);", XmmRef(0).c_str()));
      } else {
        L(Fmt("::recomp::sse::St(B, m0, %s, 8);", XmmRef(1).c_str()));
      }
      return;
    case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_CVTTSS2SI:
    case ZYDIS_MNEMONIC_CVTSD2SI: case ZYDIS_MNEMONIC_CVTTSD2SI: {
      if (!IsReg(0) || IsXmm(0)) return Unimpl();
      bool dbl = m == ZYDIS_MNEMONIC_CVTSD2SI || m == ZYDIS_MNEMONIC_CVTTSD2SI;
      bool trunc = m == ZYDIS_MNEMONIC_CVTTSS2SI || m == ZYDIS_MNEMONIC_CVTTSD2SI;
      std::string s = src(1);
      L(Write(0, Fmt("::recomp::sse::CvtToI%d(c, %s, %s, %s, %s)", Bytes(0) == 8 ? 64 : 32,
                     b(dbl), s.c_str(), b(trunc), va.c_str())));
      return;
    }
    case ZYDIS_MNEMONIC_CVTSI2SS:
      if (!IsXmm(0) || IsXmm(1)) return Unimpl();
      L(Fmt("::recomp::sse::CvtIntToF(c, false, %s, %s, %s, %s);", b(Bytes(1) == 8),
            Read(1).c_str(), XmmRef(0).c_str(), va.c_str()));
      return;
    case ZYDIS_MNEMONIC_CVTDQ2PS: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::Cvtdq2ps(c, %s, %s, %s);", XmmRef(0).c_str(), s.c_str(),
            va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_CVTSS2SD: case ZYDIS_MNEMONIC_CVTPS2PD:
    case ZYDIS_MNEMONIC_CVTSD2SS: case ZYDIS_MNEMONIC_CVTPD2PS: {
      if (!IsXmm(0)) return Unimpl();
      bool to_dbl = m == ZYDIS_MNEMONIC_CVTSS2SD || m == ZYDIS_MNEMONIC_CVTPS2PD;
      int lanes = m == ZYDIS_MNEMONIC_CVTPS2PD || m == ZYDIS_MNEMONIC_CVTPD2PS ? 2 : 1;
      std::string s = src(1);
      L(Fmt("::recomp::sse::CvtFloat(c, %s, %d, %s, %s, %s);", b(to_dbl), lanes,
            XmmRef(0).c_str(), s.c_str(), va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MINSD: case ZYDIS_MNEMONIC_MINPS:
    case ZYDIS_MNEMONIC_MINPD: case ZYDIS_MNEMONIC_MAXSS: case ZYDIS_MNEMONIC_MAXSD:
    case ZYDIS_MNEMONIC_MAXPS: case ZYDIS_MNEMONIC_MAXPD: {
      if (!IsXmm(0)) return Unimpl();
      bool max = m == ZYDIS_MNEMONIC_MAXSS || m == ZYDIS_MNEMONIC_MAXSD ||
                 m == ZYDIS_MNEMONIC_MAXPS || m == ZYDIS_MNEMONIC_MAXPD;
      bool dbl = m == ZYDIS_MNEMONIC_MINSD || m == ZYDIS_MNEMONIC_MINPD ||
                 m == ZYDIS_MNEMONIC_MAXSD || m == ZYDIS_MNEMONIC_MAXPD;
      bool packed = m == ZYDIS_MNEMONIC_MINPS || m == ZYDIS_MNEMONIC_MINPD ||
                    m == ZYDIS_MNEMONIC_MAXPS || m == ZYDIS_MNEMONIC_MAXPD;
      std::string s = src(1);
      L(Fmt("::recomp::sse::MinMax(c, %s, %s, %d, %s, %s, %s);", b(max), b(dbl),
            packed ? (dbl ? 2 : 4) : 1, XmmRef(0).c_str(), s.c_str(), va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PSRLDQ: case ZYDIS_MNEMONIC_PSLLDQ:
      if (!IsXmm(0) || !IsImm(1)) return Unimpl();
      L(Fmt("::recomp::sse::%s(%s, %s);", m == ZYDIS_MNEMONIC_PSRLDQ ? "ShrDq" : "ShlDq",
            XmmRef(0).c_str(), imm8(1).c_str()));
      return;
    case ZYDIS_MNEMONIC_CVTDQ2PD: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("%s = ::recomp::sse::Cvtdq2pd(%s);", XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_CVTSI2SD:
      if (!IsXmm(0) || IsXmm(1)) return Unimpl();
      if (Bytes(1) == 8) {  // int64 -> double can be inexact
        L(Fmt("::recomp::sse::CvtIntToF(c, true, true, %s, %s, %s);", Read(1).c_str(),
              XmmRef(0).c_str(), va.c_str()));
        return;
      }
      L(Fmt("::recomp::sse::Cvtsi2sd(%s, int32_t(%s));", XmmRef(0).c_str(), Read(1).c_str()));
      return;
    case ZYDIS_MNEMONIC_SQRTSD: case ZYDIS_MNEMONIC_SQRTSS: case ZYDIS_MNEMONIC_SQRTPS:
    case ZYDIS_MNEMONIC_SQRTPD: {
      if (!IsXmm(0)) return Unimpl();
      bool dbl = m == ZYDIS_MNEMONIC_SQRTSD || m == ZYDIS_MNEMONIC_SQRTPD;
      int lanes = m == ZYDIS_MNEMONIC_SQRTPS ? 4 : m == ZYDIS_MNEMONIC_SQRTPD ? 2 : 1;
      std::string s = src(1);
      L(Fmt("::recomp::sse::Sqrt(c, %s, %d, %s, %s, %s);", b(dbl), lanes, XmmRef(0).c_str(),
            s.c_str(), va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_RSQRTPS: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::Rsqrt(%s, %s);", XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PADDQ: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::Add<uint64_t>(%s, %s);", XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PCMPEQB: case ZYDIS_MNEMONIC_PCMPEQW: case ZYDIS_MNEMONIC_PCMPEQD:
    case ZYDIS_MNEMONIC_PSUBB: case ZYDIS_MNEMONIC_PSUBW: case ZYDIS_MNEMONIC_PSUBD:
    case ZYDIS_MNEMONIC_PSUBQ: {
      if (!IsXmm(0)) return Unimpl();
      bool eq = m == ZYDIS_MNEMONIC_PCMPEQB || m == ZYDIS_MNEMONIC_PCMPEQW ||
                m == ZYDIS_MNEMONIC_PCMPEQD;
      const char* t = m == ZYDIS_MNEMONIC_PCMPEQB || m == ZYDIS_MNEMONIC_PSUBB ? "uint8_t"
                      : m == ZYDIS_MNEMONIC_PCMPEQW || m == ZYDIS_MNEMONIC_PSUBW
                          ? "uint16_t"
                      : m == ZYDIS_MNEMONIC_PSUBQ ? "uint64_t"
                                                  : "uint32_t";
      std::string s = src(1);
      L(Fmt("::recomp::sse::%s<%s>(%s, %s);", eq ? "CmpEq" : "Sub", t,
            XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PMAXSW: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::MaxS16(%s, %s);", XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PMOVMSKB:
      if (!IsXmm(1)) return Unimpl();
      L(Write(0, Fmt("::recomp::sse::MovMskB(%s)", XmmRef(1).c_str())));
      return;
    case ZYDIS_MNEMONIC_PSHUFD: case ZYDIS_MNEMONIC_SHUFPS: {
      if (!IsXmm(0) || Nvis() < 3) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::%s(%s, %s, %s);",
            m == ZYDIS_MNEMONIC_PSHUFD ? "ShufD" : "ShufPs", XmmRef(0).c_str(),
            s.c_str(), imm8(2).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PSLLQ: case ZYDIS_MNEMONIC_PSRLQ: {
      if (!IsXmm(0)) return Unimpl();
      std::string count = IsImm(1) ? imm8(1)
                                   : "::recomp::sse::Lane<uint64_t>("s + src(1) + ", 0)";
      L(Fmt("::recomp::sse::%s(%s, %s);", m == ZYDIS_MNEMONIC_PSLLQ ? "ShlQ" : "ShrQ",
            XmmRef(0).c_str(), count.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PEXTRW:
      if (!IsXmm(1) || IsXmm(0)) return Unimpl();
      L(Write(0, Fmt("::recomp::sse::Lane<uint16_t>(%s, %u)", XmmRef(1).c_str(),
                     unsigned(Op(2).imm.value.u & 7))));
      return;
    case ZYDIS_MNEMONIC_PINSRW:
      if (!IsXmm(0)) return Unimpl();
      L(Fmt("::recomp::sse::SetLane<uint16_t>(%s, %u, uint16_t(%s));", XmmRef(0).c_str(),
            unsigned(Op(2).imm.value.u & 7), Read(1).c_str()));
      return;
    case ZYDIS_MNEMONIC_UNPCKLPD: case ZYDIS_MNEMONIC_UNPCKHPD:
    case ZYDIS_MNEMONIC_MOVLHPS: case ZYDIS_MNEMONIC_MOVHLPS: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      const char* fn = m == ZYDIS_MNEMONIC_UNPCKLPD   ? "UnpckLPd"
                       : m == ZYDIS_MNEMONIC_UNPCKHPD ? "UnpckHPd"
                       : m == ZYDIS_MNEMONIC_MOVLHPS  ? "MovLHPs"
                                                      : "MovHLPs";
      L(Fmt("::recomp::sse::%s(%s, %s);", fn, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_UNPCKHPS: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::UnpckPs(%s, %s, %s);", XmmRef(0).c_str(), s.c_str(),
            b(m == ZYDIS_MNEMONIC_UNPCKHPS)));
      return;
    }
    case ZYDIS_MNEMONIC_SHUFPD: {
      if (!IsXmm(0) || Nvis() < 3) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::ShufPd(%s, %s, %s);", XmmRef(0).c_str(), s.c_str(),
            imm8(2).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PADDB: case ZYDIS_MNEMONIC_PADDW: case ZYDIS_MNEMONIC_PADDD: {
      if (!IsXmm(0)) return Unimpl();
      const char* t = m == ZYDIS_MNEMONIC_PADDB   ? "uint8_t"
                      : m == ZYDIS_MNEMONIC_PADDW ? "uint16_t"
                                                  : "uint32_t";
      std::string s = src(1);
      L(Fmt("::recomp::sse::Add<%s>(%s, %s);", t, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PSLLW: case ZYDIS_MNEMONIC_PSLLD: case ZYDIS_MNEMONIC_PSRLW:
    case ZYDIS_MNEMONIC_PSRLD: case ZYDIS_MNEMONIC_PSRAW: case ZYDIS_MNEMONIC_PSRAD: {
      if (!IsXmm(0)) return Unimpl();
      bool word = m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSRLW ||
                  m == ZYDIS_MNEMONIC_PSRAW;
      const char* k = m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSLLD ? "kShl"
                      : m == ZYDIS_MNEMONIC_PSRLW || m == ZYDIS_MNEMONIC_PSRLD ? "kShr"
                                                                               : "kSar";
      std::string count = IsImm(1) ? imm8(1)
                                   : "::recomp::sse::Lane<uint64_t>("s + src(1) + ", 0)";
      L(Fmt("::recomp::sse::Shift<%s>(%s, %s, ::recomp::sse::%s);",
            word ? "uint16_t" : "uint32_t", XmmRef(0).c_str(), count.c_str(), k));
      return;
    }
    case ZYDIS_MNEMONIC_PUNPCKLBW: case ZYDIS_MNEMONIC_PUNPCKLWD:
    case ZYDIS_MNEMONIC_PUNPCKLDQ: case ZYDIS_MNEMONIC_PUNPCKLQDQ:
    case ZYDIS_MNEMONIC_PUNPCKHBW: case ZYDIS_MNEMONIC_PUNPCKHWD:
    case ZYDIS_MNEMONIC_PUNPCKHDQ: case ZYDIS_MNEMONIC_PUNPCKHQDQ: {
      if (!IsXmm(0)) return Unimpl();
      const char* t = m == ZYDIS_MNEMONIC_PUNPCKLBW || m == ZYDIS_MNEMONIC_PUNPCKHBW ? "uint8_t"
                      : m == ZYDIS_MNEMONIC_PUNPCKLWD || m == ZYDIS_MNEMONIC_PUNPCKHWD
                          ? "uint16_t"
                      : m == ZYDIS_MNEMONIC_PUNPCKLDQ || m == ZYDIS_MNEMONIC_PUNPCKHDQ
                          ? "uint32_t"
                          : "uint64_t";
      bool high = m == ZYDIS_MNEMONIC_PUNPCKHBW || m == ZYDIS_MNEMONIC_PUNPCKHWD ||
                  m == ZYDIS_MNEMONIC_PUNPCKHDQ || m == ZYDIS_MNEMONIC_PUNPCKHQDQ;
      std::string s = src(1);
      L(Fmt("::recomp::sse::Unpck<%s>(%s, %s, %s);", t, XmmRef(0).c_str(), s.c_str(),
            b(high)));
      return;
    }
    case ZYDIS_MNEMONIC_PSHUFLW: case ZYDIS_MNEMONIC_PSHUFHW: {
      if (!IsXmm(0) || Nvis() < 3) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::ShufW(%s, %s, %s, %s);", XmmRef(0).c_str(), s.c_str(),
            imm8(2).c_str(), b(m == ZYDIS_MNEMONIC_PSHUFHW)));
      return;
    }
    case ZYDIS_MNEMONIC_PMULUDQ: case ZYDIS_MNEMONIC_PMULDQ: case ZYDIS_MNEMONIC_PMULLD:
    case ZYDIS_MNEMONIC_PMULLW: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      const char* fn = m == ZYDIS_MNEMONIC_PMULUDQ  ? "MulUDq"
                       : m == ZYDIS_MNEMONIC_PMULDQ ? "MulDq"
                       : m == ZYDIS_MNEMONIC_PMULLD ? "MulLo<uint32_t>"
                                                    : "MulLo<uint16_t>";
      L(Fmt("::recomp::sse::%s(%s, %s);", fn, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PACKSSDW: case ZYDIS_MNEMONIC_PACKSSWB:
    case ZYDIS_MNEMONIC_PACKUSWB: case ZYDIS_MNEMONIC_PACKUSDW: {
      if (!IsXmm(0)) return Unimpl();
      const char* tt = m == ZYDIS_MNEMONIC_PACKSSDW   ? "int32_t, int16_t"
                       : m == ZYDIS_MNEMONIC_PACKSSWB ? "int16_t, int8_t"
                       : m == ZYDIS_MNEMONIC_PACKUSWB ? "int16_t, uint8_t"
                                                      : "int32_t, uint16_t";
      std::string s = src(1);
      L(Fmt("::recomp::sse::Pack<%s>(%s, %s);", tt, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PMOVSXBW: case ZYDIS_MNEMONIC_PMOVSXBD: case ZYDIS_MNEMONIC_PMOVSXBQ:
    case ZYDIS_MNEMONIC_PMOVSXWD: case ZYDIS_MNEMONIC_PMOVSXWQ: case ZYDIS_MNEMONIC_PMOVSXDQ:
    case ZYDIS_MNEMONIC_PMOVZXBW: case ZYDIS_MNEMONIC_PMOVZXBD: case ZYDIS_MNEMONIC_PMOVZXBQ:
    case ZYDIS_MNEMONIC_PMOVZXWD: case ZYDIS_MNEMONIC_PMOVZXWQ: case ZYDIS_MNEMONIC_PMOVZXDQ: {
      if (!IsXmm(0)) return Unimpl();
      struct X { ZydisMnemonic m; const char* t; };
      static const X kX[] = {
          {ZYDIS_MNEMONIC_PMOVSXBW, "int8_t, int16_t"}, {ZYDIS_MNEMONIC_PMOVSXBD, "int8_t, int32_t"},
          {ZYDIS_MNEMONIC_PMOVSXBQ, "int8_t, int64_t"}, {ZYDIS_MNEMONIC_PMOVSXWD, "int16_t, int32_t"},
          {ZYDIS_MNEMONIC_PMOVSXWQ, "int16_t, int64_t"}, {ZYDIS_MNEMONIC_PMOVSXDQ, "int32_t, int64_t"},
          {ZYDIS_MNEMONIC_PMOVZXBW, "uint8_t, uint16_t"}, {ZYDIS_MNEMONIC_PMOVZXBD, "uint8_t, uint32_t"},
          {ZYDIS_MNEMONIC_PMOVZXBQ, "uint8_t, uint64_t"}, {ZYDIS_MNEMONIC_PMOVZXWD, "uint16_t, uint32_t"},
          {ZYDIS_MNEMONIC_PMOVZXWQ, "uint16_t, uint64_t"}, {ZYDIS_MNEMONIC_PMOVZXDQ, "uint32_t, uint64_t"}};
      const char* tt = nullptr;
      for (const X& x : kX)
        if (x.m == m) tt = x.t;
      std::string s = XmmSrc(1, false);  // m16..m64: no alignment
      L(Fmt("::recomp::sse::Extend<%s>(%s, %s);", tt, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PMAXUB: case ZYDIS_MNEMONIC_PMAXUW: case ZYDIS_MNEMONIC_PMAXUD:
    case ZYDIS_MNEMONIC_PMAXSB: case ZYDIS_MNEMONIC_PMAXSD: case ZYDIS_MNEMONIC_PMINUB:
    case ZYDIS_MNEMONIC_PMINUW: case ZYDIS_MNEMONIC_PMINUD: case ZYDIS_MNEMONIC_PMINSB:
    case ZYDIS_MNEMONIC_PMINSW: case ZYDIS_MNEMONIC_PMINSD: {
      if (!IsXmm(0)) return Unimpl();
      struct X { ZydisMnemonic m; const char* f; };
      static const X kX[] = {
          {ZYDIS_MNEMONIC_PMAXUB, "Max<uint8_t>"}, {ZYDIS_MNEMONIC_PMAXUW, "Max<uint16_t>"},
          {ZYDIS_MNEMONIC_PMAXUD, "Max<uint32_t>"}, {ZYDIS_MNEMONIC_PMAXSB, "Max<int8_t>"},
          {ZYDIS_MNEMONIC_PMAXSD, "Max<int32_t>"}, {ZYDIS_MNEMONIC_PMINUB, "Min<uint8_t>"},
          {ZYDIS_MNEMONIC_PMINUW, "Min<uint16_t>"}, {ZYDIS_MNEMONIC_PMINUD, "Min<uint32_t>"},
          {ZYDIS_MNEMONIC_PMINSB, "Min<int8_t>"}, {ZYDIS_MNEMONIC_PMINSW, "Min<int16_t>"},
          {ZYDIS_MNEMONIC_PMINSD, "Min<int32_t>"}};
      const char* fn = nullptr;
      for (const X& x : kX)
        if (x.m == m) fn = x.f;
      std::string s = src(1);
      L(Fmt("::recomp::sse::%s(%s, %s);", fn, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PCMPGTB: case ZYDIS_MNEMONIC_PCMPGTW: case ZYDIS_MNEMONIC_PCMPGTD: {
      if (!IsXmm(0)) return Unimpl();
      const char* t = m == ZYDIS_MNEMONIC_PCMPGTB   ? "int8_t"
                      : m == ZYDIS_MNEMONIC_PCMPGTW ? "int16_t"
                                                    : "int32_t";
      std::string s = src(1);
      L(Fmt("::recomp::sse::CmpGt<%s>(%s, %s);", t, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PABSB: case ZYDIS_MNEMONIC_PABSW: case ZYDIS_MNEMONIC_PABSD: {
      if (!IsXmm(0)) return Unimpl();
      const char* t = m == ZYDIS_MNEMONIC_PABSB   ? "int8_t"
                      : m == ZYDIS_MNEMONIC_PABSW ? "int16_t"
                                                  : "int32_t";
      std::string s = src(1);
      L(Fmt("::recomp::sse::Abs<%s>(%s, %s);", t, XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_PALIGNR: {
      if (!IsXmm(0) || Nvis() < 3) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::Alignr(%s, %s, %s);", XmmRef(0).c_str(), s.c_str(),
            imm8(2).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_MOVMSKPS: case ZYDIS_MNEMONIC_MOVMSKPD:
      if (!IsXmm(1)) return Unimpl();
      L(Write(0, Fmt("::recomp::sse::MovMsk%s(%s)", m == ZYDIS_MNEMONIC_MOVMSKPS ? "Ps" : "Pd",
                     XmmRef(1).c_str())));
      return;
    case ZYDIS_MNEMONIC_MOVHPD: case ZYDIS_MNEMONIC_MOVHPS:
      if (IsXmm(0))
        L(Fmt("::recomp::sse::SetLane<uint64_t>(%s, 1, ::recomp::lift::Rd<uint64_t>(B, m1));",
              XmmRef(0).c_str()));
      else
        L(Fmt("::recomp::lift::Wr<uint64_t>(B, m0, ::recomp::sse::Lane<uint64_t>(%s, 1));",
              XmmRef(1).c_str()));
      return;
    case ZYDIS_MNEMONIC_CVTTPS2DQ: case ZYDIS_MNEMONIC_CVTPS2DQ:
    case ZYDIS_MNEMONIC_CVTTPD2DQ: case ZYDIS_MNEMONIC_CVTPD2DQ: {
      if (!IsXmm(0)) return Unimpl();
      bool dbl = m == ZYDIS_MNEMONIC_CVTTPD2DQ || m == ZYDIS_MNEMONIC_CVTPD2DQ;
      bool trunc = m == ZYDIS_MNEMONIC_CVTTPS2DQ || m == ZYDIS_MNEMONIC_CVTTPD2DQ;
      std::string s = src(1);
      L(Fmt("::recomp::sse::CvtToDq(c, %s, %s, %s, %s, %s);", b(dbl), b(trunc),
            XmmRef(0).c_str(), s.c_str(), va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_RSQRTSS: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::RsqrtSs(%s, %s);", XmmRef(0).c_str(), s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_BLENDVPS: case ZYDIS_MNEMONIC_BLENDVPD: {
      if (!IsXmm(0)) return Unimpl();
      std::string s = src(1);
      L(Fmt("::recomp::sse::BlendV<%s>(%s, %s, c.xmm[0]);",
            m == ZYDIS_MNEMONIC_BLENDVPS ? "uint32_t" : "uint64_t", XmmRef(0).c_str(),
            s.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_STMXCSR:
      L("::recomp::lift::Wr<uint32_t>(B, m0, c.mxcsr);");
      return;
    case ZYDIS_MNEMONIC_LDMXCSR:
      L(Fmt("::recomp::sse::Ldmxcsr(c, ::recomp::lift::Rd<uint32_t>(B, m0), %s);",
            va.c_str()));
      return;
    case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2:
      return;  // a hint
    default:
      return Unimpl();
  }
}



// AVX (VEX encodings) on the same runtime/sse lanes, one 128-bit half at
// a time. A VEX.128 write zeroes bits 255:128 of the destination; legacy
// SSE never touches them. Memory operands need no alignment except for
// vmovaps/vmovapd/vmovdqa (16 or 32 bytes).
void InsnEmitter::EmitAvx() {
  using namespace std::string_literals;
  const auto m = d_.mnemonic;
  if (!PrepareMem()) return Unimpl();
  const std::string va = Hex(in_.va);
  const int halves = d_.avx.vector_length == 256 ? 2 : 1;
  auto imm8 = [&](int i) { return unsigned(Op(i).imm.value.u & 0xFF); };
  auto b = [](bool v) { return v ? "true" : "false"; };

  // dst = src1 op src2, lane-wise per half: `call` gets the temporary
  // (a copy of src1's half) and src2's half.
  auto binary = [&](const std::function<std::string(const std::string&, const std::string&)>& call,
                    int src2_bytes = 16) {
    if (!IsVec(0) || !IsVec(1)) return Unimpl();
    for (int h = 0; h < halves; h++) {
      std::string s2 = HalfSrc(2, h, src2_bytes);
      L("{ ::recomp::Xmm t = " + Half(1, h) + "; " + call("t", s2) + " " + Half(0, h) +
        " = t; }");
    }
    if (halves == 1) ZeroUpper(0);
  };

  struct Arith {
    ZydisMnemonic m;
    const char* op;
    bool dbl;
    int lanes;  // per half; 1 for scalar forms
  };
  static const Arith kArith[] = {
      {ZYDIS_MNEMONIC_VADDPS, "kAdd", false, 4}, {ZYDIS_MNEMONIC_VADDPD, "kAdd", true, 2},
      {ZYDIS_MNEMONIC_VADDSS, "kAdd", false, 1}, {ZYDIS_MNEMONIC_VADDSD, "kAdd", true, 1},
      {ZYDIS_MNEMONIC_VSUBPS, "kSub", false, 4}, {ZYDIS_MNEMONIC_VSUBPD, "kSub", true, 2},
      {ZYDIS_MNEMONIC_VSUBSS, "kSub", false, 1}, {ZYDIS_MNEMONIC_VSUBSD, "kSub", true, 1},
      {ZYDIS_MNEMONIC_VMULPS, "kMul", false, 4}, {ZYDIS_MNEMONIC_VMULPD, "kMul", true, 2},
      {ZYDIS_MNEMONIC_VMULSS, "kMul", false, 1}, {ZYDIS_MNEMONIC_VMULSD, "kMul", true, 1},
      {ZYDIS_MNEMONIC_VDIVPS, "kDiv", false, 4}, {ZYDIS_MNEMONIC_VDIVPD, "kDiv", true, 2},
      {ZYDIS_MNEMONIC_VDIVSS, "kDiv", false, 1}, {ZYDIS_MNEMONIC_VDIVSD, "kDiv", true, 1},
  };
  for (const Arith& a : kArith)
    if (a.m == m) {
      int bytes = a.lanes == 1 ? (a.dbl ? 8 : 4) : 16;
      return binary([&](const std::string& t, const std::string& s2) {
        return Fmt("::recomp::sse::Arith(c, ::recomp::sse::%s, %s, %d, %s, %s, %s);", a.op,
                   b(a.dbl), a.lanes, t.c_str(), s2.c_str(), va.c_str());
      }, bytes);
    }

  switch (m) {
    case ZYDIS_MNEMONIC_VMINPS: case ZYDIS_MNEMONIC_VMINPD: case ZYDIS_MNEMONIC_VMINSS:
    case ZYDIS_MNEMONIC_VMINSD: case ZYDIS_MNEMONIC_VMAXPS: case ZYDIS_MNEMONIC_VMAXPD:
    case ZYDIS_MNEMONIC_VMAXSS: case ZYDIS_MNEMONIC_VMAXSD: {
      bool max = m == ZYDIS_MNEMONIC_VMAXPS || m == ZYDIS_MNEMONIC_VMAXPD ||
                 m == ZYDIS_MNEMONIC_VMAXSS || m == ZYDIS_MNEMONIC_VMAXSD;
      bool dbl = m == ZYDIS_MNEMONIC_VMINPD || m == ZYDIS_MNEMONIC_VMINSD ||
                 m == ZYDIS_MNEMONIC_VMAXPD || m == ZYDIS_MNEMONIC_VMAXSD;
      bool scalar = m == ZYDIS_MNEMONIC_VMINSS || m == ZYDIS_MNEMONIC_VMINSD ||
                    m == ZYDIS_MNEMONIC_VMAXSS || m == ZYDIS_MNEMONIC_VMAXSD;
      int lanes = scalar ? 1 : dbl ? 2 : 4;
      return binary([&](const std::string& t, const std::string& s2) {
        return Fmt("::recomp::sse::MinMax(c, %s, %s, %d, %s, %s, %s);", b(max), b(dbl), lanes,
                   t.c_str(), s2.c_str(), va.c_str());
      }, scalar ? (dbl ? 8 : 4) : 16);
    }
    case ZYDIS_MNEMONIC_VANDPS: case ZYDIS_MNEMONIC_VANDPD: case ZYDIS_MNEMONIC_VANDNPS:
    case ZYDIS_MNEMONIC_VANDNPD: case ZYDIS_MNEMONIC_VORPS: case ZYDIS_MNEMONIC_VORPD:
    case ZYDIS_MNEMONIC_VXORPS: case ZYDIS_MNEMONIC_VXORPD: {
      const char* fn = m == ZYDIS_MNEMONIC_VANDPS || m == ZYDIS_MNEMONIC_VANDPD     ? "And"
                       : m == ZYDIS_MNEMONIC_VANDNPS || m == ZYDIS_MNEMONIC_VANDNPD ? "AndNot"
                       : m == ZYDIS_MNEMONIC_VORPS || m == ZYDIS_MNEMONIC_VORPD     ? "Or"
                                                                                    : "Xor";
      return binary([&](const std::string& t, const std::string& s2) {
        return Fmt("::recomp::sse::%s(%s, %s);", fn, t.c_str(), s2.c_str());
      });
    }
    case ZYDIS_MNEMONIC_VSHUFPS:
      return binary([&](const std::string& t, const std::string& s2) {
        return Fmt("::recomp::sse::ShufPs(%s, %s, 0x%Xu);", t.c_str(), s2.c_str(), imm8(3));
      });
    case ZYDIS_MNEMONIC_VUNPCKLPS: case ZYDIS_MNEMONIC_VUNPCKHPS:
      return binary([&](const std::string& t, const std::string& s2) {
        return Fmt("::recomp::sse::UnpckPs(%s, %s, %s);", t.c_str(), s2.c_str(),
                   b(m == ZYDIS_MNEMONIC_VUNPCKHPS));
      });
    case ZYDIS_MNEMONIC_VBLENDPS: case ZYDIS_MNEMONIC_VBLENDPD: {
      // imm bit k picks src2's lane k across the whole register.
      const bool dbl = m == ZYDIS_MNEMONIC_VBLENDPD;
      const int per = dbl ? 2 : 4;
      const unsigned imm = imm8(3);
      for (int h = 0; h < halves; h++) {
        std::string s2 = HalfSrc(2, h);
        std::string sel;
        for (int i = 0; i < per; i++)
          if (imm >> (h * per + i) & 1)
            sel += Fmt(" ::recomp::sse::SetLane<%s>(t, %d, ::recomp::sse::Lane<%s>(%s, %d));",
                       dbl ? "uint64_t" : "uint32_t", i, dbl ? "uint64_t" : "uint32_t",
                       s2.c_str(), i);
        L("{ ::recomp::Xmm t = " + Half(1, h) + ";" + sel + " " + Half(0, h) + " = t; }");
      }
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VPERMILPS: {
      if (!IsVec(0) || !IsImm(2)) return Unimpl();  // the vector-control form
      // ShufPs(d, s, imm) takes lanes 0-1 from d and 2-3 from s; with both
      // the same source it permutes one register.
      for (int h = 0; h < halves; h++) {
        std::string s1 = HalfSrc(1, h);
        L(Fmt("{ ::recomp::Xmm t = %s; ::recomp::sse::ShufPs(t, %s, 0x%Xu); %s = t; }",
              s1.c_str(), s1.c_str(), imm8(2), Half(0, h).c_str()));
      }
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VMOVUPS: case ZYDIS_MNEMONIC_VMOVUPD: case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVAPD: case ZYDIS_MNEMONIC_VMOVDQU: case ZYDIS_MNEMONIC_VMOVDQA: {
      const bool aligned = m == ZYDIS_MNEMONIC_VMOVAPS || m == ZYDIS_MNEMONIC_VMOVAPD ||
                           m == ZYDIS_MNEMONIC_VMOVDQA;
      if (IsMem(0) || IsMem(1)) {
        int mi = IsMem(0) ? 0 : 1;
        if (aligned)
          L(Fmt("::recomp::sse::CheckAligned(c, m%d, %s, %du);", mi, va.c_str(), 16 * halves));
      }
      if (IsMem(0)) {
        for (int h = 0; h < halves; h++)
          L(Fmt("::recomp::sse::St(B, m0 + %du, %s, 16);", 16 * h, Half(1, h).c_str()));
        return;
      }
      if (IsMem(1)) {
        for (int h = 0; h < halves; h++)
          L(Fmt("%s = ::recomp::sse::Ld(B, m1 + %du, 16);", Half(0, h).c_str(), 16 * h));
      } else {
        for (int h = 0; h < halves; h++) L(Half(0, h) + " = " + Half(1, h) + ";");
      }
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VMOVSS: case ZYDIS_MNEMONIC_VMOVSD: {
      const int n = m == ZYDIS_MNEMONIC_VMOVSS ? 4 : 8;
      const char* t = n == 4 ? "uint32_t" : "uint64_t";
      if (IsMem(0)) {
        L(Fmt("::recomp::sse::St(B, m0, %s, %d);", Half(1, 0).c_str(), n));
        return;
      }
      if (IsMem(1)) {
        L(Fmt("%s = ::recomp::sse::Ld(B, m1, %d);", Half(0, 0).c_str(), n));
      } else {  // xmm1 = xmm2 with the low lane of xmm3
        L(Fmt("{ ::recomp::Xmm t = %s; ::recomp::sse::SetLane<%s>(t, 0, "
              "::recomp::sse::Lane<%s>(%s, 0)); %s = t; }",
              Half(1, 0).c_str(), t, t, Half(2, 0).c_str(), Half(0, 0).c_str()));
      }
      ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VBROADCASTSS: case ZYDIS_MNEMONIC_VBROADCASTSD:
    case ZYDIS_MNEMONIC_VBROADCASTF128: {
      if (!IsMem(1)) return Unimpl();  // register sources are AVX2
      if (m == ZYDIS_MNEMONIC_VBROADCASTF128) {
        L("const ::recomp::Xmm v = ::recomp::sse::Ld(B, m1, 16);");
      } else {
        const bool sd = m == ZYDIS_MNEMONIC_VBROADCASTSD;
        const char* t = sd ? "uint64_t" : "uint32_t";
        L(Fmt("const %s x = ::recomp::lift::Rd<%s>(B, m1); ::recomp::Xmm v;", t, t));
        L(Fmt("for (int i = 0; i < %d; i++) ::recomp::sse::SetLane<%s>(v, i, x);", sd ? 2 : 4, t));
      }
      for (int h = 0; h < halves; h++) L(Half(0, h) + " = v;");
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VINSERTF128: {
      const unsigned sel = imm8(3) & 1;
      std::string s3 = HalfSrc(2, 0);
      L("{ ::recomp::Xmm lo = " + Half(1, 0) + ", hi = " + Half(1, 1) + "; " +
        (sel ? "hi" : "lo") + " = " + s3 + "; " + Half(0, 0) + " = lo; " + Half(0, 1) +
        " = hi; }");
      return;
    }
    case ZYDIS_MNEMONIC_VEXTRACTF128: {
      const std::string v = Half(1, imm8(2) & 1);
      if (IsMem(0)) {
        L(Fmt("::recomp::sse::St(B, m0, %s, 16);", v.c_str()));
      } else {
        L(Half(0, 0) + " = " + v + ";");
        ZeroUpper(0);
      }
      return;
    }
    case ZYDIS_MNEMONIC_VPERM2F128: {
      const unsigned imm = imm8(3);
      std::string s2lo = HalfSrc(2, 0), s2hi = HalfSrc(2, 1);
      const std::string pick[4] = {Half(1, 0), Half(1, 1), s2lo, s2hi};
      std::string body = "{ const ::recomp::Xmm r0 = ";
      for (int h = 0; h < 2; h++) {
        unsigned ctl = imm >> (4 * h);
        if (h) body += ", r1 = ";
        body += (ctl & 8) ? "::recomp::Xmm{}" : pick[ctl & 3];
      }
      body += "; " + Half(0, 0) + " = r0; " + Half(0, 1) + " = r1; }";
      L(body);
      return;
    }
    case ZYDIS_MNEMONIC_VCVTDQ2PS: {
      if (!IsVec(0)) return Unimpl();
      for (int h = 0; h < halves; h++) {
        std::string s1 = HalfSrc(1, h);
        L(Fmt("::recomp::sse::Cvtdq2ps(c, %s, %s, %s);", Half(0, h).c_str(), s1.c_str(),
              va.c_str()));
      }
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VSQRTPS: case ZYDIS_MNEMONIC_VSQRTPD: {
      if (!IsVec(0)) return Unimpl();
      const bool dbl = m == ZYDIS_MNEMONIC_VSQRTPD;
      for (int h = 0; h < halves; h++) {
        std::string s1 = HalfSrc(1, h);
        L(Fmt("::recomp::sse::Sqrt(c, %s, %d, %s, %s, %s);", b(dbl), dbl ? 2 : 4,
              Half(0, h).c_str(), s1.c_str(), va.c_str()));
      }
      if (halves == 1) ZeroUpper(0);
      return;
    }
    case ZYDIS_MNEMONIC_VZEROUPPER:
      L("for (::recomp::Xmm& x : c.ymm_hi) x = ::recomp::Xmm{};");
      return;
    case ZYDIS_MNEMONIC_VZEROALL:
      L("for (::recomp::Xmm& x : c.ymm_hi) x = ::recomp::Xmm{};");
      L("for (int i = 0; i < 16; i++) c.xmm[i] = ::recomp::Xmm{};");
      return;
    default:
      return Unimpl();
  }
}

// x64 images. Everything that does not depend on the register model is
// shared with 32-bit code (EmitAlu, EmitShift, EmitLocked, SSE, x87);
// what differs is here: 8-byte stack slots, the 64-bit forms and the
// x64-only instructions, and the rule that 32-bit writes zero-extend.
void InsnEmitter::Emit64() {
  const auto m = d_.mnemonic;
  const uint64_t next = in_.next();
  const std::string va = Hex(in_.va);
  bool locked = (d_.attributes & ZYDIS_ATTRIB_HAS_LOCK) && Nvis() && IsMem(0);

  switch (m) {
    case ZYDIS_MNEMONIC_CALL: {
      if (IsImm(0)) {
        uint64_t t = RelTarget(in_);
        Push64(Hex(next));
        if (t == next) return;
        if (!p_.InText(t)) {
          L(Fmt("LIFT_SPILL; ::recomp::BadJump(c, %s, %s);", va.c_str(), Hex(t).c_str()));
          return;
        }
        refs_.insert(t);
        L(Fmt("LIFT_SPILL; %s(c); LIFT_RELOAD;", FnName(t).c_str()));
      } else {
        if (!PrepareMem()) return Unimpl();
        L("const uint64_t t = " + Read(0) + ";");
        Push64(Hex(next));
        L(Fmt("LIFT_SPILL; ::recomp::Dispatch(c, t, %s); LIFT_RELOAD;", va.c_str()));
      }
      uint64_t tt;
      if (Classify(p_, f_, in_, &tt) == Flow::Stop)
        L(Fmt("::recomp::Unreachable(c, %s);", va.c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_JMP: {
      if (IsImm(0)) {
        L(GotoOrTail(RelTarget(in_)));
        return;
      }
      if (!PrepareMem()) return Unimpl();
      L("const uint64_t t = " + Read(0) + ";");
      auto jt = p_.jumptables.find(in_.va);
      if (jt != p_.jumptables.end()) {
        L("switch (t) {");
        for (uint64_t tgt : jt->second)
          L(Fmt("  case %s: %s", Hex(tgt).c_str(), GotoOrTail(tgt).c_str()));
        L(Fmt("  default: LIFT_SPILL; ::recomp::BadJump(c, %s, t);", va.c_str()));
        L("}");
      } else {
        L(Fmt("LIFT_SPILL; ::recomp::Dispatch(c, t, %s); return;", va.c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_RET: {
      uint64_t extra = Nvis() ? Op(0).imm.value.u : 0;
      L(Fmt("rsp += %lluu; LIFT_SPILL; return;", U(8 + extra)));
      return;
    }
    case ZYDIS_MNEMONIC_JRCXZ:
      L(Fmt("if (rcx == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_JECXZ:
      L(Fmt("if (uint32_t(rcx) == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOP:
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE: {
      if (d_.address_width != 64) return Unimpl();
      const char* cond = m == ZYDIS_MNEMONIC_LOOPE    ? " && F.zf"
                         : m == ZYDIS_MNEMONIC_LOOPNE ? " && !F.zf"
                                                      : "";
      L(Fmt("rcx -= 1; if (rcx != 0%s) %s", cond, GotoOrTail(RelTarget(in_)).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_UD2:
      L(Fmt("LIFT_SPILL; ::recomp::Trap(c, %s); LIFT_RELOAD;", va.c_str()));
      return;
    default:
      break;
  }

  if (const char* cond = CondExpr(m)) {
    if (d_.meta.category == ZYDIS_CATEGORY_COND_BR) {
      L(Fmt("if (%s) %s", cond, GotoOrTail(RelTarget(in_)).c_str()));
      return;
    }
    if (!PrepareMem()) return Unimpl();
    if (d_.meta.category == ZYDIS_CATEGORY_SETCC) {
      L(Write(0, Fmt("(%s) ? 1u : 0u", cond)));
      return;
    }
    // CMOVcc: the source is read even when the condition is false, and a
    // 32-bit destination is zero-extended either way.
    int n = Bytes(0);
    L(Fmt("const %s v = %s;", VT(n), Read(1).c_str()));
    if (n == 4)
      L(Write(0, Fmt("(%s) ? v : %s", cond, Read(0).c_str())));
    else
      L(Fmt("if (%s) { %s }", cond, Write(0, "v").c_str()));
    return;
  }

  if (!PrepareMem()) return Unimpl();
  if (locked) return EmitLocked();

  const int n = Nvis() ? Bytes(0) : 0;
  switch (m) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
      return;
    case ZYDIS_MNEMONIC_MOV:
      L(Write(0, Read(1, n)));
      return;
    case ZYDIS_MNEMONIC_MOVZX:
      L(Write(0, Read(1)));
      return;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
      if (n == 8)
        L(Write(0, Fmt("uint64_t(int64_t(::recomp::lift::Sext<%d>(%s)))", Bytes(1),
                       Read(1).c_str())));
      else
        L(Write(0, Fmt("uint32_t(::recomp::lift::Sext<%d>(%s))", Bytes(1), Read(1).c_str())));
      return;
    case ZYDIS_MNEMONIC_LEA:
      L(Write(0, AddrExpr(1, false)));
      return;
    case ZYDIS_MNEMONIC_PUSH:
      if (n == 2 && !IsImm(0)) {
        L("const uint32_t v = " + Read(0) + ";");
        L("rsp -= 2; ::recomp::lift::Wr<uint16_t>(B, rsp, uint16_t(v));");
      } else {
        L("const uint64_t v = " + Read(0, 8) + ";");
        Push64("v");
      }
      return;
    case ZYDIS_MNEMONIC_POP:
      if (n == 2) return Unimpl();
      // A memory destination's address uses RSP after the increment.
      L("const uint64_t v = ::recomp::lift::Rd<uint64_t>(B, rsp); rsp += 8;");
      if (IsMem(0))
        L(Fmt("::recomp::lift::Wr<uint64_t>(B, %s, v);", AddrExpr(0).c_str()));
      else
        L(Write(0, "v"));
      return;
    case ZYDIS_MNEMONIC_PUSHFQ:
      Push64("::recomp::PackEFlags(F)");
      return;
    case ZYDIS_MNEMONIC_POPFQ:
      L("::recomp::UnpackEFlags(F, uint32_t(::recomp::lift::Rd<uint64_t>(B, rsp))); rsp += 8;");
      return;
    case ZYDIS_MNEMONIC_XCHG:
      if (IsMem(0) || IsMem(1)) {
        int mi = IsMem(0) ? 0 : 1, ri = 1 - mi;
        const char* t = MemType(Bytes(mi));
        L(Fmt("const %s v = __atomic_exchange_n(::recomp::lift::AtomicPtr<%s>(B, m%d), "
              "%s(%s), __ATOMIC_SEQ_CST);", VT(n), t, mi, t, Read(ri).c_str()));
        L(Write(ri, "v"));
      } else {
        L(Fmt("const %s a = %s, b = %s;", VT(n), Read(0).c_str(), Read(1).c_str()));
        L(Write(0, "b"));
        L(Write(1, "a"));
      }
      return;
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_NOT:
      return EmitAlu();
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR:
      return EmitShift();
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
      L(Fmt("const %s r = ::recomp::lift::%s<%d>(F, %s, %s, %s);", VT(n),
            m == ZYDIS_MNEMONIC_SHLD ? "Shld" : "Shrd", n, Read(0).c_str(), Read(1).c_str(),
            Read(2, 1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_IMUL:
      if (Nvis() > 1) {
        std::string a = Nvis() == 3 ? Read(1) : Read(0);
        std::string b = Nvis() == 3 ? Read(2, n) : Read(1, n);
        L(Fmt("const %s r = ::recomp::lift::Imul2<%d>(F, %s, %s);", VT(n), n, a.c_str(),
              b.c_str()));
        L(Write(0, "r"));
        return;
      }
      [[fallthrough]];
    case ZYDIS_MNEMONIC_MUL: {  // one operand: rdx:rax (edx:eax, ax) = acc * r/m
      const char* fn = m == ZYDIS_MNEMONIC_MUL ? "Mul1" : "Imul1";
      L(Fmt("%s lo, hi; ::recomp::lift::%s<%d>(F, %s, %s, lo, hi);", VT(n), fn, n,
            ReadReg(Acc(n)).c_str(), Read(0).c_str()));
      if (n == 1) {
        L(WriteReg(ZYDIS_REGISTER_AX, "lo | (hi << 8)"));
      } else {
        L(WriteReg(Acc(n), "lo"));
        L(WriteReg(n == 8 ? ZYDIS_REGISTER_RDX : n == 4 ? ZYDIS_REGISTER_EDX
                                                         : ZYDIS_REGISTER_DX,
                   "hi"));
      }
      return;
    }
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV: {
      const char* fn = m == ZYDIS_MNEMONIC_DIV ? "Div" : "Idiv";
      std::string fail =
          Fmt("{ LIFT_SPILL; ::recomp::RaiseDivideError(c, %s); LIFT_RELOAD; }", va.c_str());
      if (n == 1) {
        L("uint32_t lo = uint32_t(rax) & 0xFFu, hi = (uint32_t(rax) >> 8) & 0xFFu;");
        L(Fmt("if (::recomp::lift::%s<1>(lo, hi, %s)) { %s } else %s", fn, Read(0).c_str(),
              WriteReg(ZYDIS_REGISTER_AX, "lo | (hi << 8)").c_str(), fail.c_str()));
      } else {
        ZydisRegister hreg = n == 8   ? ZYDIS_REGISTER_RDX
                             : n == 4 ? ZYDIS_REGISTER_EDX
                                      : ZYDIS_REGISTER_DX;
        L(Fmt("%s lo = %s, hi = %s;", VT(n), ReadReg(Acc(n)).c_str(), ReadReg(hreg).c_str()));
        L(Fmt("if (::recomp::lift::%s<%d>(lo, hi, %s)) { %s %s } else %s", fn, n,
              Read(0).c_str(), WriteReg(Acc(n), "lo").c_str(), WriteReg(hreg, "hi").c_str(),
              fail.c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_CBW:
      L(WriteReg(ZYDIS_REGISTER_AX, "uint32_t(int32_t(int8_t(rax)))"));
      return;
    case ZYDIS_MNEMONIC_CWDE:
      L(WriteReg(ZYDIS_REGISTER_EAX, "uint32_t(int32_t(int16_t(rax)))"));
      return;
    case ZYDIS_MNEMONIC_CDQE:
      L("rax = uint64_t(int64_t(int32_t(rax)));");
      return;
    case ZYDIS_MNEMONIC_CWD:
      L(WriteReg(ZYDIS_REGISTER_DX, "(rax & 0x8000u) ? 0xFFFFu : 0u"));
      return;
    case ZYDIS_MNEMONIC_CDQ:
      L(WriteReg(ZYDIS_REGISTER_EDX, "uint32_t(int32_t(rax) >> 31)"));
      return;
    case ZYDIS_MNEMONIC_CQO:
      L("rdx = uint64_t(int64_t(rax) >> 63);");
      return;
    case ZYDIS_MNEMONIC_LEAVE:
      L("rsp = rbp; rbp = ::recomp::lift::Rd<uint64_t>(B, rsp); rsp += 8;");
      return;
    case ZYDIS_MNEMONIC_ENTER: {
      uint64_t size = Op(0).imm.value.u;
      if (Op(1).imm.value.u & 31) return Unimpl();
      Push64("rbp");
      L(Fmt("rbp = rsp; rsp -= %lluu;", U(size)));
      return;
    }
    case ZYDIS_MNEMONIC_CLD: L("F.df = false;"); return;
    case ZYDIS_MNEMONIC_STD: L("F.df = true;"); return;
    case ZYDIS_MNEMONIC_CLC: L("F.cf = false;"); return;
    case ZYDIS_MNEMONIC_STC: L("F.cf = true;"); return;
    case ZYDIS_MNEMONIC_CMC: L("F.cf = !F.cf;"); return;
    case ZYDIS_MNEMONIC_SAHF: L("::recomp::lift::Sahf(F, (uint32_t(rax) >> 8) & 0xFFu);"); return;
    case ZYDIS_MNEMONIC_LAHF: L(WriteReg(ZYDIS_REGISTER_AH, "::recomp::lift::Lahf(F)")); return;
    case ZYDIS_MNEMONIC_BSWAP:
      L(Write(0, Fmt("__builtin_bswap%d(%s)", n * 8, Read(0).c_str())));
      return;
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR:
      L(Fmt("const %s r = ::recomp::lift::%s<%d>(F, %s, %s);", VT(n),
            m == ZYDIS_MNEMONIC_BSF ? "Bsf" : "Bsr", n, Read(0).c_str(), Read(1).c_str()));
      // An all-zero source (ZF) leaves the whole destination unchanged: a
      // 32-bit one is not zero-extended then.
      if (n == 4)
        L("if (!F.zf) { " + Write(0, "r") + " }");
      else
        L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
      const char* one = n == 8 ? "1ull" : "1u";
      // A memory operand with a register bit offset can address outside
      // the operand: the offset is signed and selects a byte relative to it.
      if (IsMem(0) && IsReg(1)) {
        L(Fmt("const int64_t off = int64_t(%s(%s));", n == 8 ? "int64_t" : "int32_t",
              Read(1).c_str()));
        L("const uint64_t a = m0 + uint64_t(off >> 3);");
        L("const uint32_t bit = uint32_t(off) & 7u;");
        L("uint32_t v = ::recomp::lift::Rd<uint8_t>(B, a);");
        one = "1u";
      } else {
        L(Fmt("const uint32_t bit = %s & %du;", Read(1).c_str(), n * 8 - 1));
        L(Fmt("%s v = %s;", VT(n), Read(0).c_str()));
      }
      L("F.cf = (v >> bit) & 1u;");
      std::string upd = m == ZYDIS_MNEMONIC_BTS   ? Fmt("v |= %s << bit;", one)
                        : m == ZYDIS_MNEMONIC_BTR ? Fmt("v &= ~(%s << bit);", one)
                        : m == ZYDIS_MNEMONIC_BTC ? Fmt("v ^= %s << bit;", one)
                                                  : "";
      if (!upd.empty()) {
        L(upd);
        if (IsMem(0) && IsReg(1))
          L("::recomp::lift::Wr<uint8_t>(B, a, uint8_t(v));");
        else
          L(Write(0, "v"));
      }
      return;
    }
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_TZCNT:
      L(Fmt("const %s r = ::recomp::lift::%s<%d>(F, %s);", VT(n),
            m == ZYDIS_MNEMONIC_LZCNT ? "Lzcnt" : "Tzcnt", n, Read(1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_PREFETCHW:
      return;  // a hint
    case ZYDIS_MNEMONIC_CMPXCHG:
      // Not locked: one compare, then either store (the destination) or
      // load (the accumulator).
      L(Fmt("const %s d = %s;", VT(n), Read(0).c_str()));
      L(Fmt("(void)::recomp::lift::Sub<%d>(F, %s, d);", n, ReadReg(Acc(n)).c_str()));
      L("if (F.zf) { " + Write(0, Read(1)) + " } else { " + WriteReg(Acc(n), "d") + " }");
      return;
    case ZYDIS_MNEMONIC_XADD:
      L(Fmt("const %s a = %s, b = %s;", VT(n), Read(0).c_str(), Read(1).c_str()));
      L(Fmt("const %s r = ::recomp::lift::Add<%d>(F, a, b);", VT(n), n));
      L(Write(1, "a"));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_XLAT:
      L(WriteReg(ZYDIS_REGISTER_AL,
                 "::recomp::lift::Rd<uint8_t>(B, rbx + (rax & 0xFFu))"));
      return;
    case ZYDIS_MNEMONIC_CPUID:
      L("LIFT_SPILL; ::recomp::Cpuid(c); LIFT_RELOAD;");
      return;
    case ZYDIS_MNEMONIC_XGETBV:
      L("LIFT_SPILL; ::recomp::Xgetbv(c); LIFT_RELOAD;");
      return;
    case ZYDIS_MNEMONIC_RDTSC:
      L("{ LIFT_SPILL; const uint64_t t = ::recomp::Rdtsc(c); rax = uint32_t(t); "
        "rdx = t >> 32; }");
      return;
    default:
      break;
  }
  Unimpl();
}

void InsnEmitter::Emit() {
  const auto m = d_.mnemonic;
  const uint64_t next = in_.next();

  if (IsX87(d_)) {
    if (!PrepareMem()) return Unimpl();
    return EmitX87();
  }
  if (d_.meta.isa_set == ZYDIS_ISA_SET_AVX) return EmitAvx();
  if (IsMmxInsn()) return EmitMmx();
  if (IsSse(d_)) return EmitSse();
  if (d_.meta.category == ZYDIS_CATEGORY_STRINGOP) return EmitString();

  // Far pointers and other operand widths we don't model (only seen in
  // data that merely decodes as code) become traps.
  for (int i = 0; i < Nvis(); i++)
    if (IsMem(i) && m != ZYDIS_MNEMONIC_LEA && !MemType(Bytes(i)))
      return Unimpl();

  bool locked = (d_.attributes & ZYDIS_ATTRIB_HAS_LOCK) && Nvis() && IsMem(0);

  if (X64()) return Emit64();

  if ((m == ZYDIS_MNEMONIC_CALL || m == ZYDIS_MNEMONIC_JMP) &&
      p_.resume_sites.count(in_.va)) {
    // The target abandons this call chain (an SEH __except block that
    // starts with `mov esp, [ebp-18h]`, or longjmp's jump to a setjmp
    // return point with that caller's registers restored): unwind the host
    // stack to the lifted function owning the frame instead of calling.
    if (!PrepareMem()) return Unimpl();
    L("const uint32_t t = " + Read(0) + ";");
    L(Fmt("LIFT_SPILL; throw ::recomp::GuestResume{t, ebp, %s};",
          Hex(in_.va).c_str()));
    return;
  }

  switch (m) {
    // ---- control flow ---------------------------------------------------
    case ZYDIS_MNEMONIC_CALL: {
      if (IsImm(0)) {
        uint64_t t = RelTarget(in_);
        Push32(Hex(next));
        if (t == next) return;  // call $+5; pop reg: reads EIP
        if (!p_.InText(t)) {
          L(Fmt("LIFT_SPILL; ::recomp::BadJump(c, %s, %s);", Hex(in_.va).c_str(),
                Hex(t).c_str()));
          return;
        }
        refs_.insert(t);
        L(Fmt("LIFT_SPILL; %s(c); LIFT_RELOAD;", FnName(t).c_str()));
      } else {
        if (!PrepareMem()) return Unimpl();
        L("const uint32_t t = " + Read(0) + ";");
        Push32(Hex(next));
        L(Fmt("LIFT_SPILL; ::recomp::Dispatch(c, t, %s); LIFT_RELOAD;",
              Hex(in_.va).c_str()));
      }
      uint64_t tt;
      if (Classify(p_, f_, in_, &tt) == Flow::Stop)
        L(Fmt("::recomp::Unreachable(c, %s);", Hex(in_.va).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_JMP: {
      if (IsImm(0)) {
        L(GotoOrTail(RelTarget(in_)));
        return;
      }
      if (!PrepareMem()) return Unimpl();
      L("const uint32_t t = " + Read(0) + ";");
      auto jt = p_.jumptables.find(in_.va);
      if (jt != p_.jumptables.end()) {
        L("switch (t) {");
        for (uint64_t tgt : jt->second)
          L(Fmt("  case %s: %s", Hex(tgt).c_str(), GotoOrTail(tgt).c_str()));
        L(Fmt("  default: LIFT_SPILL; ::recomp::BadJump(c, %s, t);",
              Hex(in_.va).c_str()));
        L("}");
      } else {
        L(Fmt("LIFT_SPILL; ::recomp::Dispatch(c, t, %s); return;",
              Hex(in_.va).c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_RET: {
      uint32_t extra = Nvis() ? static_cast<uint32_t>(Op(0).imm.value.u) : 0;
      L(Fmt("esp += %uu; LIFT_SPILL; return;", 4 + extra));
      return;
    }
    case ZYDIS_MNEMONIC_JECXZ:
      L(Fmt("if (ecx == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_JCXZ:
      L(Fmt("if ((ecx & 0xFFFFu) == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOP:
      L(Fmt("ecx -= 1; if (ecx != 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOPE:
      L(Fmt("ecx -= 1; if (ecx != 0 && F.zf) %s",
            GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOPNE:
      L(Fmt("ecx -= 1; if (ecx != 0 && !F.zf) %s",
            GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_UD2:
      L(Fmt("LIFT_SPILL; ::recomp::Trap(c, %s); LIFT_RELOAD;", Hex(in_.va).c_str()));
      return;
    default:
      break;
  }

  if (const char* cond = CondExpr(m)) {
    if (d_.meta.category == ZYDIS_CATEGORY_COND_BR) {
      L(Fmt("if (%s) %s", cond, GotoOrTail(RelTarget(in_)).c_str()));
      return;
    }
    if (!PrepareMem()) return Unimpl();
    if (d_.meta.category == ZYDIS_CATEGORY_SETCC) {
      L(Write(0, Fmt("(%s) ? 1u : 0u", cond)));
      return;
    }
    // CMOVcc: the source is read even when the condition is false.
    L("const uint32_t v = " + Read(1) + ";");
    L(Fmt("if (%s) { %s }", cond, Write(0, "v").c_str()));
    return;
  }

  if (!PrepareMem()) return Unimpl();
  if (locked) return EmitLocked();

  switch (m) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
      return;
    case ZYDIS_MNEMONIC_MOV:
      L(Write(0, Read(1, Bytes(0))));
      return;
    case ZYDIS_MNEMONIC_MOVZX:
      L(Write(0, Read(1)));
      return;
    case ZYDIS_MNEMONIC_MOVSX:
      L(Write(0, Fmt("uint32_t(::recomp::lift::Sext<%d>(%s))", Bytes(1),
                     Read(1).c_str())));
      return;
    case ZYDIS_MNEMONIC_LEA:
      L(Write(0, AddrExpr(1, false)));
      return;
    case ZYDIS_MNEMONIC_PUSH: {
      L("const uint32_t v = " + Read(0, 4) + ";");
      if (Bytes(0) == 2 && !IsImm(0)) {
        L("esp -= 2; ::recomp::lift::Wr<uint16_t>(B, esp, uint16_t(v));");
      } else {
        Push32("v");
      }
      return;
    }
    case ZYDIS_MNEMONIC_POP: {
      // A memory destination's address uses ESP after the increment, so
      // recompute it here instead of using the pre-computed m0.
      L("const uint32_t v = ::recomp::lift::Rd<uint32_t>(B, esp); esp += 4;");
      if (IsMem(0))
        L(Fmt("::recomp::lift::Wr<uint32_t>(B, %s, v);", AddrExpr(0).c_str()));
      else
        L(Write(0, "v"));
      return;
    }
    case ZYDIS_MNEMONIC_PUSHAD:
      L("const uint32_t s = esp;");
      for (const char* r : {"eax", "ecx", "edx", "ebx", "s", "ebp", "esi", "edi"})
        Push32(r);
      return;
    case ZYDIS_MNEMONIC_POPAD:
      L("edi = ::recomp::lift::Rd<uint32_t>(B, esp); esi = ::recomp::lift::Rd<uint32_t>(B, esp + 4);");
      L("ebp = ::recomp::lift::Rd<uint32_t>(B, esp + 8); ebx = ::recomp::lift::Rd<uint32_t>(B, esp + 16);");
      L("edx = ::recomp::lift::Rd<uint32_t>(B, esp + 20); ecx = ::recomp::lift::Rd<uint32_t>(B, esp + 24);");
      L("eax = ::recomp::lift::Rd<uint32_t>(B, esp + 28); esp += 32;");
      return;
    // 16-bit forms (66 60 / 66 61): word pushes; POPA writes only the low
    // halves, so the upper 16 bits of every register survive.
    case ZYDIS_MNEMONIC_PUSHA:
      L("const uint32_t s = esp;");
      for (const char* r : {"eax", "ecx", "edx", "ebx", "s", "ebp", "esi", "edi"})
        L(Fmt("esp -= 2; ::recomp::lift::Wr<uint16_t>(B, esp, uint16_t(%s));", r));
      return;
    case ZYDIS_MNEMONIC_POPA: {
      int off = 0;
      for (const char* r : std::initializer_list<const char*>{
               "edi", "esi", "ebp", nullptr, "ebx", "edx", "ecx", "eax"}) {
        if (r)
          L(Fmt("%s = ::recomp::lift::SetLo16(%s, ::recomp::lift::Rd<uint16_t>(B, esp + %du));",
                r, r, off));
        off += 2;
      }
      L("esp += 16;");
      return;
    }
    case ZYDIS_MNEMONIC_PUSHFD:
      Push32("::recomp::PackEFlags(F)");
      return;
    case ZYDIS_MNEMONIC_POPFD:
      L("::recomp::UnpackEFlags(F, ::recomp::lift::Rd<uint32_t>(B, esp)); esp += 4;");
      return;
    case ZYDIS_MNEMONIC_XCHG:
      if (IsMem(0) || IsMem(1)) {
        int mi = IsMem(0) ? 0 : 1, ri = 1 - mi;
        const char* t = MemType(Bytes(mi));
        L(Fmt("const uint32_t v = __atomic_exchange_n(::recomp::lift::AtomicPtr<%s>(B, m%d), %s(%s), __ATOMIC_SEQ_CST);",
              t, mi, t, Read(ri).c_str()));
        L(Write(ri, "v"));
      } else {
        L("const uint32_t a = " + Read(0) + ", b = " + Read(1) + ";");
        L(Write(0, "b"));
        L(Write(1, "a"));
      }
      return;
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_NOT:
      return EmitAlu();
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR:
      return EmitShift();
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
      L(Fmt("const uint32_t r = ::recomp::lift::%s<%d>(F, %s, %s, %s);",
            m == ZYDIS_MNEMONIC_SHLD ? "Shld" : "Shrd", Bytes(0),
            Read(0).c_str(), Read(1).c_str(), Read(2, 1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_IMUL:
      if (Nvis() > 1) {
        int n = Bytes(0);
        std::string a = Nvis() == 3 ? Read(1) : Read(0);
        std::string b = Nvis() == 3 ? Read(2, n) : Read(1, n);
        L(Fmt("const uint32_t r = ::recomp::lift::Imul2<%d>(F, %s, %s);", n,
              a.c_str(), b.c_str()));
        L(Write(0, "r"));
        return;
      }
      [[fallthrough]];
    case ZYDIS_MNEMONIC_MUL: {  // one operand: edx:eax (ax) = acc * r/m
      int n = Bytes(0);
      const char* fn = m == ZYDIS_MNEMONIC_MUL ? "Mul1" : "Imul1";
      if (n == 1) {
        L(Fmt("uint32_t lo, hi; ::recomp::lift::%s<1>(F, eax, %s, lo, hi);", fn, Read(0).c_str()));
        L("eax = ::recomp::lift::SetLo16(eax, lo | (hi << 8));");
      } else if (n == 2) {
        L(Fmt("uint32_t lo, hi; ::recomp::lift::%s<2>(F, eax, %s, lo, hi);", fn, Read(0).c_str()));
        L("eax = ::recomp::lift::SetLo16(eax, lo); edx = ::recomp::lift::SetLo16(edx, hi);");
      } else {
        L(Fmt("::recomp::lift::%s<4>(F, eax, %s, eax, edx);", fn, Read(0).c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV: {
      int n = Bytes(0);
      const char* fn = m == ZYDIS_MNEMONIC_DIV ? "Div" : "Idiv";
      std::string fail = Fmt("{ LIFT_SPILL; ::recomp::RaiseDivideError(c, %s); LIFT_RELOAD; }",
                             Hex(in_.va).c_str());
      if (n == 1) {
        L("uint32_t lo = eax & 0xFFu, hi = (eax >> 8) & 0xFFu;");
        L(Fmt("if (::recomp::lift::%s<1>(lo, hi, %s)) eax = ::recomp::lift::SetLo16(eax, lo | (hi << 8)); else %s",
              fn, Read(0).c_str(), fail.c_str()));
      } else if (n == 2) {
        L("uint32_t lo = eax & 0xFFFFu, hi = edx & 0xFFFFu;");
        L(Fmt("if (::recomp::lift::%s<2>(lo, hi, %s)) { eax = ::recomp::lift::SetLo16(eax, lo); edx = ::recomp::lift::SetLo16(edx, hi); } else %s",
              fn, Read(0).c_str(), fail.c_str()));
      } else {
        L(Fmt("uint32_t lo = eax, hi = edx; if (::recomp::lift::%s<4>(lo, hi, %s)) { eax = lo; edx = hi; } else %s",
              fn, Read(0).c_str(), fail.c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_CDQ:
      L("edx = uint32_t(int32_t(eax) >> 31);");
      return;
    case ZYDIS_MNEMONIC_CWDE:
      L("eax = uint32_t(int32_t(int16_t(eax)));");
      return;
    case ZYDIS_MNEMONIC_CBW:
      L("eax = ::recomp::lift::SetLo16(eax, uint32_t(int32_t(int8_t(eax))));");
      return;
    case ZYDIS_MNEMONIC_CWD:
      L("edx = ::recomp::lift::SetLo16(edx, (eax & 0x8000u) ? 0xFFFFu : 0u);");
      return;
    case ZYDIS_MNEMONIC_LEAVE:
      L("esp = ebp; ebp = ::recomp::lift::Rd<uint32_t>(B, esp); esp += 4;");
      return;
    case ZYDIS_MNEMONIC_ENTER: {
      uint32_t size = static_cast<uint32_t>(Op(0).imm.value.u);
      uint32_t level = static_cast<uint32_t>(Op(1).imm.value.u) & 31;
      if (level) return Unimpl();
      Push32("ebp");
      L(Fmt("ebp = esp; esp -= %uu;", size));
      return;
    }
    case ZYDIS_MNEMONIC_CLD: L("F.df = false;"); return;
    case ZYDIS_MNEMONIC_STD: L("F.df = true;"); return;
    case ZYDIS_MNEMONIC_CLC: L("F.cf = false;"); return;
    case ZYDIS_MNEMONIC_STC: L("F.cf = true;"); return;
    case ZYDIS_MNEMONIC_CMC: L("F.cf = !F.cf;"); return;
    case ZYDIS_MNEMONIC_SAHF: L("::recomp::lift::Sahf(F, (eax >> 8) & 0xFFu);"); return;
    case ZYDIS_MNEMONIC_LAHF: L("eax = ::recomp::lift::SetHi8(eax, ::recomp::lift::Lahf(F));"); return;
    case ZYDIS_MNEMONIC_BSWAP:
      L(Write(0, "__builtin_bswap32(" + Read(0) + ")"));
      return;
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR:
      L(Fmt("const uint32_t r = ::recomp::lift::%s<%d>(F, %s, %s);",
            m == ZYDIS_MNEMONIC_BSF ? "Bsf" : "Bsr", Bytes(0), Read(0).c_str(),
            Read(1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
      int n = Bytes(0);
      // Memory operand with a register bit offset can address outside the
      // operand: the offset is signed and selects a byte relative to it.
      if (IsMem(0) && IsReg(1)) {
        L(Fmt("const int32_t off = int32_t(%s);", Read(1).c_str()));
        L("const uint32_t a = m0 + uint32_t(off >> 3);");
        L("const uint32_t bit = uint32_t(off) & 7u;");
        L("uint32_t v = ::recomp::lift::Rd<uint8_t>(B, a);");
      } else {
        L(Fmt("const uint32_t bit = %s & %du;", Read(1).c_str(), n * 8 - 1));
        L("uint32_t v = " + Read(0) + ";");
      }
      L("F.cf = (v >> bit) & 1u;");
      const char* upd = m == ZYDIS_MNEMONIC_BTS   ? "v |= 1u << bit;"
                        : m == ZYDIS_MNEMONIC_BTR ? "v &= ~(1u << bit);"
                        : m == ZYDIS_MNEMONIC_BTC ? "v ^= 1u << bit;"
                                                  : nullptr;
      if (upd) {
        L(upd);
        if (IsMem(0) && IsReg(1))
          L("::recomp::lift::Wr<uint8_t>(B, a, uint8_t(v));");
        else
          L(Write(0, "v"));
      }
      return;
    }
    case ZYDIS_MNEMONIC_CMPXCHG: {
      int n = Bytes(0);
      L("const uint32_t d = " + Read(0) + ";");
      L(Fmt("(void)::recomp::lift::Sub<%d>(F, %s, d);", n, ReadReg(Acc(n)).c_str()));
      L("if (F.zf) { " + Write(0, Read(1)) + " } else { " + WriteReg(Acc(n), "d") + " }");
      return;
    }
    case ZYDIS_MNEMONIC_XADD:
      L(Fmt("const uint32_t a = %s, b = %s;", Read(0).c_str(), Read(1).c_str()));
      L(Fmt("const uint32_t r = ::recomp::lift::Add<%d>(F, a, b);", Bytes(0)));
      L(Write(1, "a"));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_XLAT:
      L("eax = ::recomp::lift::SetLo8(eax, ::recomp::lift::Rd<uint8_t>(B, ebx + (eax & 0xFFu)));");
      return;
    case ZYDIS_MNEMONIC_CPUID:
      L("LIFT_SPILL; ::recomp::Cpuid(c); LIFT_RELOAD;");
      return;
    case ZYDIS_MNEMONIC_RDTSC:
      L("{ LIFT_SPILL; const uint64_t t = ::recomp::Rdtsc(c); eax = uint32_t(t); edx = uint32_t(t >> 32); }");
      return;
    default:
      break;
  }
  Unimpl();
}

}  // namespace

std::string FnName(uint64_t va) { return Fmt("f_%08llX", U(va)); }

void Emitter::EmitFunction(const Function& f, std::string* out,
                           std::set<uint64_t>* refs, EmitStats* stats) {
  std::string& o = *out;
  auto nm = p_.names.find(f.entry);
  o += "// " + (nm != p_.names.end() ? nm->second : FnName(f.entry)) + "\n";
  // A replaced function keeps its lifted body under another name, for the
  // replacement to call or be diffed against; every call site still
  // reaches f_X, which the game project defines.
  o += "void " + FnName(f.entry) + (p_.replaced.count(f.entry) ? "_lifted" : "") +
       "(::recomp::Ctx& c) {\n  LIFT_ENTER\n";
  // Functions that register an SEH frame (write fs:[0]) can be resumed into
  // by their __except blocks, and setjmp callers by longjmp; see
  // GuestResume.
  bool resumable = !f.setjmp_returns.empty();
  // x64: a function with a language handler; its establisher frame (EF) is
  // known once its prologue has run.
  auto eh = p_.x64 ? p_.eh_frames.find(f.entry) : p_.eh_frames.end();
  if (eh != p_.eh_frames.end()) resumable = true;
  for (const auto& [va, in] : f.insns) {
    if (!in.ok || in.d.mnemonic != ZYDIS_MNEMONIC_MOV) continue;
    const auto& o0 = in.ops[0];
    if (o0.type == ZYDIS_OPERAND_TYPE_MEMORY &&
        o0.mem.segment == ZYDIS_REGISTER_FS &&
        o0.mem.base == ZYDIS_REGISTER_NONE &&
        o0.mem.index == ZYDIS_REGISTER_NONE && o0.mem.disp.value == 0)
      resumable = true;
  }
  t_in_try = resumable;
  if (eh != p_.eh_frames.end()) o += "  uint64_t EF = 0;\n";
  if (resumable) o += "  try {\n";
  // Instructions are emitted in address order, so if the function also
  // reaches code below its entry (shared tails, e.g. strstr jumping into
  // strchr), execution must start with a jump to the entry.
  if (!f.insns.empty() && f.insns.begin()->first != f.entry)
    o += Fmt("  goto L_%08llX;\n", U(f.entry));
  auto pr = p_.probes.find(f.entry);
  if (pr != p_.probes.end())
    o += Fmt("  LIFT_SPILL; ::recomp::Probe(c, 0x%llXu, \"%s\"); LIFT_RELOAD;\n",
             U(f.entry), pr->second.c_str());

  // Labels: branch targets plus non-contiguous fallthroughs.
  std::set<uint64_t> labels = f.labels;
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    auto nx = std::next(it);
    uint64_t t;
    Flow fl = Classify(p_, f, it->second, &t);
    if ((fl == Flow::Fall || fl == Flow::Branch) &&
        (nx == f.insns.end() || nx->first != it->second.next()))
      labels.insert(it->second.next());
  }

  auto runs = FindX87Runs(f, labels);

  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    const Insn& in = it->second;
    if (labels.count(in.va)) o += Fmt("L_%08llX:\n", U(in.va));
    if (eh != p_.eh_frames.end() && in.va == eh->second.prolog_end) {
      const auto& e = eh->second;
      static const char* kRegs[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
      o += e.frame_reg ? Fmt("  EF = %s - 0x%Xu;\n", kRegs[e.frame_reg], 16u * e.frame_off)
                       : std::string("  EF = rsp;\n");
    }
    auto run = runs.find(in.va);
    if (run != runs.end()) {
      auto last = f.insns.find(run->second);
      EmitX87Run(f, it, last, &o, refs, stats);
      it = last;
    } else {
      stats->insns++;
      o += Fmt("  { // %08llX %s\n", U(in.va), dec_.Format(in).c_str());
      if (!in.ok) {
        o += Fmt("    LIFT_SPILL; ::recomp::Unimplemented(c, 0x%llXu, \"undecodable\");\n",
                 U(in.va));
        stats->unimplemented["(undecodable)"]++;
      } else {
        InsnEmitter(p_, f, in, &o, refs, stats).Emit();
      }
      o += "  }\n";
    }

    // Continue to the next instruction if it isn't laid out next.
    const Insn& done = it->second;
    uint64_t t;
    Flow fl = Classify(p_, f, done, &t);
    if (fl == Flow::Fall || fl == Flow::Branch) {
      uint64_t nx = done.next();
      auto ni = std::next(it);
      if (IsOtherEntry(p_, f, nx)) {
        refs->insert(nx);
        o += Fmt("  { LIFT_SPILL; %sreturn %s(c); }\n", TailAttr(),
                 FnName(nx).c_str());
      } else if (ni == f.insns.end() || ni->first != nx) {
        o += f.insns.count(nx)
                 ? Fmt("  goto L_%08llX;\n", U(nx))
                 : Fmt("  LIFT_SPILL; ::recomp::BadJump(c, 0x%llXu, 0x%llXu);\n",
                       U(done.va), U(nx));
      }
    }
  }
  if (resumable)
    o += std::string("  } catch (const ::recomp::GuestResume& r) {\n") +
         (p_.x64 ? "    if (r.ebp != EF) throw;  // another activation's frame\n"
                 : "    if (r.ebp != ebp) throw;  // another activation's frame\n") +
         "    ::recomp::Resume(c, r);\n"
         "    return;\n"
         "  }\n";
  o += "}\n\n";
}

// Non-x87 instructions that may sit inside an x87 run: anything straight-
// line that does not call out (the run's state is only in locals).
static bool RunNeutral(const Insn& in) {
  switch (in.d.meta.category) {
    case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSTEM:
      return false;
    default:
      return true;
  }
}

std::map<uint64_t, uint64_t> Emitter::FindX87Runs(const Function& f,
                                                  const std::set<uint64_t>& labels) const {
  std::map<uint64_t, uint64_t> runs;  // first -> last instruction
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    const Insn& first = it->second;
    if (!first.ok || first.d.meta.category != ZYDIS_CATEGORY_X87_ALU) continue;
    X87Sim sim;
    std::string scratch;
    std::set<uint64_t> refs;
    EmitStats st;
    int count = 0;
    auto last = f.insns.end();
    for (auto j = it; j != f.insns.end(); ++j) {
      const Insn& in = j->second;
      if (!in.ok || (j != it && labels.count(in.va))) break;
      uint64_t t;
      if (Classify(p_, f, in, &t) != Flow::Fall) break;
      if (IsX87(in.d)) {
        X87Sim trial = sim;
        if (!InsnEmitter(p_, f, in, &scratch, &refs, &st).EmitInRun(trial)) break;
        sim = trial;
        ++count;
        last = j;
      } else if (!RunNeutral(in)) {
        break;
      }
      auto nx = std::next(j);
      if (nx == f.insns.end() || nx->first != in.next() || p_.Splits(in.next())) break;
    }
    if (count >= 1) {
      runs[first.va] = last->first;
      it = last;
    }
  }
  return runs;
}

void Emitter::EmitX87Run(const Function& f, std::map<uint64_t, Insn>::const_iterator first,
                         std::map<uint64_t, Insn>::const_iterator last, std::string* out,
                         std::set<uint64_t>* refs, EmitStats* stats) const {
  std::string& o = *out;
  auto end = std::next(last);
  const uint64_t id = first->first;
  EmitStats fast_stats;  // the slow branch below counts the instructions

  // The run on host doubles, for runs with arithmetic: an instruction that
  // cannot be done exactly in doubles saves rk, the index of the
  // instruction in `resume`, and jumps to XS, which converts the doubles
  // to the x<k> locals and continues at that instruction (label XR) on the
  // exact path. The instructions before it are done by then.
  X87Sim dsim('d');
  std::string dbody;
  std::vector<uint64_t> resume;
  bool doubles = false;
  for (auto j = first; j != end; ++j)
    if (IsX87(j->second.d) && (X87OpName(j->second.d.mnemonic) ||
                               j->second.d.mnemonic == ZYDIS_MNEMONIC_FSQRT))
      doubles = true;
  for (auto j = first; doubles && j != end; ++j) {
    const Insn& in = j->second;
    dbody += Fmt("  { // %08llX %s\n", U(in.va), dec_.Format(in).c_str());
    std::string fail = Fmt("{ rk = %zu; goto XS_%08llX; }", resume.size(), U(id));
    bool can_fail;
    if (!InsnEmitter(p_, f, in, &dbody, refs, &fast_stats).EmitInRunDouble(dsim, fail, &can_fail))
      doubles = false;
    if (can_fail) resume.push_back(in.va);
    dbody += "  }\n";
  }

  X87Sim sim;
  std::string body;
  for (auto j = first; j != end; ++j) {
    const Insn& in = j->second;
    if (doubles && std::find(resume.begin(), resume.end(), in.va) != resume.end())
      body += Fmt("  XR_%08llX:;\n", U(in.va));
    body += Fmt("  { // %08llX %s\n", U(in.va), dec_.Format(in).c_str());
    InsnEmitter(p_, f, in, &body, refs, &fast_stats).EmitInRun(sim);
    body += "  }\n";
  }
  o += Fmt("  { // x87 run %08llX..%08llX\n", U(first->first), U(last->first));
  o += Fmt("  if (::recomp::x87::run::Guard(c.fpu, 0x%02Xu, 0x%02Xu)) {\n", sim.need_valid,
           sim.need_empty);
  o += "  const uint32_t rcw = c.fpu.cw, rtop = c.fpu.top;\n";
  o += "  uint32_t rfl = 0, rcc = 0; bool rc1 = false;\n";
  for (int k = sim.lo; k <= sim.hi; ++k) {
    if (sim.loaded.count(k))
      o += Fmt("  ::recomp::x87::run::V %s = ::recomp::x87::run::ToV(c.fpu.r[(rtop + %du) & 7u]);\n", sim.Var(k).c_str(),
               unsigned(k & 7));
    else if (sim.written.count(k))
      o += Fmt("  ::recomp::x87::run::V %s{};\n", sim.Var(k).c_str());
  }
  if (doubles) {
    std::string decl, to;
    for (int k = sim.lo; k <= sim.hi; ++k) {
      if (sim.loaded.count(k)) {
        decl += (decl.empty() ? "" : ", ") + dsim.Var(k);
        to += Fmt("%s::recomp::x87::run::ToD(%s, &%s)", to.empty() ? "" : " && ",
                  sim.Var(k).c_str(), dsim.Var(k).c_str());
      } else if (sim.written.count(k)) {
        decl += (decl.empty() ? "" : ", ") + dsim.Var(k) + " = 0";
      }
    }
    // The doubles the run writes, as registers (a register it only reads
    // never changes).
    std::string back;
    for (int k = sim.lo; k <= sim.hi; ++k)
      if (sim.written.count(k))
        back += Fmt("  %s = ::recomp::x87::run::FromD(%s);\n", sim.Var(k).c_str(),
                    dsim.Var(k).c_str());
    o += "  if (::recomp::x87::run::DoubleMode(rcw)) {\n";
    o += "  double " + decl + ";\n";
    o += to.empty() ? "  {\n" : "  if (" + to + ") {\n";
    if (!resume.empty()) o += "  int rk;\n";
    o += dbody;
    o += back;
    o += Fmt("  goto XE_%08llX;\n", U(id));
    if (!resume.empty()) {
      o += Fmt("  XS_%08llX:\n", U(id));
      o += back;
      o += "  switch (rk) {\n";
      for (size_t i = 0; i < resume.size(); ++i)
        o += i + 1 < resume.size() ? Fmt("    case %zu: goto XR_%08llX;\n", i, U(resume[i]))
                                   : Fmt("    default: goto XR_%08llX;\n", U(resume[i]));
      o += "  }\n";
    }
    o += "  }\n  }\n";
  }
  o += body;
  if (doubles) o += Fmt("  XE_%08llX:\n", U(id));
  // Write back: registers assigned in the run, the empty bits of the
  // slots it touched, TOP, and the status word (sticky flags, C1,
  // C3/C2/C0).
  uint32_t set = 0, clear = 0;  // entry-relative (k & 7)
  for (int k : sim.touched) {
    if (sim.written.count(k))
      o += Fmt("  c.fpu.r[(rtop + %du) & 7u] = ::recomp::x87::run::ToF80(%s);\n",
               unsigned(k & 7), sim.Var(k).c_str());
    (sim.slots[k] == X87Sim::Slot::Empty ? set : clear) |= 1u << (k & 7);
  }
  if (set | clear)
    o += Fmt("  c.fpu.empty = uint8_t((c.fpu.empty & ~::recomp::x87::run::Rotl8(0x%02Xu, rtop)) | "
             "::recomp::x87::run::Rotl8(0x%02Xu, rtop));\n", set | clear, set);
  o += Fmt("  c.fpu.top = uint8_t((rtop + %du) & 7u);\n", unsigned(sim.off & 7));
  o += Fmt("  c.fpu.sw = uint16_t(%s);\n", sim.StatusExpr().c_str());
  o += "  } else {\n";
  for (auto j = first; j != end; ++j) {
    const Insn& in = j->second;
    stats->insns++;
    o += Fmt("  { // %08llX %s\n", U(in.va), dec_.Format(in).c_str());
    InsnEmitter(p_, f, in, &o, refs, stats).Emit();
    o += "  }\n";
  }
  o += "  }\n  }\n";
}

}  // namespace lifter

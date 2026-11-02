// --x87-test: generates the inputs of tests/lift_x87_test.cpp. Every
// distinct x87 instruction form in the EXE is lifted on its own, next to
// its raw bytes, so the test can run both from the same FPU state and
// compare. Memory operands are rewritten to [eax] (ModRM mod=0 rm=0): the
// same bytes address [rax] when executed natively in 64-bit mode.

#include "x87test.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "emit.h"
#include "function.h"

namespace lifter {
namespace {

bool IsSegOrAddrPrefix(uint8_t b) {
  return b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
         b == 0x65 || b == 0x67;
}

// Appends raw[0, end) to *b without segment and address-size prefixes.
void CopyNoSegPrefixes(const uint8_t* raw, const ZydisDecodedInstruction& d, int end,
                       std::vector<uint8_t>* b) {
  for (int i = 0; i < end; i++)
    if (i >= d.raw.prefix_count || !IsSegOrAddrPrefix(raw[i])) b->push_back(raw[i]);
}

// Lifts each snippet (bytes, then ret) at its own `slot`-byte slot of a
// scratch program (x64 code at an address above 4 GiB, like its images).
// Returns the C++ (prologue, declarations, functions) and each snippet's
// address in *vas.
std::string LiftSnippets(const Decoder& dec, const char* prologue,
                         const std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>>& code,
                         std::vector<uint64_t>* vas, bool x64 = false) {
  Program q;
  q.x64 = x64;
  if (!q.mem.Reserve(x64 ? 36 : 32)) return {};
  const uint64_t base = x64 ? 0x140001000ull : 0x00401000;
  uint32_t size = 0x1000;
  for (const auto& [bytes, slot] : code) size += slot;
  q.mem.Commit(base, size, recomp::Prot::RWX);
  Module& m = q.modules.emplace_back();
  m.text_lo = base;
  m.text_hi = base + size;
  uint64_t va = base;
  for (const auto& [bytes, slot] : code) {
    for (size_t i = 0; i < bytes->size(); i++) q.mem.write<uint8_t>(va + i, (*bytes)[i]);
    q.mem.write<uint8_t>(va + bytes->size(), 0xC3);  // ret
    q.entries.insert(va);
    vas->push_back(va);
    va += slot;
  }
  Emitter em(q, dec);
  std::string body;
  std::set<uint64_t> refs;
  EmitStats st;
  for (uint64_t sva : *vas) {
    Function f;
    BuildFunction(q, dec, sva, &f);
    em.EmitFunction(f, &body, &refs, &st);
  }
  std::string o = prologue;
  char buf[64];
  for (uint64_t sva : *vas) {
    std::snprintf(buf, sizeof buf, "void f_%08llX(::recomp::Ctx& c);\n",
                  (unsigned long long)sva);
    o += buf;
  }
  return o + body + "}  // namespace recomp::gen\n\n";
}

// "0x12,0x34," and a disassembly safe inside a C string literal.
std::string HexBytes(const std::vector<uint8_t>& bytes) {
  std::string s;
  char h[8];
  for (uint8_t x : bytes) {
    std::snprintf(h, sizeof h, "0x%02X,", x);
    s += h;
  }
  return s;
}
std::string Literal(std::string text) {
  for (char& ch : text)
    if (ch == '"' || ch == '\\') ch = '\'';
  return text;
}

int WriteFile(const std::string& path, const std::string& s) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return 1;
  std::fwrite(s.data(), 1, s.size(), fp);
  std::fclose(fp);
  return 0;
}

struct Form {
  std::vector<uint8_t> bytes;
  std::string text;
  int mem_bytes = 0;
  uint64_t example_va = 0;
};

}  // namespace

int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  // Collect forms from every known function.
  std::map<std::vector<uint8_t>, Form> forms;
  for (uint64_t e : p.entries) {
    Function f;
    BuildFunction(p, dec, e, &f);
    for (const auto& [va, in] : f.insns) {
      if (!in.ok || in.d.meta.category != ZYDIS_CATEGORY_X87_ALU) continue;
      const auto m = in.d.mnemonic;
      // Environment images differ between 32- and 64-bit mode (pointer
      // fields), so they are not comparable natively.
      if (m == ZYDIS_MNEMONIC_FNSTENV || m == ZYDIS_MNEMONIC_FLDENV ||
          m == ZYDIS_MNEMONIC_FNSAVE || m == ZYDIS_MNEMONIC_FRSTOR)
        continue;
      const uint8_t* raw = p.Bytes(va);
      std::vector<uint8_t> b;
      int mem_bytes = 0;
      bool has_modrm = in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM;
      int modrm_off = has_modrm ? in.d.raw.modrm.offset : in.d.length;
      CopyNoSegPrefixes(raw, in.d, modrm_off, &b);
      if (has_modrm) {
        if (in.d.raw.modrm.mod == 3) {
          b.push_back(raw[modrm_off]);
        } else {
          b.push_back(uint8_t((in.d.raw.modrm.reg << 3) | 0));  // [eax]
          for (int i = 0; i < in.d.operand_count_visible; i++)
            if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
              mem_bytes = in.ops[i].size / 8;
        }
      }
      if (forms.count(b)) continue;
      Form fm;
      fm.bytes = b;
      fm.text = dec.Format(in);
      fm.mem_bytes = mem_bytes;
      fm.example_va = va;
      forms[b] = fm;
    }
  }

  // Sequences: random straight-line runs of forms, lifted as one block, so
  // the lifter's x87 runs (locals, one guard, write-back) are checked
  // against the CPU too. Transcendentals (1-ulp differences would compound)
  // and fldcw (could unmask exceptions, which trap natively) stay out.
  std::vector<const Form*> pool;
  for (const auto& [k, fm] : forms) {
    const std::string& t = fm.text;
    bool skip = false;
    for (const char* n : {"fldcw", "fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x"})
      if (t.rfind(n, 0) == 0) skip = true;
    if (!skip) pool.push_back(&fm);
  }
  std::vector<Form> seqs;
  std::mt19937 rng(20261011);
  for (int n = 0; n < 4000 && !pool.empty(); n++) {
    Form sq;
    int len = 3 + int(rng() % 8);
    for (int i = 0; i < len; i++) {
      const Form* fm = pool[rng() % pool.size()];
      // fnstsw ax rewrites the base of the [eax] operands: last only.
      while (i + 1 < len && fm->text.rfind("fnstsw ax", 0) == 0)
        fm = pool[rng() % pool.size()];
      sq.bytes.insert(sq.bytes.end(), fm->bytes.begin(), fm->bytes.end());
      sq.text += (i ? "; " : "") + fm->text;
      sq.mem_bytes = std::max(sq.mem_bytes, fm->mem_bytes);
    }
    seqs.push_back(sq);
  }

  // Forms in 16-byte slots, sequences in 128-byte ones.
  std::vector<const Form*> all;
  std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> code;
  for (const auto& [k, fm] : forms) {
    all.push_back(&fm);
    code.push_back({&fm.bytes, 16});
  }
  for (const auto& fm : seqs) {
    all.push_back(&fm);
    code.push_back({&fm.bytes, 128});
  }
  std::vector<uint64_t> vas;
  std::string o = LiftSnippets(dec, prologue, code, &vas);
  if (o.empty()) return 1;
  o += "#include \"lift_x87_forms.h\"\nextern const X87Form kX87Forms[] = {\n";
  for (size_t i = 0; i < all.size(); i++) {
    const Form& fm = *all[i];
    char buf[2048];
    std::snprintf(buf, sizeof buf, "  {&recomp::gen::f_%08llX, {%s}, %zu, %d, \"%s\", 0x%08llX},\n",
                  (unsigned long long)vas[i], HexBytes(fm.bytes).c_str(), fm.bytes.size(),
                  fm.mem_bytes, Literal(fm.text).c_str(), (unsigned long long)fm.example_va);
    o += buf;
  }
  o += "};\nextern const int kX87FormCount = " + std::to_string(all.size()) + ";\n";
  if (WriteFile(out, o)) return 1;
  std::printf("x87 forms: %zu (%zu sequences) -> %s\n", all.size(), seqs.size(),
              out.c_str());
  return 0;
}

// ---- integer forms ---------------------------------------------------------
//
// Same idea for integer instructions. Memory operands are rewritten to
// [R] (ModRM mod=0) with R a base register the instruction does not use
// otherwise; the test sets R to a buffer mapped at the same low address
// natively and in the guest arena, so every register value is identical
// in both runs. Excluded: stack and control flow, esp operands, segment
// overrides, and opcodes that mean something else in 64-bit mode.

namespace {

bool BadOperands(const Insn& in);

bool Excluded(const Insn& in) {
  // SSE/MMX forms (some are DATAXFER/LOGICAL, not the SSE category) and
  // FCMOVcc (CMOV category): the integer test compares only general
  // registers and memory.
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
      ZydisRegisterClass rc = ZydisRegisterGetClass(in.ops[i].reg.value);
      if (rc == ZYDIS_REGCLASS_XMM || rc == ZYDIS_REGCLASS_YMM ||
          rc == ZYDIS_REGCLASS_MMX || rc == ZYDIS_REGCLASS_X87)
        return true;
    }
  // SSE/SSE2 forms without an XMM operand (CVTTSD2SI r32, m64) too: they
  // belong to the SSE test.
  switch (in.d.meta.isa_set) {
    case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2:
    case ZYDIS_ISA_SET_SSEMXCSR: case ZYDIS_ISA_SET_SSE_PREFETCH:
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSE4:
    case ZYDIS_ISA_SET_AVX:
      return true;
    default:
      break;
  }
  switch (in.d.meta.category) {
    case ZYDIS_CATEGORY_X87_ALU: case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_PUSH: case ZYDIS_CATEGORY_POP:
    case ZYDIS_CATEGORY_SYSTEM: case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_IO: case ZYDIS_CATEGORY_IOSTRINGOP:
    case ZYDIS_CATEGORY_SEGOP: case ZYDIS_CATEGORY_MMX:
    case ZYDIS_CATEGORY_SSE:
      return true;
    case ZYDIS_CATEGORY_FLAGOP:  // only these are comparable natively
      switch (in.d.mnemonic) {
        case ZYDIS_MNEMONIC_SAHF: case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_CLC:
        case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_CLD:
        case ZYDIS_MNEMONIC_STD:
          return false;
        default:
          return true;
      }
    default:
      break;
  }
  switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_CPUID: case ZYDIS_MNEMONIC_RDTSC: case ZYDIS_MNEMONIC_LEAVE:
    case ZYDIS_MNEMONIC_ENTER: case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS:
    case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAS: case ZYDIS_MNEMONIC_AAM:
    case ZYDIS_MNEMONIC_AAD: case ZYDIS_MNEMONIC_BOUND: case ZYDIS_MNEMONIC_ARPL:
    case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LES: case ZYDIS_MNEMONIC_SALC:
    case ZYDIS_MNEMONIC_PUSHAD: case ZYDIS_MNEMONIC_POPAD: case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
    case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_FWAIT: case ZYDIS_MNEMONIC_XLAT: case ZYDIS_MNEMONIC_XGETBV:
      return true;
    default:
      break;
  }
  if (in.d.opcode == 0x82 && in.d.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT) return true;
  return BadOperands(in);
}

// Operands neither run can reproduce: esp, eip, segment registers, fs/gs
// memory, and lea forms that would change meaning in 64-bit mode.
bool BadOperands(const Insn& in) {
  for (int i = 0; i < in.d.operand_count; i++) {
    const auto& o = in.ops[i];
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
      ZydisRegister r = o.reg.value;
      ZydisRegister l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
      if (l == ZYDIS_REGISTER_RSP || l == ZYDIS_REGISTER_RIP ||
          ZydisRegisterGetClass(r) == ZYDIS_REGCLASS_SEGMENT)
        return true;
    }
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
      if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS)
        return true;
      // lea computes an address; it keeps its original operand, which must
      // not involve esp, nor be absolute (RIP-relative in 64-bit mode).
      if (in.d.mnemonic == ZYDIS_MNEMONIC_LEA &&
          (o.mem.base == ZYDIS_REGISTER_ESP || o.mem.index == ZYDIS_REGISTER_ESP ||
           o.mem.base == ZYDIS_REGISTER_RSP || o.mem.index == ZYDIS_REGISTER_RSP ||
           o.mem.base == ZYDIS_REGISTER_RIP ||
           (o.mem.base == ZYDIS_REGISTER_NONE && o.mem.index == ZYDIS_REGISTER_NONE)))
        return true;
    }
  }
  return false;
}

// SSE/SSE2 forms for tests/lift_sse_test.cpp: XMM-register forms only
// (no MMX), without ldmxcsr (random memory would #GP or unmask) and
// prefetches (no effect to compare).
bool SseCandidate(const Insn& in) {
  switch (in.d.meta.isa_set) {
    case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2: case ZYDIS_ISA_SET_SSEMXCSR:
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSE4:
    case ZYDIS_ISA_SET_AVX:
      break;
    default:
      return false;
  }
  if (in.d.mnemonic == ZYDIS_MNEMONIC_LDMXCSR || in.d.mnemonic == ZYDIS_MNEMONIC_VLDMXCSR)
    return false;
  // Approximate: no host result to match (tests/sse_rsqrt_test instead).
  if (in.d.mnemonic == ZYDIS_MNEMONIC_RSQRTPS || in.d.mnemonic == ZYDIS_MNEMONIC_RSQRTSS)
    return false;
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        ZydisRegisterGetClass(in.ops[i].reg.value) == ZYDIS_REGCLASS_MMX)
      return false;
  return !BadOperands(in);
}

// GPR number (0..7, 0..15 for x64 code) behind a register of any width,
// or -1.
int GprNum(ZydisRegister r) {
  ZydisRegister l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
  if (l >= ZYDIS_REGISTER_EAX && l <= ZYDIS_REGISTER_EDI) return int(l - ZYDIS_REGISTER_EAX);
  l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
  if (l >= ZYDIS_REGISTER_RAX && l <= ZYDIS_REGISTER_R15) return int(l - ZYDIS_REGISTER_RAX);
  return -1;
}

// Index in a copy made by CopyNoSegPrefixes of the raw byte at `off`.
int CopiedIndex(const uint8_t* raw, const ZydisDecodedInstruction& d, int off) {
  int k = off;
  for (int i = 0; i < off && i < d.raw.prefix_count; i++)
    if (IsSegOrAddrPrefix(raw[i])) k--;
  return k;
}

}  // namespace

namespace {

// The integer and SSE tests share the form extraction: each distinct
// instruction (by bytes) with its memory operand rewritten to [base].
int WriteFormTest(const Program& p, const Decoder& dec, const std::string& out,
                  const char* prologue, bool sse) {
  struct IForm {
    std::vector<uint8_t> bytes;
    std::string text;
    int base = -1;  // register holding the buffer address, -1: no memory
    uint32_t undef = 0;
    uint64_t example_va = 0;
  };
  std::map<std::vector<uint8_t>, IForm> forms;
  for (uint64_t e : p.entries) {
    Function f;
    BuildFunction(p, dec, e, &f);
    for (const auto& [va, in] : f.insns) {
      if (!in.ok || (sse ? !SseCandidate(in) : Excluded(in))) continue;
      const uint8_t* raw = p.Bytes(va);
      std::vector<uint8_t> b;
      int base = -1;
      bool has_modrm = in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM;
      bool mem = false;
      for (int i = 0; i < in.d.operand_count_visible; i++)
        if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            in.d.mnemonic != ZYDIS_MNEMONIC_LEA)
          mem = true;
      // Registers the instruction uses in any role (explicit or implicit).
      uint32_t used = 0;
      for (int i = 0; i < in.d.operand_count; i++)
        if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
          int n = GprNum(in.ops[i].reg.value);
          if (n >= 0) used |= 1u << n;
        }
      bool string_op = in.d.meta.category == ZYDIS_CATEGORY_STRINGOP;
      bool moffs = !has_modrm && in.d.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT &&
                   in.d.opcode >= 0xA0 && in.d.opcode <= 0xA3;
      if (moffs) {
        // mov al/eax, [moffs32] and back: in 64-bit mode A0-A3 take an
        // 8-byte address, so use the ModRM form (8A/8B/88/89) on [base].
        static const uint8_t op[] = {0x8A, 0x8B, 0x88, 0x89};
        for (int r : {3, 6, 7, 1, 2})
          if (!(used & (1u << r))) {
            base = r;
            break;
          }
        CopyNoSegPrefixes(raw, in.d, in.d.raw.prefix_count, &b);
        if (in.d.attributes & ZYDIS_ATTRIB_HAS_REX)
          b[CopiedIndex(raw, in.d, in.d.raw.rex.offset)] &= 0xFC;
        b.push_back(op[in.d.opcode - 0xA0]);
        b.push_back(uint8_t((0 << 3) | base));  // reg = al/ax/eax
      } else if (string_op) {
        // Implicit [esi]/[edi]: the test points them into the buffer.
        CopyNoSegPrefixes(raw, in.d, in.d.length, &b);
      } else if (has_modrm && mem) {
        int reg = in.d.raw.modrm.reg;
        for (int r : {3, 6, 7, 1, 2, 0})
          if (r != reg && !(used & (1u << r))) {
            base = r;
            break;
          }
        if (base < 0) continue;
        CopyNoSegPrefixes(raw, in.d, in.d.raw.modrm.offset, &b);
        // x64: [base] without SIB, so REX.X and REX.B go (REX.R keeps the
        // reg field's register); in a 3-byte VEX prefix they are inverted.
        if (in.d.attributes & ZYDIS_ATTRIB_HAS_REX)
          b[CopiedIndex(raw, in.d, in.d.raw.rex.offset)] &= 0xFC;
        if ((in.d.attributes & ZYDIS_ATTRIB_HAS_VEX) && in.d.raw.vex.size == 3)
          b[CopiedIndex(raw, in.d, in.d.raw.vex.offset) + 1] |= 0x60;
        b.push_back(uint8_t((reg << 3) | base));
        // Immediates follow ModRM/SIB/displacement: keep them.
        int imm_off = in.d.raw.imm[0].size ? in.d.raw.imm[0].offset : in.d.length;
        for (int i = imm_off; i < in.d.length; i++) b.push_back(raw[i]);
      } else {
        CopyNoSegPrefixes(raw, in.d, in.d.length, &b);
        // inc/dec r32 (40-4F) are REX prefixes in 64-bit mode: use FF /0, /1.
        if (!p.x64 && b.size() == 1 && b[0] >= 0x40 && b[0] <= 0x4F)
          b = {0xFF, uint8_t(0xC0 + (b[0] >= 0x48 ? 8 : 0) + (b[0] & 7))};
        else if (!p.x64 && b.size() == 2 && b[0] == 0x66 && b[1] >= 0x40 && b[1] <= 0x4F)
          b = {0x66, 0xFF, uint8_t(0xC0 + (b[1] >= 0x48 ? 8 : 0) + (b[1] & 7))};
      }
      if (forms.count(b)) continue;
      IForm fm;
      fm.bytes = b;
      fm.text = dec.Format(in);
      fm.base = base;
      fm.undef = in.d.cpu_flags ? in.d.cpu_flags->undefined : 0;
      fm.example_va = va;
      forms[b] = fm;
    }
  }

  std::vector<const IForm*> all;
  std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> code;
  for (const auto& [k, fm] : forms) {
    all.push_back(&fm);
    code.push_back({&fm.bytes, 32});
  }
  std::vector<uint64_t> vas;
  std::string o = LiftSnippets(dec, prologue, code, &vas, p.x64);
  if (o.empty()) return 1;
  const std::string kind = sse ? "Sse" : "Int";
  o += std::string("#include \"lift_") + (sse ? "sse" : "int") + "_forms.h\"\n" +
       "extern const " + kind + "Form k" + kind + "Forms[] = {\n";
  for (size_t i = 0; i < all.size(); i++) {
    const IForm& fm = *all[i];
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "  {&recomp::gen::f_%08llX, {%s}, %zu, %d, 0x%X, \"%s\", 0x%08llX},\n",
                  (unsigned long long)vas[i], HexBytes(fm.bytes).c_str(), fm.bytes.size(),
                  fm.base, fm.undef, Literal(fm.text).c_str(),
                  (unsigned long long)fm.example_va);
    o += buf;
  }
  o += "};\nextern const int k" + kind + "FormCount = " + std::to_string(all.size()) +
       ";\n";
  if (WriteFile(out, o)) return 1;
  std::printf("%s forms: %zu -> %s\n", sse ? "SSE" : "integer", all.size(), out.c_str());
  return 0;
}

}  // namespace

int WriteIntTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  return WriteFormTest(p, dec, out, prologue, false);
}

int WriteSseTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  return WriteFormTest(p, dec, out, prologue, true);
}

}  // namespace lifter

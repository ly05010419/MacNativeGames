#include "function.h"

#include <algorithm>
#include <cstdio>
#include <deque>

#include "runtime/unwind.h"

namespace lifter {

Decoder::Decoder(bool x64) {
  if (x64)
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  else
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
  ZydisFormatterInit(&fmt_, ZYDIS_FORMATTER_STYLE_INTEL);
}

bool Decoder::Decode(const Program& p, uint64_t va, Insn* out) const {
  out->va = va;
  out->ok = false;
  if (!p.InText(va)) return false;
  size_t n = p.BytesLeft(va);
  if (n > ZYDIS_MAX_INSTRUCTION_LENGTH) n = ZYDIS_MAX_INSTRUCTION_LENGTH;
  out->ok = ZYAN_SUCCESS(
      ZydisDecoderDecodeFull(&dec_, p.Bytes(va), n, &out->d, out->ops));
  return out->ok;
}

std::string Decoder::Format(const Insn& in) const {
  if (!in.ok) return "(bad)";
  char buf[256];
  ZydisFormatterFormatInstruction(&fmt_, &in.d, in.ops,
                                  in.d.operand_count_visible, buf, sizeof buf,
                                  in.va, ZYAN_NULL);
  return buf;
}

uint64_t RelTarget(const Insn& in) {
  ZyanU64 t = 0;
  ZydisCalcAbsoluteAddress(&in.d, &in.ops[0], in.va, &t);
  // 32-bit code wraps around the 4 GiB address space.
  return in.d.machine_mode == ZYDIS_MACHINE_MODE_LONG_64 ? t : uint32_t(t);
}

namespace {

bool IsJcc(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ:
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsOtherEntry(const Program& p, const Function& f, uint64_t t) {
  return t != f.entry && p.Splits(t);
}

Flow Classify(const Program& p, const Function& f, const Insn& in,
              uint64_t* target) {
  *target = 0;
  if (!in.ok) return Flow::Stop;
  const auto& d = in.d;
  const auto& op0 = in.ops[0];
  switch (d.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
      return Flow::Return;
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_INT1:
      return d.mnemonic == ZYDIS_MNEMONIC_INT ? Flow::Fall : Flow::Stop;
    case ZYDIS_MNEMONIC_JMP:
      if (op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        *target = RelTarget(in);
        return IsOtherEntry(p, f, *target) ? Flow::TailCall : Flow::Jump;
      }
      if (op0.type == ZYDIS_OPERAND_TYPE_POINTER) return Flow::Stop;
      return p.jumptables.count(in.va) ? Flow::JumpTable : Flow::TailDispatch;
    case ZYDIS_MNEMONIC_CALL:
      if (op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        *target = RelTarget(in);
        if (p.noreturn.count(*target)) return Flow::Stop;
      } else if (op0.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 op0.mem.base == ZYDIS_REGISTER_NONE &&
                 op0.mem.index == ZYDIS_REGISTER_NONE &&
                 p.noreturn_iat.count(
                     static_cast<uint32_t>(op0.mem.disp.value))) {
        return Flow::Stop;
      } else if (op0.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 op0.mem.base == ZYDIS_REGISTER_RIP &&
                 p.noreturn_iat.count(in.next() + op0.mem.disp.value)) {
        return Flow::Stop;
      }
      return Flow::Fall;
    default:
      break;
  }
  if (IsJcc(d.mnemonic)) {
    *target = RelTarget(in);
    return Flow::Branch;
  }
  return Flow::Fall;
}

namespace {

// The 64-bit register enclosing r (rax for eax, ax, al and ah alike), so
// comparisons work for both 32-bit and x64 code.
ZydisRegister Full(ZydisRegister r) {
  return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
}

bool WritesReg(const Insn& in, ZydisRegister r) {
  for (int i = 0; i < in.d.operand_count; i++)
    if (in.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        (in.ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
        Full(in.ops[i].reg.value) == Full(r))
      return true;
  return false;
}

bool WritesFlags(const Insn& in) {
  return in.d.cpu_flags &&
         (in.d.cpu_flags->modified | in.d.cpu_flags->set_0 |
          in.d.cpu_flags->set_1 | in.d.cpu_flags->undefined) != 0;
}

// The decoded instruction of f that ends at va, if any.
const Insn* Before(const Function& f, uint64_t va) {
  auto it = f.insns.lower_bound(va);
  if (it == f.insns.begin()) return nullptr;
  --it;
  return it->second.ok && it->second.next() == va ? &it->second : nullptr;
}

}  // namespace

namespace {

// The .pdata entry whose range holds va, or null.
const recomp::PeFunction* PdataAt(const recomp::PeImage& img, uint64_t va) {
  const auto& f = img.functions;
  auto it = std::upper_bound(f.begin(), f.end(), va,
                             [](uint64_t v, const recomp::PeFunction& e) { return v < e.begin; });
  if (it == f.begin()) return nullptr;
  --it;
  return va < it->end ? &*it : nullptr;
}

// The start of the function a .pdata range belongs to: chained fragments
// (code the compiler moved out of line) lead to their parent, in short
// form (unwind RVA with bit 0 set) or through the UNWIND_INFO.
uint64_t PrimaryOf(const Program& p, const recomp::PeImage& img, const recomp::PeFunction* f) {
  for (int depth = 0; f && f->chained() && depth < 32; depth++) {
    uint64_t parent = p.mem.read<uint32_t>(f->entry + 8) & 1
                          ? f->unwind
                          : recomp::unwind::Read(p.mem, img.image_base, f->unwind).chained;
    f = PdataAt(img, img.image_base + p.mem.read<uint32_t>(parent));
  }
  return f ? f->begin : 0;
}

// x64 MSVC switches: a table of 32-bit RVAs indexed off the image base
// (which some register holds), sometimes through a byte index table, with
// the index widened between the bounds check and the load:
//   cmp eI, N; ja default; [movsxd rI, eJ | cdqe] [lea rB, [__ImageBase]]
//   [movzx eI, byte [rK + rB + index_rva]] mov eT, [rB + rI*4 + table_rva]
//   add rT, rB; jmp rT
// Without a bounds check (an index the compiler knows is in range, such as
// a state variable), the table runs while its entries target the jmp's own
// function, to the end of the .pdata range that holds it (MSVC places a
// function's tables inside its range, after the code).
bool RecoverJumpTable64(const Program& p, const Function& f, const Insn& jmp,
                        std::vector<uint64_t>* targets) {
  const auto& j0 = jmp.ops[0];
  if (!jmp.ok || jmp.d.mnemonic != ZYDIS_MNEMONIC_JMP ||
      j0.type != ZYDIS_OPERAND_TYPE_REGISTER)
    return false;
  const Module* mod = p.TextOf(jmp.va);
  const uint64_t image = mod->img.image_base;
  auto in_text = [&](uint64_t a, uint64_t n) {
    return n && p.TextOf(a) == mod && p.TextOf(a + n - 1) == mod;
  };
  auto reg = [](const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER ? Full(o.reg.value) : ZYDIS_REGISTER_NONE;
  };
  const ZydisRegister t = Full(j0.reg.value);
  enum { kAdd, kLoad, kJa, kCmp } state = kAdd;
  ZydisRegister idx = ZYDIS_REGISTER_NONE, base = ZYDIS_REGISTER_NONE;
  uint64_t table = 0, index = 0;
  bool two_level = false;
  auto unbounded = [&]() {
    if (state < kJa || two_level) return false;
    const recomp::PeImage& img = mod->img;
    const uint64_t home = PrimaryOf(p, img, PdataAt(img, jmp.va));
    const recomp::PeFunction* holder = PdataAt(img, table);
    if (!home || !holder || PrimaryOf(p, img, holder) != home) return false;
    targets->clear();
    for (uint64_t a = table; a + 4 <= holder->end; a += 4) {
      uint64_t tv = image + p.mem.read<uint32_t>(a);
      if (p.TextOf(tv) != mod || PrimaryOf(p, img, PdataAt(img, tv)) != home) break;
      if (std::find(targets->begin(), targets->end(), tv) == targets->end())
        targets->push_back(tv);
    }
    return !targets->empty();
  };
  const Insn* in = &jmp;
  for (int steps = 0; steps < 16; steps++) {
    in = Before(f, in->va);
    if (!in) return unbounded();
    const auto& d = in->d;
    const auto& o0 = in->ops[0];
    const auto& o1 = in->ops[1];
    switch (state) {
      case kAdd:
        if (d.mnemonic == ZYDIS_MNEMONIC_ADD && reg(o0) == t && o0.size == 64 &&
            o1.type == ZYDIS_OPERAND_TYPE_REGISTER) {
          state = kLoad;
        } else if (WritesReg(*in, t) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return false;
        }
        break;
      case kLoad:
        if ((d.mnemonic == ZYDIS_MNEMONIC_MOV || d.mnemonic == ZYDIS_MNEMONIC_MOVSXD) &&
            reg(o0) == t && o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 32 &&
            o1.mem.base != ZYDIS_REGISTER_NONE && o1.mem.base != ZYDIS_REGISTER_RIP &&
            o1.mem.index != ZYDIS_REGISTER_NONE && o1.mem.scale == 4) {
          idx = Full(o1.mem.index);
          base = Full(o1.mem.base);
          table = image + uint64_t(o1.mem.disp.value);
          state = kJa;
        } else if (WritesReg(*in, t) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return false;
        }
        break;
      case kJa:
        if (d.mnemonic == ZYDIS_MNEMONIC_JNBE) {
          state = kCmp;
        } else if (!two_level && d.mnemonic == ZYDIS_MNEMONIC_MOVZX && reg(o0) == idx &&
                   o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 8 &&
                   o1.mem.index != ZYDIS_REGISTER_NONE && o1.mem.scale == 1 &&
                   (Full(o1.mem.base) == base || Full(o1.mem.index) == base)) {
          // The image base register is either operand of the byte load.
          two_level = true;
          index = image + uint64_t(o1.mem.disp.value);
          idx = Full(o1.mem.base) == base ? Full(o1.mem.index) : Full(o1.mem.base);
        } else if ((d.mnemonic == ZYDIS_MNEMONIC_MOVSXD ||
                    (d.mnemonic == ZYDIS_MNEMONIC_MOV && o0.size == 32)) &&
                   reg(o0) == idx && o1.type == ZYDIS_OPERAND_TYPE_REGISTER) {
          idx = Full(o1.reg.value);  // widened from another register
        } else if (d.mnemonic == ZYDIS_MNEMONIC_CDQE && idx == ZYDIS_REGISTER_RAX) {
          // rax from eax: same register
        } else if (WritesReg(*in, idx) || d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
          return unbounded();
        }
        break;
      case kCmp:
        if (!WritesFlags(*in)) {
          if (WritesReg(*in, idx)) return unbounded();
          break;
        }
        if (d.mnemonic != ZYDIS_MNEMONIC_CMP || reg(o0) != idx ||
            o1.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || o1.imm.value.u >= 4096)
          return unbounded();
        uint32_t cases = static_cast<uint32_t>(o1.imm.value.u) + 1;
        if (two_level) {
          if (!in_text(index, cases)) return false;
          uint32_t n = cases;
          cases = 0;
          for (uint32_t i = 0; i < n; i++)
            cases = std::max<uint32_t>(cases, p.mem.read<uint8_t>(index + i) + 1u);
        }
        if (!in_text(table, uint64_t(cases) * 4)) return false;
        targets->clear();
        for (uint32_t i = 0; i < cases; i++) {
          uint64_t tv = image + p.mem.read<uint32_t>(table + 4 * i);
          if (p.TextOf(tv) != mod) return false;
          if (std::find(targets->begin(), targets->end(), tv) == targets->end())
            targets->push_back(tv);
        }
        return true;
    }
  }
  return unbounded();
}

}  // namespace

bool RecoverJumpTable(const Program& p, const Function& f, const Insn& jmp,
                      std::vector<uint64_t>* targets) {
  if (p.x64) return RecoverJumpTable64(p, f, jmp, targets);
  const auto& m = jmp.ops[0];
  if (!jmp.ok || jmp.d.mnemonic != ZYDIS_MNEMONIC_JMP ||
      m.type != ZYDIS_OPERAND_TYPE_MEMORY || m.mem.base != ZYDIS_REGISTER_NONE ||
      m.mem.index == ZYDIS_REGISTER_NONE || m.mem.scale != 4)
    return false;
  // MSVC places the tables in .text after the function.
  const Module* mod = p.TextOf(jmp.va);
  auto in_text = [&](uint64_t a, uint32_t n) {
    return n && p.TextOf(a) == mod && p.TextOf(a + n - 1) == mod;
  };
  // Walk back from the jmp through the straight-line code before it
  // (the compiler schedules unrelated instructions in between, e.g. a
  // run of x87 loads and stores): an optional movzx from the byte index
  // table, the ja, then the cmp that sets its flags.
  ZydisRegister r = m.mem.index;
  uint32_t index = 0;
  bool two_level = false, seen_ja = false;
  // The byte index loaded with a byte mov (not movzx) needs the xor that
  // clears the rest of its register before it.
  ZydisRegister needs_zero = ZYDIS_REGISTER_NONE;
  const Insn* in = &jmp;
  for (int steps = 0; steps < 16; steps++) {
    in = Before(f, in->va);
    if (!in) return false;
    const auto& o0 = in->ops[0];
    const auto& o1 = in->ops[1];
    if (!seen_ja) {
      if (in->d.mnemonic == ZYDIS_MNEMONIC_JNBE) {
        seen_ja = true;
      } else if (!two_level && in->d.mnemonic == ZYDIS_MNEMONIC_MOVZX &&
                 o0.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                 Full(o0.reg.value) == Full(r) &&
                 o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 8 &&
                 o1.mem.base != ZYDIS_REGISTER_NONE &&
                 o1.mem.index == ZYDIS_REGISTER_NONE) {
        two_level = true;
        index = static_cast<uint32_t>(o1.mem.disp.value);
        r = o1.mem.base;
      } else if (!two_level && in->d.mnemonic == ZYDIS_MNEMONIC_MOV &&
                 o0.type == ZYDIS_OPERAND_TYPE_REGISTER && o0.size == 8 &&
                 Full(o0.reg.value) == Full(r) &&
                 o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 8 &&
                 o1.mem.base != ZYDIS_REGISTER_NONE &&
                 o1.mem.index == ZYDIS_REGISTER_NONE &&
                 ZydisRegisterGetId(o0.reg.value) < 4 &&
                 ZydisRegisterGetClass(o0.reg.value) == ZYDIS_REGCLASS_GPR8 &&
                 Full(o1.mem.base) != Full(r)) {
        // xor edx, edx; mov dl, [index + eax]: the same as a movzx.
        two_level = true;
        needs_zero = Full(r);
        index = static_cast<uint32_t>(o1.mem.disp.value);
        r = o1.mem.base;
      } else if (needs_zero != ZYDIS_REGISTER_NONE && in->d.mnemonic == ZYDIS_MNEMONIC_XOR &&
                 o0.type == ZYDIS_OPERAND_TYPE_REGISTER && o1.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                 o0.reg.value == o1.reg.value && o0.size == 32 && Full(o0.reg.value) == needs_zero) {
        needs_zero = ZYDIS_REGISTER_NONE;
      } else if (in->d.mnemonic == ZYDIS_MNEMONIC_JMP && in->ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                 in->next() + in->ops[0].imm.value.u == jmp.va) {
        // A jump to the switch itself (the compiler joined two paths
        // there): the code before it is the path with the bounds check.
      } else if (WritesReg(*in, r) || in->d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE ||
                 (needs_zero != ZYDIS_REGISTER_NONE && WritesReg(*in, needs_zero))) {
        return false;
      }
      if (seen_ja && needs_zero != ZYDIS_REGISTER_NONE) return false;
      continue;
    }
    if (!WritesFlags(*in)) {
      if (WritesReg(*in, r) || in->d.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) return false;
      continue;
    }
    if (in->d.mnemonic != ZYDIS_MNEMONIC_CMP ||
        o0.type != ZYDIS_OPERAND_TYPE_REGISTER || Full(o0.reg.value) != Full(r) ||
        o1.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || o1.imm.value.u >= 4096)
      return false;
    uint32_t cases = static_cast<uint32_t>(o1.imm.value.u) + 1;
    if (two_level) {
      // A byte index table picks the entry.
      if (!in_text(index, cases)) return false;
      uint32_t n = cases;
      cases = 0;
      for (uint32_t i = 0; i < n; i++)
        cases = std::max<uint32_t>(cases, p.mem.read<uint8_t>(index + i) + 1u);
    }
    if (!in_text(static_cast<uint32_t>(m.mem.disp.value), cases * 4)) return false;
    targets->clear();
    for (uint32_t i = 0; i < cases; i++) {
      uint64_t t = p.mem.read<uint32_t>(static_cast<uint32_t>(m.mem.disp.value) + 4 * i);
      if (p.TextOf(t) != mod) return false;
      // Cases often share a target; each becomes one switch case.
      if (std::find(targets->begin(), targets->end(), t) == targets->end())
        targets->push_back(t);
    }
    return true;
  }
  return false;
}

void BuildFunction(const Program& p, const Decoder& dec, uint64_t entry,
                   Function* out) {
  Function& f = *out;
  f.entry = entry;
  std::deque<uint64_t> work{entry};
  f.labels.insert(entry);

  auto reach = [&](uint64_t va) {
    if (!p.InText(va)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "branch to %08llX outside .text",
                    (unsigned long long)va);
      f.warnings.push_back(buf);
      return;
    }
    // Falling or jumping into another function's entry is a tail call,
    // handled by the emitter; never lift that code into this function.
    if (IsOtherEntry(p, f, va)) {
      f.tail_calls.insert(va);
      return;
    }
    if (!f.insns.count(va)) work.push_back(va);
  };

  while (!work.empty()) {
    uint64_t va = work.front();
    work.pop_front();
    if (f.insns.count(va)) continue;
    Insn in;
    dec.Decode(p, va, &in);
    f.insns.emplace(va, in);
    if (!in.ok) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "undecodable bytes at %08llX",
                    (unsigned long long)va);
      f.warnings.push_back(buf);
      continue;
    }
    uint64_t t = 0;
    switch (Classify(p, f, in, &t)) {
      case Flow::Fall:
        if (in.d.mnemonic == ZYDIS_MNEMONIC_CALL &&
            in.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          uint64_t ct = RelTarget(in);
          if (ct != in.next()) f.direct_calls.insert(ct);
          if (p.setjmps.count(ct)) f.setjmp_returns.insert(in.next());
        }
        reach(in.next());
        break;
      case Flow::Branch:
        if (IsOtherEntry(p, f, t)) {
          f.tail_calls.insert(t);
        } else {
          f.labels.insert(t);
          reach(t);
        }
        reach(in.next());
        break;
      case Flow::Jump:
        f.labels.insert(t);
        reach(t);
        break;
      case Flow::TailCall:
        f.tail_calls.insert(t);
        break;
      case Flow::JumpTable:
        for (uint64_t jt : p.jumptables.at(va)) {
          if (IsOtherEntry(p, f, jt)) {
            f.tail_calls.insert(jt);
          } else {
            f.labels.insert(jt);
            reach(jt);
          }
        }
        break;
      case Flow::TailDispatch:
      case Flow::Return:
      case Flow::Stop:
        break;
    }
  }
}

}  // namespace lifter

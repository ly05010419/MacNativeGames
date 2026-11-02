#pragma once
// Guest CPU context: one per guest thread. Lifted functions take it by
// reference, cache registers and flags in locals, and spill them back
// around anything that can observe them (calls, HLE, returns).

#include <cstdint>

#include "runtime/x87.h"

namespace recomp {

struct Flags {
  bool cf, pf, af, zf, sf, of, df;
  // AC (bit 18) and ID (bit 21) are just storage, but user code can flip
  // them on every CPU since the 486 (AC) and every one with CPUID (ID),
  // and that is how it detects them.
  bool ac, id;
};

struct Thread;

// An SSE register: 16 bytes, lanes little-endian (as in guest memory).
struct Xmm {
  uint8_t b[16];
};

struct Ctx {
  // Each register in an 8-byte slot of its own. With adjacent fields the
  // compiler merges spills and reloads of neighbours into 64-bit or vector
  // accesses, which the CPU cannot forward from or to the 32-bit accesses
  // of the function on the other side of the call (see ARCHITECTURE.md,
  // "Registers in Ctx"). x64 guests use the whole slot (rax..r15); 32-bit
  // ones only its low half (eax..edi), both hosts being little-endian.
  union { uint64_t rax; uint32_t eax; };
  union { uint64_t rcx; uint32_t ecx; };
  union { uint64_t rdx; uint32_t edx; };
  union { uint64_t rbx; uint32_t ebx; };
  union { uint64_t rsp; uint32_t esp; };
  union { uint64_t rbp; uint32_t ebp; };
  union { uint64_t rsi; uint32_t esi; };
  union { uint64_t rdi; uint32_t edi; };
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  // Flags as one EFLAGS word (PackEFlags), not as the Flags bytes lifted
  // code keeps in locals: a spill is one 32-bit store and a reload one
  // 32-bit load, which the CPU forwards (see ARCHITECTURE.md, "Flags").
  alignas(8) uint32_t eflags = 0x202;
  uint32_t fs_base;  // guest VA of this thread's TEB (32-bit guests)
  uint64_t gs_base;  // guest VA of this thread's TEB (x64 guests)
  x87::State fpu;
  // SSE state. Unlike the integer registers these are never cached in
  // locals: lifted code operates on them in place (runtime/sse.h). 32-bit
  // guests have xmm0-7; ymm_hi holds bits 255:128 of the AVX registers.
  Xmm xmm[16];
  Xmm ymm_hi[16];
  uint32_t mxcsr;
  Thread* thread;    // runtime-owned per-thread data
};

// EFLAGS packing for Ctx::eflags, pushfd/popfd and lahf/sahf. Bit 1 always
// reads as 1 and IF (bit 9) as set, like user mode on Windows.
[[gnu::always_inline]] inline uint32_t PackEFlags(const Flags& f) {
  return (f.cf ? 0x1u : 0) | 0x2u | (f.pf ? 0x4u : 0) | (f.af ? 0x10u : 0) |
         (f.zf ? 0x40u : 0) | (f.sf ? 0x80u : 0) | 0x200u |
         (f.df ? 0x400u : 0) | (f.of ? 0x800u : 0) | (f.ac ? 0x40000u : 0) |
         (f.id ? 0x200000u : 0);
}
[[gnu::always_inline]] inline void UnpackEFlags(Flags& f, uint32_t v) {
  f.cf = v & 0x1;
  f.pf = v & 0x4;
  f.af = v & 0x10;
  f.zf = v & 0x40;
  f.sf = v & 0x80;
  f.df = v & 0x400;
  f.of = v & 0x800;
  f.ac = v & 0x40000;
  f.id = v & 0x200000;
}

}  // namespace recomp

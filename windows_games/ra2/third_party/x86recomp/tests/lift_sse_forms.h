#pragma once
// One lifted SSE/SSE2 instruction form (from recomp_lift --sse-test).
#include <cstdint>

#include "runtime/ctx.h"

struct SseForm {
  void (*fn)(recomp::Ctx&);
  uint8_t bytes[15];
  uint8_t len;
  int base;           // GPR holding the memory operand's address, -1: none
  uint32_t undef;     // EFLAGS bits the instruction leaves undefined
  const char* text;   // disassembly of the original instruction
  uint64_t example_va;
};
extern const SseForm kSseForms[];
extern const int kSseFormCount;

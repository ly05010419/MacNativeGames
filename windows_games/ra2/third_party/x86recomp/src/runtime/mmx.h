// MMX instructions, lifted onto the x87 register file they alias.
//
// Intel SDM Vol. 1 §9.5: MM0-MM7 are the low 64 bits (significand) of the
// x87 physical registers R0-R7. Every MMX instruction except EMMS sets TOP
// to 0 and marks all eight registers valid; an MMX write also sets the
// register's sign and exponent bits (79-64) to all ones. EMMS marks all
// eight registers empty. Operand semantics: SDM Vol. 2 for each mnemonic.
#pragma once

#include <cstdint>

#include "runtime/x87.h"

namespace recomp::mmx {

// The state change every MMX instruction but EMMS makes.
[[gnu::always_inline]] inline void Enter(x87::State& s) {
  s.top = 0;
  s.empty = 0;
}
[[gnu::always_inline]] inline uint64_t Get(const x87::State& s, int n) { return s.r[n].sig; }
[[gnu::always_inline]] inline void Set(x87::State& s, int n, uint64_t v) {
  s.r[n].sig = v;
  s.r[n].se = 0xFFFF;
}
[[gnu::always_inline]] inline void Emms(x87::State& s) { s.empty = 0xFF; }

namespace detail {
template <typename T>
[[gnu::always_inline]] inline T Lane(uint64_t v, int i) {
  return T(v >> (i * 8 * sizeof(T)));
}
template <typename T>
[[gnu::always_inline]] inline uint64_t Put(uint64_t r, int i, T x) {
  constexpr int kBits = 8 * sizeof(T);
  constexpr uint64_t kMask = kBits == 64 ? ~0ull : (1ull << kBits) - 1;
  const int sh = i * kBits;
  return (r & ~(kMask << sh)) | ((uint64_t(x) & kMask) << sh);
}
inline int16_t SatS16(int32_t v) { return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
inline uint8_t SatU8(int32_t v) { return uint8_t(v > 255 ? 255 : v < 0 ? 0 : v); }
}  // namespace detail

// PSRLW: each word shifted right logically; counts above 15 clear it.
inline uint64_t Psrlw(uint64_t v, uint64_t n) {
  if (n > 15) return 0;
  uint64_t r = 0;
  for (int i = 0; i < 4; i++)
    r = detail::Put<uint16_t>(r, i, uint16_t(detail::Lane<uint16_t>(v, i) >> n));
  return r;
}

// PSRAD: each dword shifted right arithmetically; counts above 31 fill it
// with its sign bit.
inline uint64_t Psrad(uint64_t v, uint64_t n) {
  const int s = n > 31 ? 31 : int(n);
  uint64_t r = 0;
  for (int i = 0; i < 2; i++)
    r = detail::Put<uint32_t>(r, i, uint32_t(int32_t(detail::Lane<uint32_t>(v, i)) >> s));
  return r;
}

// PUNPCKLBW: interleave the low four bytes, destination first. The
// memory form reads only 32 bits (the caller passes them zero-extended).
inline uint64_t Punpcklbw(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    r = detail::Put<uint8_t>(r, 2 * i, detail::Lane<uint8_t>(d, i));
    r = detail::Put<uint8_t>(r, 2 * i + 1, detail::Lane<uint8_t>(s, i));
  }
  return r;
}

// PUNPCKHWD: interleave the high two words, destination first.
inline uint64_t Punpckhwd(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 2; i++) {
    r = detail::Put<uint16_t>(r, 2 * i, detail::Lane<uint16_t>(d, 2 + i));
    r = detail::Put<uint16_t>(r, 2 * i + 1, detail::Lane<uint16_t>(s, 2 + i));
  }
  return r;
}

// PACKUSWB: signed words to unsigned saturated bytes; destination words
// give the low four bytes, source words the high four.
inline uint64_t Packuswb(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    r = detail::Put<uint8_t>(r, i, detail::SatU8(int16_t(detail::Lane<uint16_t>(d, i))));
    r = detail::Put<uint8_t>(r, 4 + i, detail::SatU8(int16_t(detail::Lane<uint16_t>(s, i))));
  }
  return r;
}

// PACKSSDW: signed dwords to signed saturated words; destination dwords
// give the low two words, source dwords the high two.
inline uint64_t Packssdw(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 2; i++) {
    r = detail::Put<uint16_t>(r, i, uint16_t(detail::SatS16(int32_t(detail::Lane<uint32_t>(d, i)))));
    r = detail::Put<uint16_t>(r, 2 + i,
                              uint16_t(detail::SatS16(int32_t(detail::Lane<uint32_t>(s, i)))));
  }
  return r;
}

// PMULHW / PMULLW: signed word products, high / low 16 bits of each.
inline uint64_t Pmulhw(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    const int32_t p = int32_t(int16_t(detail::Lane<uint16_t>(d, i))) *
                      int32_t(int16_t(detail::Lane<uint16_t>(s, i)));
    r = detail::Put<uint16_t>(r, i, uint16_t(uint32_t(p) >> 16));
  }
  return r;
}
inline uint64_t Pmullw(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    const int32_t p = int32_t(int16_t(detail::Lane<uint16_t>(d, i))) *
                      int32_t(int16_t(detail::Lane<uint16_t>(s, i)));
    r = detail::Put<uint16_t>(r, i, uint16_t(uint32_t(p)));
  }
  return r;
}

// PADDUSB: unsigned bytes added with saturation at 0xFF.
inline uint64_t Paddusb(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 8; i++)
    r = detail::Put<uint8_t>(
        r, i, detail::SatU8(int32_t(detail::Lane<uint8_t>(d, i)) + detail::Lane<uint8_t>(s, i)));
  return r;
}

}  // namespace recomp::mmx

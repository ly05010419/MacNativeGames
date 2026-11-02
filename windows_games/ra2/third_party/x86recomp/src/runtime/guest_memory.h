#pragma once
// The guest address space lives in one host reservation of 2^bits bytes:
// 4 GiB for 32-bit guests, more for x64 ones (ARCHITECTURE.md, "64-bit
// guests"). Guest address A is host address base() + A. The base is
// picked at startup and is not a compile-time constant.
//
// Win32 memory semantics (4 KiB commit granularity, protections) are kept
// in a per-guest-page table. The host only distinguishes accessible from
// inaccessible, at host page granularity (4 KiB on x86-64 Linux, 16 KiB on
// Apple Silicon), so guest behaviour never depends on host page faults.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp {

using gaddr = uint64_t;

enum class Prot : uint8_t { None = 0, R = 1, W = 2, X = 4, RW = 3, RX = 5, RWX = 7 };

class GuestMemory {
 public:
  static constexpr uint32_t kPageSize = 0x1000;  // guest (Win32) page

  // Above 32 bits the reservation is followed by an inaccessible guard of
  // the same size: lifted x64 code adds addresses to the base unmasked.
  bool Reserve(unsigned bits = 32);

  // Commit [va, va+size) rounded out to guest pages; newly committed pages
  // read as zero. Protect fails if any page in the range is uncommitted,
  // like VirtualProtect. Decommitted pages read as zero when recommitted.
  bool Commit(gaddr va, uint64_t size, Prot prot);
  bool Protect(gaddr va, uint64_t size, Prot prot);
  // zero=false skips clearing the pages (they are being thrown away and
  // may not even be host-writable, e.g. an unmapped file view).
  void Decommit(gaddr va, uint64_t size, bool zero = true);
  bool IsCommitted(gaddr va) const;
  Prot QueryProt(gaddr va) const;

  uint8_t* base() const { return base_; }
  uint64_t size() const { return size_; }
  size_t host_page_size() const { return host_page_; }

  template <typename T = uint8_t>
  T* ptr(gaddr va) const {
    return reinterpret_cast<T*>(base_ + va);
  }
  gaddr to_guest(const void* host) const {
    return static_cast<gaddr>(static_cast<const uint8_t*>(host) - base_);
  }

  // Guest and every supported host are little-endian, so loads/stores are
  // plain copies. memcpy keeps unaligned guest accesses well-defined.
  template <typename T>
  T read(gaddr va) const {
    T v;
    std::memcpy(&v, base_ + va, sizeof(T));
    return v;
  }
  template <typename T>
  void write(gaddr va, T v) const {
    std::memcpy(base_ + va, &v, sizeof(T));
  }

 private:
  static constexpr uint8_t kCommitted = 0x80;

  // Host pages overlapping [va, va+size), as a byte offset range.
  void HostRange(gaddr va, uint64_t size, uint64_t* start, uint64_t* end) const;
  bool HostPageInUse(uint64_t host_off) const;
  bool InRange(gaddr va, uint64_t size) const {
    return va < size_ && size <= size_ - va;
  }

  uint8_t* base_ = nullptr;
  uint64_t size_ = 0;
  size_t host_page_ = 0;
  std::vector<uint8_t> pages_;  // one entry per guest page: Prot | kCommitted
};

}  // namespace recomp

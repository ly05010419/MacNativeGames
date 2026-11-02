#include "runtime/guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>

namespace recomp {

namespace {

// Preferred host placement keeps base() stable across runs, which makes
// guest pointers in logs and core dumps easy to read. 64 GiB is free on
// Linux and clear of macOS's 4 GiB __PAGEZERO and main image. Any other
// address works too; lifted code reads the base at runtime.
constexpr uintptr_t kPreferredBase = 0x1000000000ull;
// Larger arenas (x64 guests) and their guard go above that.
constexpr uintptr_t kPreferredBaseLarge = 0x20000000000ull;

constexpr int kReserveFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

}  // namespace

bool GuestMemory::Reserve(unsigned bits) {
  host_page_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (host_page_ < kPageSize || host_page_ % kPageSize) {
    std::fprintf(stderr, "GuestMemory: unsupported host page size %zu\n",
                 host_page_);
    return false;
  }

  const uint64_t size = 1ull << bits;
  const uint64_t span = bits > 32 ? 2 * size : size;  // with the guard
  void* hint = reinterpret_cast<void*>(bits > 32 ? kPreferredBaseLarge : kPreferredBase);
#ifdef MAP_FIXED_NOREPLACE
  void* p = mmap(hint, span, PROT_NONE, kReserveFlags | MAP_FIXED_NOREPLACE,
                 -1, 0);
  if (p == MAP_FAILED) p = mmap(nullptr, span, PROT_NONE, kReserveFlags, -1, 0);
#else
  // Without MAP_FIXED_NOREPLACE the hint is advisory; take what we get.
  void* p = mmap(hint, span, PROT_NONE, kReserveFlags, -1, 0);
#endif
  if (p == MAP_FAILED) {
    std::perror("GuestMemory::Reserve mmap");
    return false;
  }
  base_ = static_cast<uint8_t*>(p);
  size_ = size;
  pages_.assign(size / kPageSize, 0);
  return true;
}

void GuestMemory::HostRange(gaddr va, uint64_t size, uint64_t* start,
                            uint64_t* end) const {
  uint64_t mask = host_page_ - 1;
  *start = uint64_t(va) & ~mask;
  *end = (uint64_t(va) + size + mask) & ~mask;
}

bool GuestMemory::HostPageInUse(uint64_t host_off) const {
  for (uint64_t g = host_off / kPageSize, n = host_page_ / kPageSize; n--; g++)
    if (pages_[g] & kCommitted) return true;
  return false;
}

bool GuestMemory::Commit(gaddr va, uint64_t size, Prot prot) {
  if (!size) return true;
  if (!InRange(va, size)) return false;
  uint64_t s, e;
  HostRange(va, size, &s, &e);
  // Host pages are only ever "accessible" or not; guest protection is
  // bookkeeping. Fresh anonymous pages are zero, matching VirtualAlloc.
  if (mprotect(base_ + s, e - s, PROT_READ | PROT_WRITE) != 0) {
    std::perror("GuestMemory::Commit mprotect");
    return false;
  }
  uint64_t first = va / kPageSize, last = (uint64_t(va) + size - 1) / kPageSize;
  for (uint64_t g = first; g <= last; g++)
    pages_[g] = kCommitted | static_cast<uint8_t>(prot);
  return true;
}

bool GuestMemory::Protect(gaddr va, uint64_t size, Prot prot) {
  if (!size) return true;
  if (!InRange(va, size)) return false;
  uint64_t first = va / kPageSize, last = (uint64_t(va) + size - 1) / kPageSize;
  for (uint64_t g = first; g <= last; g++)
    if (!(pages_[g] & kCommitted)) return false;
  for (uint64_t g = first; g <= last; g++)
    pages_[g] = kCommitted | static_cast<uint8_t>(prot);
  return true;
}

void GuestMemory::Decommit(gaddr va, uint64_t size, bool zero) {
  if (!size || !InRange(va, size)) return;
  uint64_t first = va / kPageSize, last = (uint64_t(va) + size - 1) / kPageSize;
  for (uint64_t g = first; g <= last; g++) {
    if (zero && (pages_[g] & kCommitted))
      std::memset(base_ + g * kPageSize, 0, kPageSize);  // reads back as zero
    pages_[g] = 0;
  }
  // Return host pages no committed guest page still uses. Remapping (not
  // madvise) is used because MADV_DONTNEED does not zero on macOS.
  uint64_t s, e;
  HostRange(va, size, &s, &e);
  for (uint64_t h = s; h < e; h += host_page_) {
    if (HostPageInUse(h)) continue;
    mmap(base_ + h, host_page_, PROT_NONE, kReserveFlags | MAP_FIXED, -1, 0);
  }
}

bool GuestMemory::IsCommitted(gaddr va) const {
  return va < size_ && (pages_[va / kPageSize] & kCommitted);
}

Prot GuestMemory::QueryProt(gaddr va) const {
  return va < size_ ? static_cast<Prot>(pages_[va / kPageSize] & 0x7) : Prot::None;
}

}  // namespace recomp

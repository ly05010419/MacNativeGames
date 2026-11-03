#pragma once
// Guest memory management with Win32 semantics, all inside the arena.
// Addresses and sizes are 64-bit for x64 guests' sake.
//
// GuestAllocator: VirtualAlloc-style reservations (64 KiB granularity) and
// page commits. GuestHeap: HeapAlloc-style blocks carved from reserved
// segments. Block metadata lives on the host side, so guest overruns can't
// corrupt the allocator (nothing in the guest relies on heap headers).

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp {

class GuestAllocator {
 public:
  static constexpr uint64_t kGranularity = 0x10000;

  void Init(GuestMemory* mem, uint64_t lo, uint64_t hi);
  // Marks a range as permanently reserved (image, thunks, TEBs).
  void MarkReserved(uint64_t va, uint64_t size);

  // Reserves `size` bytes at `va` (rounded down to 64 KiB) or, if va == 0,
  // at the lowest (or highest with top_down) free address. Returns the base
  // or 0.
  uint64_t Reserve(uint64_t va, uint64_t size, bool top_down = false);
  // Commits pages inside a reservation. Returns false if any part of the
  // range is not reserved.
  bool Commit(uint64_t va, uint64_t size, Prot prot);
  bool Decommit(uint64_t va, uint64_t size);
  // Releases a whole reservation given its base. zero=false: see
  // GuestMemory::Decommit.
  bool Release(uint64_t base, bool zero = true);

  struct Region {
    uint64_t base = 0, size = 0;  // reservation containing the address
  };
  // Finds the reservation containing va; false if va is free.
  bool Find(uint64_t va, Region* out) const;
  // First free address >= va and the size of that free gap (for
  // VirtualQuery on unreserved memory).
  void FreeGap(uint64_t va, uint64_t* base, uint64_t* size) const;

  GuestMemory& mem() { return *mem_; }

 private:
  bool FindLocked(uint64_t va, Region* out) const;

  GuestMemory* mem_ = nullptr;
  uint64_t lo_ = 0, hi_ = 0;
  std::map<uint64_t, uint64_t> regions_;  // base -> size
  mutable std::recursive_mutex mu_;
};

class GuestHeap {
 public:
  // growable when max_size == 0 (HeapCreate semantics).
  GuestHeap(GuestAllocator& vm, uint64_t initial_size, uint64_t max_size);
  ~GuestHeap();

  uint64_t handle() const { return handle_; }

  uint64_t Alloc(uint64_t size, bool zero);
  bool Free(uint64_t p);
  // Returns the new address, or 0 on failure. in_place_only: fail rather
  // than move.
  uint64_t ReAlloc(uint64_t p, uint64_t size, bool zero, bool in_place_only);
  // Requested size of block p, or UINT64_MAX ((SIZE_T)-1) if p is not a
  // live block.
  uint64_t Size(uint64_t p) const;

 private:
  struct Block {
    uint64_t size;       // rounded size actually taken
    uint64_t requested;  // size the caller asked for (HeapSize)
  };

  bool Grow(uint64_t need);
  void InsertFree(uint64_t addr, uint64_t size);
  void EraseFree(uint64_t addr);
  bool IsSegmentStart(uint64_t addr) const;

  GuestAllocator& vm_;
  uint64_t max_size_;
  uint64_t handle_ = 0;
  std::vector<std::pair<uint64_t, uint64_t>> segments_;  // base, size
  std::map<uint64_t, Block> used_;
  std::map<uint64_t, uint64_t> free_;                // addr -> size
  std::multimap<uint64_t, uint64_t> free_by_size_;   // size -> addr
  mutable std::mutex mu_;
};

}  // namespace recomp

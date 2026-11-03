#pragma once
// Guest memory whose contents are produced on first access.
//
// A lazy region is a range of guest memory with host pages of its own
// (host-page aligned, nothing else in them). While a page of it is made
// inaccessible, the first access to it faults; the runtime's fault handler
// (dispatch.cpp) calls HandleFault before any guest exception dispatch,
// the region fills the page and makes it accessible, and the faulting
// instruction is retried. Guest code sees ordinary memory.
//
// LazyImage uses this for pixels that live elsewhere (a GPU render
// target): between Begin and End (a DirectDraw Lock and Unlock) they are
// read back only if the guest touches them, and only the rows it touched.
//
// Host code that hands a guest buffer to the kernel (read(2), write(2))
// must Touch it first: the kernel returns EFAULT on an inaccessible page
// instead of faulting.

#include <cstdint>
#include <functional>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp::lazy {

class Region {
 public:
  Region(GuestMemory& mem, gaddr va, uint32_t size);
  virtual ~Region();
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;

  gaddr va() const { return va_; }
  uint32_t size() const { return size_; }

 protected:
  friend bool HandleFault(gaddr addr);
  friend void Touch(gaddr va, uint32_t size);
  // Makes the host pages in [lo, hi) (host-page aligned, inside the
  // region) accessible, filling them first as needed. Called on the
  // faulting thread with the registry lock held.
  virtual void Fill(gaddr lo, gaddr hi) = 0;

  // Host page protection of [lo, hi), host-page aligned.
  void SetAccess(gaddr lo, gaddr hi, bool accessible);

  GuestMemory& mem_;
  gaddr va_;
  uint32_t size_;
  uint32_t page_;  // host page size
};

// Holds the registry lock: regions change state only under it, and a
// fault on a lazy page while this thread holds it is not handled (it
// would deadlock) but reported like any other fault.
class Guard {
 public:
  Guard();
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

// Registers/unregisters a region: the most derived class does, at the end
// of its constructor and first thing in its destructor.
void Register(Region* r);
void Unregister(Region* r);

// Fault handler entry: true if `addr` is in a lazy region, which has now
// made its page accessible (retry the access).
bool HandleFault(gaddr addr);

// Makes [va, va + size) accessible wherever it overlaps a lazy region,
// as an access by the guest would.
void Touch(gaddr va, uint32_t size);

// A pitch-linear image of `height` rows at `va` (host-page aligned),
// mirroring pixels kept elsewhere that `read` fetches.
class LazyImage final : public Region {
 public:
  struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
  };
  // Copies rect `r` of the source image into `dst` (r's top-left), rows
  // `pitch` bytes apart.
  using ReadFn = std::function<void(const Rect& r, uint8_t* dst, uint32_t pitch)>;

  LazyImage(GuestMemory& mem, gaddr va, uint32_t pitch, int width, int height,
            uint32_t bytes_per_pixel, ReadFn read);
  ~LazyImage() override;

  // The guest is about to access rect `r` (clipped to the image): its
  // bytes are read on first touch. read_only: the guest promised not to
  // write, End uploads nothing.
  void Begin(Rect r, bool read_only);
  // The guest is done. If it changed anything, `upload` is called (with
  // the registry lock held) with the band of rows of the rect it changed;
  // the band's bytes in memory are then all valid.
  void End(const std::function<void(const Rect&)>& upload);
  // Makes the bytes of rect `r` accessible for host code (Blt). During a
  // Begin/End, the guest's rect bytes among them are read first.
  void Settle(Rect r);

 private:
  enum : uint8_t { kOpen = 1, kPulled = 2 };

  void Fill(gaddr lo, gaddr hi) override;
  // Pages [p0, p1) of the image (a range of `state_`).
  void Open(uint32_t p0, uint32_t p1);
  void CloseAll();
  // Reads the rect bytes of the unpulled pages in [p0, p1) and opens them.
  void Pull(uint32_t p0, uint32_t p1);
  // Clips `r` to the image; false if empty. *p0, *p1: the pages holding
  // its bytes.
  bool Pages(Rect* r, uint32_t* p0, uint32_t* p1) const;
  // The pages holding bytes of the rect, [rect_p0_, rect_p1_).
  bool InRect(uint32_t p) const { return p >= rect_p0_ && p < rect_p1_; }
  // Calls f(row, byte offset, length, page) for each piece of each rect
  // row in rows [y0, y1), split at page boundaries.
  template <typename F>
  void ForEachPiece(int y0, int y1, F f) const;

  uint32_t pitch_, bpp_;
  int width_, height_;
  ReadFn read_;
  std::vector<uint8_t> state_;  // per host page
  uint32_t open_pages_ = 0;
  bool active_ = false, read_only_ = false;
  Rect rect_;
  uint32_t rect_p0_ = 0, rect_p1_ = 0;
  uint32_t faults_ = 0;          // pulls in this Begin/End
  std::vector<uint8_t> snap_;    // pulled bytes as read, at their offsets
  std::vector<uint8_t> staging_;
};

}  // namespace recomp::lazy

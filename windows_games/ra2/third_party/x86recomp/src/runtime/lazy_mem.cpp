#include "runtime/lazy_mem.h"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace recomp::lazy {

namespace {

std::mutex g_mu;
std::vector<Region*> g_regions;  // under g_mu
std::atomic<int> g_count{0};     // g_regions.size(), read without the lock
thread_local bool t_held = false;

}  // namespace

Guard::Guard() {
  g_mu.lock();
  t_held = true;
}

Guard::~Guard() {
  t_held = false;
  g_mu.unlock();
}

Region::Region(GuestMemory& mem, gaddr va, uint32_t size)
    : mem_(mem), va_(va), size_(size), page_(uint32_t(mem.host_page_size())) {
  if (va % page_ || size % page_) {
    std::fprintf(stderr, "lazy::Region %08X+%X is not host-page aligned\n", va, size);
    std::abort();
  }
}

Region::~Region() = default;

void Region::SetAccess(gaddr lo, gaddr hi, bool accessible) {
  if (hi <= lo) return;
  if (mprotect(mem_.ptr(lo), hi - lo, accessible ? PROT_READ | PROT_WRITE : PROT_NONE)) {
    std::perror("lazy::Region mprotect");
    std::abort();
  }
}

void Register(Region* r) {
  Guard g;
  g_regions.push_back(r);
  g_count.store(int(g_regions.size()));
}

void Unregister(Region* r) {
  Guard g;
  g_regions.erase(std::remove(g_regions.begin(), g_regions.end(), r), g_regions.end());
  g_count.store(int(g_regions.size()));
}

bool HandleFault(gaddr addr) {
  if (!g_count.load() || t_held) return false;
  Guard g;
  for (Region* r : g_regions) {
    if (addr - r->va_ >= r->size_) continue;
    gaddr lo = addr & ~(r->page_ - 1);
    r->Fill(lo, lo + r->page_);
    return true;
  }
  return false;
}

void Touch(gaddr va, uint32_t size) {
  if (!g_count.load() || !size) return;
  Guard g;
  uint64_t end = uint64_t(va) + size;
  for (Region* r : g_regions) {
    uint64_t lo = std::max<uint64_t>(va, r->va_);
    uint64_t hi = std::min<uint64_t>(end, uint64_t(r->va_) + r->size_);
    if (lo >= hi) continue;
    lo &= ~uint64_t(r->page_ - 1);
    hi = (hi + r->page_ - 1) & ~uint64_t(r->page_ - 1);
    r->Fill(gaddr(lo), gaddr(hi));
  }
}

// ---- LazyImage ------------------------------------------------------------------------
//
// Per host page: kOpen (accessible) and, during a Begin/End, kPulled (its
// rect bytes were read and snapshotted). Inside the rect, open == pulled
// during a Begin/End; pages outside it open without being filled (the
// guest has no business there; they hold whatever they held).
//
// A page of a row is rarely a whole row (rows are pitch bytes, pages 4 or
// 16 KiB, neither divides the other in general), so a pull reads whole
// rect rows but copies only the bytes of the pages it opens: a page
// already open may hold the guest's writes.

namespace {
uint32_t RoundUp(uint64_t v, uint32_t a) { return uint32_t((v + a - 1) / a * a); }

// A first touch pulls the whole rect if it is at most this big (a cursor
// or sprite rect: one read-back instead of a fault per page).
constexpr uint32_t kWholeRectBytes = 1u << 20;
}  // namespace

LazyImage::LazyImage(GuestMemory& mem, gaddr va, uint32_t pitch, int width, int height,
                     uint32_t bytes_per_pixel, ReadFn read)
    : Region(mem, va, RoundUp(uint64_t(pitch) * uint32_t(height), uint32_t(mem.host_page_size()))),
      pitch_(pitch),
      bpp_(bytes_per_pixel),
      width_(width),
      height_(height),
      read_(std::move(read)) {
  state_.assign(size_ / page_, kOpen);
  open_pages_ = uint32_t(state_.size());
  Register(this);
}

LazyImage::~LazyImage() {
  Unregister(this);
  // Accessible again for whoever frees the memory.
  Open(0, uint32_t(state_.size()));
}

template <typename F>
void LazyImage::ForEachPiece(int y0, int y1, F f) const {
  uint32_t len = uint32_t(rect_.right - rect_.left) * bpp_;
  for (int y = y0; y < y1; y++) {
    uint32_t off = uint32_t(y) * pitch_ + uint32_t(rect_.left) * bpp_, end = off + len;
    while (off < end) {
      uint32_t p = off / page_;
      uint32_t n = std::min(end, (p + 1) * page_) - off;
      f(y, off, n, p);
      off += n;
    }
  }
}

void LazyImage::Open(uint32_t p0, uint32_t p1) {
  // One mprotect per run of closed pages.
  for (uint32_t p = p0; p < p1;) {
    if (state_[p] & kOpen) {
      p++;
      continue;
    }
    uint32_t q = p;
    while (q < p1 && !(state_[q] & kOpen)) state_[q++] |= kOpen;
    SetAccess(va_ + p * page_, va_ + q * page_, true);
    open_pages_ += q - p;
    p = q;
  }
}

void LazyImage::CloseAll() {
  if (!open_pages_) return;
  for (uint8_t& s : state_) s &= uint8_t(~kOpen);
  SetAccess(va_, va_ + size_, false);
  open_pages_ = 0;
}

bool LazyImage::Pages(Rect* r, uint32_t* p0, uint32_t* p1) const {
  r->left = std::max(r->left, 0);
  r->top = std::max(r->top, 0);
  r->right = std::min(r->right, width_);
  r->bottom = std::min(r->bottom, height_);
  *p0 = *p1 = 0;
  if (r->empty()) return false;
  *p0 = (uint32_t(r->top) * pitch_ + uint32_t(r->left) * bpp_) / page_;
  *p1 = (uint32_t(r->bottom - 1) * pitch_ + uint32_t(r->right) * bpp_ - 1) / page_ + 1;
  return true;
}

void LazyImage::Begin(Rect r, bool read_only) {
  Guard g;
  Pages(&r, &rect_p0_, &rect_p1_);
  rect_ = r;
  read_only_ = read_only;
  active_ = true;
  faults_ = 0;
  for (uint8_t& s : state_) s &= uint8_t(~kPulled);
  CloseAll();
}

void LazyImage::Pull(uint32_t p0, uint32_t p1) {
  p0 = std::max(p0, rect_p0_);
  p1 = std::min(p1, rect_p1_);
  while (p0 < p1 && (state_[p0] & kPulled)) p0++;
  while (p1 > p0 && (state_[p1 - 1] & kPulled)) p1--;
  if (p0 >= p1) return;
  // Rect rows with bytes in [p0, p1).
  int y0 = std::max(rect_.top, int(uint64_t(p0) * page_ / pitch_));
  int y1 = std::min(rect_.bottom, int((uint64_t(p1) * page_ - 1) / pitch_) + 1);
  uint32_t row = uint32_t(rect_.right - rect_.left) * bpp_;
  if (y0 < y1) {
    staging_.resize(size_t(row) * uint32_t(y1 - y0));
    read_({rect_.left, y0, rect_.right, y1}, staging_.data(), row);
  }
  if (snap_.size() < size_) snap_.resize(size_);
  std::vector<uint32_t> fresh;  // pages to fill: closed ones in [p0, p1)
  for (uint32_t p = p0; p < p1; p++)
    if (!(state_[p] & kPulled)) fresh.push_back(p);
  Open(p0, p1);
  uint8_t* base = mem_.ptr(va_);
  ForEachPiece(y0, y1, [&](int y, uint32_t off, uint32_t n, uint32_t p) {
    if (p < p0 || p >= p1 || (state_[p] & kPulled)) return;
    const uint8_t* src = staging_.data() + size_t(y - y0) * row +
                         (off - uint32_t(y) * pitch_ - uint32_t(rect_.left) * bpp_);
    std::memcpy(base + off, src, n);
    std::memcpy(snap_.data() + off, src, n);
  });
  for (uint32_t p : fresh) state_[p] |= kPulled;
}

void LazyImage::Fill(gaddr lo, gaddr hi) {
  uint32_t p0 = (lo - va_) / page_, p1 = (hi - va_) / page_;
  for (uint32_t p = p0; p < p1; p++) {
    if (state_[p] & kOpen) continue;
    if (!active_ || !InRect(p)) {
      Open(p, p + 1);
      continue;
    }
    // A guest walking the rect would fault on every page: each fault
    // pulls twice as much as the one before, in aligned blocks (so a walk
    // in either direction takes about log2(pages) faults).
    uint32_t b0 = rect_p0_, b1 = rect_p1_;
    if (uint64_t(b1 - b0) * page_ > kWholeRectBytes) {
      uint32_t n = 1u << std::min(faults_, 24u);
      b0 = std::max(b0, p & ~(n - 1));
      b1 = std::min(b1, (p & ~(n - 1)) + n);
    }
    faults_++;
    Pull(b0, b1);
    Open(p, p + 1);  // the pull opened it unless it had no rect bytes
  }
}

void LazyImage::End(const std::function<void(const Rect&)>& upload) {
  Guard g;
  if (!active_) return;
  active_ = false;
  if (read_only_ || !faults_) return;
  // Rows whose pulled bytes the guest changed.
  const uint8_t* base = mem_.ptr(va_);
  int top = rect_.bottom, bottom = rect_.top;
  ForEachPiece(rect_.top, rect_.bottom, [&](int y, uint32_t off, uint32_t n, uint32_t p) {
    if (!(state_[p] & kPulled) || (y >= top && y < bottom)) return;
    if (std::memcmp(base + off, snap_.data() + off, n)) {
      top = std::min(top, y);
      bottom = std::max(bottom, y + 1);
    }
  });
  if (top >= bottom) return;
  // The band goes back whole: the rest of its bytes must be read first.
  Rect band{rect_.left, top, rect_.right, bottom};
  Pull(uint32_t(uint64_t(top) * pitch_ / page_),
       uint32_t((uint64_t(bottom - 1) * pitch_ + uint32_t(rect_.right) * bpp_ - 1) / page_ + 1));
  upload(band);
}

void LazyImage::Settle(Rect r) {
  Guard g;
  uint32_t p0, p1;
  if (!Pages(&r, &p0, &p1)) return;
  if (active_) {
    faults_++;
    Pull(p0, p1);
  }
  Open(p0, p1);
}

}  // namespace recomp::lazy

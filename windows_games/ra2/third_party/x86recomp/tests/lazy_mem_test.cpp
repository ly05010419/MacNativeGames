// lazy_mem_test: LazyImage over a fake source image, with the runtime's
// fault path reduced to its lazy part (a SIGSEGV/SIGBUS handler that calls
// lazy::HandleFault and retries). Checks that:
//   - a Begin/End the guest does not touch reads and uploads nothing,
//     and one touch reads about one host page's rows;
//   - the guest reads the source's pixels inside the rect, however it
//     walks it, in about log2(pages) read-backs;
//   - after End + upload, the source equals the source before with exactly
//     the guest's writes applied (random rects, pitches that are not a
//     multiple of the page size and the reverse, sub-rects);
//   - several threads, each with its own image, work at once;
//   - Touch makes a range usable by the kernel (read(2) into it), and so
//     does Settle, with the guest's edits and the source's pixels.

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"
#include "runtime/lazy_mem.h"

using namespace recomp;

namespace {

GuestMemory g_mem;
GuestAllocator g_vm;
std::atomic<int> g_failures{0};

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void OnFault(int, siginfo_t* si, void*) {
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_mem.base());
  if (a - base < g_mem.size() && lazy::HandleFault(uint32_t(a - base))) return;
  const char m[] = "unexpected fault\n";
  (void)!write(2, m, sizeof m - 1);
  _exit(2);
}

// A source image (the "GPU") and a LazyImage mirroring it in guest memory.
struct Fixture {
  int w, h;
  uint32_t bpp, pitch;
  std::vector<uint8_t> src;  // w*bpp per row, packed
  gaddr va = 0;
  uint32_t size = 0;
  int reads = 0;
  size_t bytes_read = 0;
  std::unique_ptr<lazy::LazyImage> img;

  Fixture(int w_, int h_, uint32_t bpp_, uint32_t seed) : w(w_), h(h_), bpp(bpp_) {
    pitch = (uint32_t(w) * bpp + 3) & ~3u;
    src.resize(size_t(w) * bpp * uint32_t(h));
    std::mt19937 rng(seed);
    for (uint8_t& b : src) b = uint8_t(rng());
    uint32_t hp = uint32_t(g_mem.host_page_size());
    size = (pitch * uint32_t(h) + hp - 1) / hp * hp;
    va = g_vm.Reserve(0, size);
    g_vm.Commit(va, size, Prot::RW);
    img = std::make_unique<lazy::LazyImage>(
        g_mem, va, pitch, w, h, bpp,
        [this](const lazy::LazyImage::Rect& r, uint8_t* dst, uint32_t dpitch) {
          reads++;
          bytes_read += size_t(r.right - r.left) * bpp * size_t(r.bottom - r.top);
          for (int y = r.top; y < r.bottom; y++)
            std::memcpy(dst + size_t(y - r.top) * dpitch,
                        &src[(size_t(y) * w + r.left) * bpp], size_t(r.right - r.left) * bpp);
        });
  }
  ~Fixture() {
    img.reset();
    g_vm.Release(va);
  }
  uint8_t* px(int x, int y) { return g_mem.ptr(va + uint32_t(y) * pitch + uint32_t(x) * bpp); }
  uint8_t* sp(int x, int y) { return &src[(size_t(y) * w + x) * bpp]; }
  void End() {
    img->End([this](const lazy::LazyImage::Rect& b) {
      for (int y = b.top; y < b.bottom; y++)
        std::memcpy(sp(b.left, y), px(b.left, y), size_t(b.right - b.left) * bpp);
    });
  }
};

void Untouched() {
  Fixture f(1366, 300, 4, 1);
  for (int i = 0; i < 3; i++) {
    f.img->Begin({0, 0, f.w, f.h}, false);
    f.End();
  }
  CHECK(f.reads == 0);
}

// One pixel touched in a big rect reads about one host page's rows.
void OneTouch() {
  Fixture f(1280, 720, 4, 4);
  f.img->Begin({0, 0, f.w, f.h}, false);
  CHECK(!std::memcmp(f.px(600, 300), f.sp(600, 300), f.bpp));
  f.End();
  CHECK(f.reads == 1);
  CHECK(f.bytes_read <= (g_mem.host_page_size() / f.pitch + 2) * f.pitch);
}

void SweepReads(bool backwards) {
  Fixture f(1280, 720, 4, 2);
  std::vector<uint8_t> before = f.src;
  f.img->Begin({0, 0, f.w, f.h}, false);
  bool same = true;
  for (int i = 0; i < f.h; i++) {
    int y = backwards ? f.h - 1 - i : i;
    for (int x = 0; x < f.w; x++) same &= !std::memcmp(f.px(x, y), f.sp(x, y), f.bpp);
  }
  CHECK(same);
  // 900 host pages at 4 KiB, 225 at 16 KiB: doubling blocks.
  CHECK(f.reads <= 16);
  f.End();
  CHECK(f.src == before);
}

void RandomEdits(int w, int h, uint32_t bpp, uint32_t seed, int rounds) {
  Fixture f(w, h, bpp, seed);
  std::vector<uint8_t> model = f.src;
  std::mt19937 rng(seed * 7 + 1);
  for (int round = 0; round < rounds; round++) {
    lazy::LazyImage::Rect r{0, 0, w, h};
    if (rng() % 2) {
      int x0 = int(rng() % uint32_t(w)), y0 = int(rng() % uint32_t(h));
      r = {x0, y0, x0 + 1 + int(rng() % uint32_t(w - x0)), y0 + 1 + int(rng() % uint32_t(h - y0))};
    }
    bool ro = rng() % 5 == 0;
    f.img->Begin(r, ro);
    std::vector<uint8_t> local = model;  // what the guest should read
    int ops = int(rng() % 200);
    for (int i = 0; i < ops; i++) {
      int x = r.left + int(rng() % uint32_t(r.right - r.left));
      int y = r.top + int(rng() % uint32_t(r.bottom - r.top));
      uint8_t* lp = &local[(size_t(y) * w + x) * bpp];
      if (rng() % 3 == 0 || ro) {
        CHECK(!std::memcmp(f.px(x, y), lp, bpp));
      } else {
        for (uint32_t b = 0; b < bpp; b++) lp[b] = uint8_t(rng());
        std::memcpy(f.px(x, y), lp, bpp);
      }
    }
    f.End();
    if (!ro) model = local;
    CHECK(f.src == model);
    if (f.src != model) return;
  }
}

void Threads() {
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; t++)
    ts.emplace_back([t] { RandomEdits(640 + 37 * t, 200, t % 2 ? 2 : 4, 100 + uint32_t(t), 60); });
  for (auto& t : ts) t.join();
}

void TouchForKernel() {
  Fixture f(800, 64, 4, 3);
  int fds[2];
  CHECK(pipe(fds) == 0);
  std::vector<uint8_t> data(1000, 0xAB);
  CHECK(write(fds[1], data.data(), data.size()) == ssize_t(data.size()));
  f.img->Begin({0, 0, f.w, f.h}, false);
  gaddr dst = f.va + 10 * f.pitch + 40;
  lazy::Touch(dst, 1000);
  CHECK(read(fds[0], g_mem.ptr(dst), 1000) == 1000);
  f.End();
  CHECK(!std::memcmp(f.sp(10, 10), data.data(), 1000));
  close(fds[0]);
  close(fds[1]);
}

// Settle(r) makes r's bytes usable by host code (here: the kernel, which
// does not fault) with the guest's edits and the source's pixels, during a
// lock and outside one.
void SettleForHost() {
  Fixture f(1366, 120, 4, 5);
  int fds[2];
  CHECK(pipe(fds) == 0);
  auto row_via_kernel = [&](int x0, int x1, int y) {
    size_t n = size_t(x1 - x0) * f.bpp;
    std::vector<uint8_t> out(n);
    bool ok = write(fds[1], f.px(x0, y), n) == ssize_t(n) && read(fds[0], out.data(), n) == ssize_t(n);
    CHECK(ok);
    return out;
  };
  f.img->Begin({0, 0, f.w, f.h}, false);
  std::memset(f.px(100, 50), 0x5A, 16);  // the guest writes 4 pixels
  f.img->Settle({90, 40, 300, 60});
  for (int y = 40; y < 60; y++) {
    std::vector<uint8_t> got = row_via_kernel(90, 300, y);
    for (int x = 90; x < 300; x++) {
      bool edited = y == 50 && x >= 100 && x < 104;
      const uint8_t* want = edited ? f.px(x, y) : f.sp(x, y);
      CHECK(!std::memcmp(&got[size_t(x - 90) * f.bpp], want, f.bpp));
      if (edited) CHECK(f.px(x, y)[0] == 0x5A);
    }
  }
  f.End();
  CHECK(f.sp(101, 50)[0] == 0x5A);
  f.img->Begin({0, 0, f.w, f.h}, false);
  f.End();
  f.img->Settle({0, 100, f.w, 110});  // outside a lock: opened, nothing read
  int reads = f.reads;
  row_via_kernel(0, f.w, 105);
  CHECK(f.reads == reads);
  close(fds[0]);
  close(fds[1]);
}

}  // namespace

int main() {
  if (!g_mem.Reserve()) return 1;
  g_vm.Init(&g_mem, 0x10000, 0x80000000u);
  stack_t ss{};
  ss.ss_size = 1 << 20;
  ss.ss_sp = std::malloc(ss.ss_size);
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  Untouched();
  OneTouch();
  SweepReads(false);
  SweepReads(true);
  RandomEdits(1366, 97, 4, 11, 200);  // pitch 5464: not a page divisor
  RandomEdits(1024, 160, 2, 12, 200); // pitch 2048: divides the page
  RandomEdits(5120, 40, 4, 13, 100);  // pitch 20480: over a page
  RandomEdits(3, 5000, 4, 14, 200);   // many rows per page
  Threads();
  TouchForKernel();
  SettleForHost();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures.load());
    return 1;
  }
  std::printf("lazy_mem: ok\n");
  return 0;
}

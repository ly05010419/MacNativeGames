// bink_format_test: compact movies (runtime/hle/bink_format.h) as the
// packaging step writes them (scripts/ios/compact_movies.py in a game
// project): split into the Bink header and the container, or refused.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/hle/bink_format.h"

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    g_failed++;
  }
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i)));
}

// A Bink header ('i' revision, so no extra 4 bytes) with `tracks` tracks.
std::vector<uint8_t> BinkHeader(uint32_t tracks) {
  std::vector<uint8_t> h = {'B', 'I', 'K', 'i'};
  Put32(h, 123456);  // size of the rest
  Put32(h, 3580);    // frames
  Put32(h, 0);       // largest frame
  Put32(h, 0);
  Put32(h, 640);     // width
  Put32(h, 480);     // height
  Put32(h, 15);      // rate
  Put32(h, 1);       // divider
  Put32(h, 0);       // flags
  Put32(h, tracks);
  for (uint32_t i = 0; i < tracks; i++) Put32(h, 0);              // max packet
  for (uint32_t i = 0; i < tracks; i++) Put32(h, 22050 | 1u << 29);  // rate, flags
  for (uint32_t i = 0; i < tracks; i++) Put32(h, 100 + i);        // IDs
  return h;
}

std::vector<uint8_t> MakeCompact(const std::vector<uint8_t>& header,
                             const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> v = {'X', 'R', 'M', 'V'};
  Put32(v, uint32_t(4 + header.size() + payload.size()));
  Put32(v, uint32_t(header.size()));
  v.insert(v.end(), header.begin(), header.end());
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

}  // namespace

int main() {
  using namespace recomp::bink;
  const std::vector<uint8_t> header = BinkHeader(2);
  const std::vector<uint8_t> payload = {0x1A, 0x45, 0xDF, 0xA3, 1, 2, 3};  // Matroska's ID first
  std::vector<uint8_t> data = MakeCompact(header, payload);

  Check(IsMovie(data.data()), "a compact movie is a movie");
  Check(IsMovie(header.data()), "a Bink movie is a movie");
  Check(Le32(data.data() + 4) + 8 == data.size(), "its size at +4 covers it all");

  std::vector<uint8_t> got;
  Check(SplitCompact(data, &got) == Compact::Yes, "split");
  Check(got == header, "the Bink header comes back whole");
  Check(data == payload, "the container is what is left");
  Check(Le32(got.data() + 8) == 3580 && Le32(got.data() + 20) == 640, "frames and width");

  std::vector<uint8_t> bink = header;
  Check(SplitCompact(bink, &got) == Compact::No && bink == header, "a Bink movie is left alone");

  std::vector<uint8_t> shortened = MakeCompact(header, payload);
  shortened.resize(12 + 20);
  Check(SplitCompact(shortened, &got) == Compact::Broken, "a header cut short is refused");

  std::vector<uint8_t> tiny = MakeCompact(std::vector<uint8_t>(header.begin(), header.begin() + 40),
                                      payload);
  Check(SplitCompact(tiny, &got) == Compact::Broken, "a header under 44 bytes is refused");

  std::vector<uint8_t> wrong = MakeCompact(header, payload);
  wrong[12] = 'X';
  Check(SplitCompact(wrong, &got) == Compact::Broken, "no Bink header inside is refused");

  if (g_failed) return 1;
  std::printf("bink_format: all passed\n");
  return 0;
}

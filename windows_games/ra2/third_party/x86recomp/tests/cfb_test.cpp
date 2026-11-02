// cfb_test: compound files (runtime/cfb.h). Expected values are from
// [MS-CFB] (header fields 2.2, special sector numbers 2.1, directory
// entries 2.6, name order 2.6.4), worked out by hand, plus round trips
// through Write and Read.

#include "runtime/cfb.h"

#include <cstdio>
#include <cstring>
#include <string>

using recomp::cfb::Entry;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

uint16_t Le16(const std::vector<uint8_t>& f, size_t at) { return uint16_t(f[at] | f[at + 1] << 8); }
uint32_t Le32(const std::vector<uint8_t>& f, size_t at) {
  return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8 | uint32_t(f[at + 2]) << 16 |
         uint32_t(f[at + 3]) << 24;
}

std::unique_ptr<Entry> Stream(const char16_t* name, size_t size, uint8_t seed) {
  auto e = std::make_unique<Entry>();
  e->type = Entry::Type::Stream;
  e->name = name;
  for (size_t i = 0; i < size; i++) e->data.push_back(uint8_t(seed + i * 7));
  return e;
}

void TestNames() {
  using recomp::cfb::CompareNames;
  // Shorter names sort first, whatever their characters.
  Check(CompareNames(u"Z", u"AA") < 0, "shorter name first");
  Check(CompareNames(u"abc", u"ABC") == 0, "case-insensitive");
  Check(CompareNames(u"ABD", u"abc") > 0, "upper-cased order");
  Check(recomp::cfb::ValidName(u"CONTENTS"), "plain name valid");
  Check(!recomp::cfb::ValidName(u"a/b"), "slash invalid");
  Check(!recomp::cfb::ValidName(u""), "empty invalid");
  Check(!recomp::cfb::ValidName(std::u16string(32, u'x')), "32 characters invalid");
}

// An empty root: one header and three sectors (FAT, directory, nothing
// else), the FAT marking itself and ending the directory's chain.
void TestEmptyLayout() {
  Entry root;
  root.type = Entry::Type::Root;
  root.name = u"Root Entry";
  const std::vector<uint8_t> f = recomp::cfb::Write(root);
  Check(f.size() == 512 * 3, "empty file: header + FAT + directory");
  static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  Check(std::memcmp(f.data(), sig, 8) == 0, "signature");
  Check(Le16(f, 0x18) == 0x3E && Le16(f, 0x1A) == 3, "version 3.62");
  Check(Le16(f, 0x1C) == 0xFFFE, "byte order mark");
  Check(Le16(f, 0x1E) == 9 && Le16(f, 0x20) == 6, "sector shifts");
  Check(Le32(f, 0x28) == 0, "no directory sector count in version 3");
  Check(Le32(f, 0x2C) == 1, "one FAT sector");
  Check(Le32(f, 0x30) == 1, "directory at sector 1");
  Check(Le32(f, 0x38) == 4096, "mini stream cutoff");
  Check(Le32(f, 0x3C) == 0xFFFFFFFE && Le32(f, 0x40) == 0, "no mini FAT");
  Check(Le32(f, 0x44) == 0xFFFFFFFE && Le32(f, 0x48) == 0, "no DIFAT sectors");
  Check(Le32(f, 0x4C) == 0 && Le32(f, 0x50) == 0xFFFFFFFF, "header DIFAT: FAT at 0, rest free");
  // FAT (sector 0, file offset 512): FATSECT, ENDOFCHAIN, then FREESECT.
  Check(Le32(f, 512) == 0xFFFFFFFD, "FAT marks its own sector");
  Check(Le32(f, 516) == 0xFFFFFFFE, "directory chain ends");
  Check(Le32(f, 520) == 0xFFFFFFFF, "free sector");
  // Root entry (sector 1, file offset 1024).
  const size_t d = 1024;
  Check(Le16(f, d) == 'R' && Le16(f, d + 0x40) == 22, "root name and its byte length");
  Check(f[d + 0x42] == 5 && f[d + 0x43] == 1, "root type, black");
  Check(Le32(f, d + 0x4C) == 0xFFFFFFFF, "root has no children");
  Check(Le32(f, d + 0x74) == 0xFFFFFFFE && Le32(f, d + 0x78) == 0, "no mini stream");
  // The other three entries of the sector are free.
  Check(f[d + 128 + 0x42] == 0 && Le32(f, d + 128 + 0x44) == 0xFFFFFFFF, "free entry");
}

// One 100-byte stream: two mini sectors, chained in the mini FAT.
void TestMiniLayout() {
  Entry root;
  root.type = Entry::Type::Root;
  root.name = u"Root Entry";
  root.children.push_back(Stream(u"CONTENTS", 100, 1));
  const std::vector<uint8_t> f = recomp::cfb::Write(root);
  // Sectors: FAT 0, directory 1, mini FAT 2, mini stream 3.
  Check(f.size() == 512 * 5, "header + 4 sectors");
  Check(Le32(f, 0x3C) == 2 && Le32(f, 0x40) == 1, "mini FAT at 2");
  Check(Le32(f, 512 + 4 * 2) == 0xFFFFFFFE && Le32(f, 512 + 4 * 3) == 0xFFFFFFFE,
        "one-sector chains");
  Check(Le32(f, 512 * 3) == 1 && Le32(f, 512 * 3 + 4) == 0xFFFFFFFE, "mini FAT chain 0 -> 1");
  const size_t root_e = 1024, s_e = 1024 + 128;
  Check(Le32(f, root_e + 0x4C) == 1, "root's child is entry 1");
  Check(Le32(f, root_e + 0x74) == 3 && Le32(f, root_e + 0x78) == 128, "mini stream: 2 x 64");
  Check(f[s_e + 0x42] == 2 && Le32(f, s_e + 0x74) == 0 && Le32(f, s_e + 0x78) == 100,
        "stream entry");
  Check(f[512 * 4] == 1 && f[512 * 4 + 99] == uint8_t(1 + 99 * 7), "stream bytes in mini sectors");
}

void TestRoundTrip() {
  Entry root;
  root.type = Entry::Type::Root;
  root.name = u"Root Entry";
  root.mtime = 0x01D7000012345678ull;
  for (int i = 0; i < 9; i++) {
    std::u16string n = u"S";
    n += char16_t(u'a' + i);
    root.children.push_back(Stream(n.c_str(), size_t(i) * 1500, uint8_t(i)));
  }
  root.children.push_back(Stream(u"CONTENTS", 3'000'000, 42));  // in sectors of its own
  auto sub = std::make_unique<Entry>();
  sub->type = Entry::Type::Storage;
  sub->name = u"Sub";
  sub->clsid[0] = 0xAB;
  sub->ctime = 7;
  sub->state_bits = 3;
  sub->children.push_back(Stream(u"x", 4096, 9));  // exactly the cutoff: a big stream
  sub->children.push_back(Stream(u"y", 4095, 8));  // just under: mini
  root.children.push_back(std::move(sub));

  const std::vector<uint8_t> f = recomp::cfb::Write(root);
  Entry back;
  std::string err;
  Check(recomp::cfb::Read(f, &back, &err), ("read back: " + err).c_str());
  Check(back.type == Entry::Type::Root && back.mtime == root.mtime, "root kept");
  Check(back.children.size() == root.children.size(), "child count");
  for (const auto& c : root.children) {
    const Entry* b = back.Find(c->name);
    Check(b && b->type == c->type && b->data == c->data, "child kept");
  }
  const Entry* s = back.Find(u"sub");
  Check(s && s->clsid[0] == 0xAB && s->ctime == 7 && s->state_bits == 3, "storage fields kept");
  Check(s && s->Find(u"X") && s->Find(u"X")->data.size() == 4096, "cutoff stream kept");
  Check(s && s->Find(u"y") && s->Find(u"y")->data == root.children.back()->children[1]->data,
        "mini stream kept");
  // A second write of what was read is the same file.
  Check(recomp::cfb::Write(back) == f, "write is deterministic");
}

// Over 109 FAT sectors (14 MB of data): the FAT is listed in DIFAT sectors.
void TestDifat() {
  Entry root;
  root.type = Entry::Type::Root;
  root.name = u"Root Entry";
  root.children.push_back(Stream(u"big", 15'000'000, 3));
  const std::vector<uint8_t> f = recomp::cfb::Write(root);
  Check(Le32(f, 0x2C) > 109, "more than 109 FAT sectors");
  Check(Le32(f, 0x48) >= 1 && Le32(f, 0x44) != 0xFFFFFFFE, "DIFAT sectors used");
  Entry back;
  std::string err;
  Check(recomp::cfb::Read(f, &back, &err), ("DIFAT read back: " + err).c_str());
  Check(back.Find(u"big") && back.Find(u"big")->data == root.children[0]->data, "big stream kept");
}

void TestRejects() {
  Entry e;
  std::string err;
  Check(!recomp::cfb::Read(std::vector<uint8_t>(100, 0), &e, &err), "short file rejected");
  std::vector<uint8_t> f(1536, 0);
  Check(!recomp::cfb::Read(f, &e, &err), "no signature rejected");
  Entry root;
  root.type = Entry::Type::Root;
  root.name = u"Root Entry";
  root.children.push_back(Stream(u"a", 10, 1));
  f = recomp::cfb::Write(root);
  // A directory chain pointing at itself.
  f[512 + 4] = 1, f[512 + 5] = 0, f[512 + 6] = 0, f[512 + 7] = 0;
  Check(!recomp::cfb::Read(f, &e, &err), "cyclic chain rejected");
}

}  // namespace

int main() {
  TestNames();
  TestEmptyLayout();
  TestMiniLayout();
  TestRoundTrip();
  TestDifat();
  TestRejects();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cfb: all passed\n");
  return 0;
}

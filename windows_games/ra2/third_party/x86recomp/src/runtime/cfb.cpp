#include "runtime/cfb.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace recomp::cfb {
namespace {

// [MS-CFB] 2.1: sector numbers with a meaning of their own; 2.6.1: no
// directory entry.
constexpr uint32_t kMaxRegSect = 0xFFFFFFFA, kDifSect = 0xFFFFFFFC, kFatSect = 0xFFFFFFFD,
                   kEndOfChain = 0xFFFFFFFE, kFreeSect = 0xFFFFFFFF, kNoStream = 0xFFFFFFFF;
constexpr uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr uint32_t kHeaderDifat = 109;  // FAT sector numbers in the header
constexpr uint32_t kDirEntrySize = 128, kMiniSectorSize = 64;
constexpr uint32_t kSectorSize = 512;   // what Write makes (version 3)
constexpr uint8_t kRed = 0, kBlack = 1;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) { return Le32(p) | uint64_t(Le32(p + 4)) << 32; }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
  Put32(p, uint32_t(v));
  Put32(p + 4, uint32_t(v >> 32));
}

char16_t Upper(char16_t c) { return c >= u'a' && c <= u'z' ? char16_t(c - 32) : c; }

// ---- reading -----------------------------------------------------------------

struct RawEntry {
  std::u16string name;
  uint8_t type = 0;
  uint32_t left = kNoStream, right = kNoStream, child = kNoStream;
  uint8_t clsid[16] = {};
  uint32_t state_bits = 0;
  uint64_t ctime = 0, mtime = 0, size = 0;
  uint32_t start = 0;
};

class Reader {
 public:
  Reader(const std::vector<uint8_t>& f, std::string* err) : f_(f), err_(err) {}

  bool Run(Entry* root) {
    if (f_.size() < kSectorSize) return Fail("shorter than a header");
    const uint8_t* h = f_.data();
    if (std::memcmp(h, kSignature, 8) != 0) return Fail("no compound file signature");
    const uint16_t major = Le16(h + 0x1A), shift = Le16(h + 0x1E);
    if (Le16(h + 0x1C) != 0xFFFE) return Fail("not little-endian");
    if (!((major == 3 && shift == 9) || (major == 4 && shift == 12)))
      return Fail("unknown version or sector size");
    if (Le16(h + 0x20) != 6) return Fail("mini sectors are not 64 bytes");
    ss_ = 1u << shift;
    // The header takes the first sector's place.
    nsect_ = f_.size() < ss_ ? 0 : uint32_t((f_.size() - ss_) / ss_);
    cutoff_ = Le32(h + 0x38);
    if (!ReadFat(h)) return false;
    std::vector<uint8_t> dir;
    if (!ReadChain(Le32(h + 0x30), UINT64_MAX, &dir)) return false;
    for (size_t off = 0; off + kDirEntrySize <= dir.size(); off += kDirEntrySize)
      raw_.push_back(Parse(dir.data() + off, major));
    if (raw_.empty() || raw_[0].type != uint8_t(Entry::Type::Root))
      return Fail("no root entry");
    // The mini FAT and the mini stream (the root's data).
    std::vector<uint8_t> mf;
    const uint32_t mf_start = Le32(h + 0x3C);
    if (mf_start != kEndOfChain && !ReadChain(mf_start, UINT64_MAX, &mf)) return false;
    for (size_t i = 0; i + 4 <= mf.size(); i += 4) minifat_.push_back(Le32(mf.data() + i));
    if (raw_[0].size && !ReadChain(raw_[0].start, raw_[0].size, &ministream_)) return false;
    visited_.assign(raw_.size(), false);
    visited_[0] = true;
    Fill(raw_[0], root);
    return Children(raw_[0].child, root);
  }

 private:
  bool Fail(const char* why) {
    *err_ = why;
    return false;
  }

  const uint8_t* Sector(uint32_t s) const { return f_.data() + size_t(s + 1) * ss_; }

  bool ReadFat(const uint8_t* h) {
    const uint32_t nfat = Le32(h + 0x2C), per = ss_ / 4;
    std::vector<uint32_t> fat_sectors;
    for (uint32_t i = 0; i < kHeaderDifat && fat_sectors.size() < nfat; i++)
      fat_sectors.push_back(Le32(h + 0x4C + 4 * i));
    uint32_t d = Le32(h + 0x44);
    for (uint32_t k = 0; k < Le32(h + 0x48) && fat_sectors.size() < nfat; k++) {
      if (d >= nsect_) return Fail("DIFAT chain leaves the file");
      const uint8_t* p = Sector(d);
      for (uint32_t i = 0; i + 1 < per && fat_sectors.size() < nfat; i++)
        fat_sectors.push_back(Le32(p + 4 * i));
      d = Le32(p + 4 * (per - 1));
    }
    if (fat_sectors.size() < nfat) return Fail("DIFAT lists fewer FAT sectors than the header");
    for (uint32_t s : fat_sectors) {
      if (s >= nsect_) return Fail("FAT sector outside the file");
      const uint8_t* p = Sector(s);
      for (uint32_t i = 0; i < per; i++) fat_.push_back(Le32(p + 4 * i));
    }
    return true;
  }

  // The sectors from `start` along the FAT, `size` bytes of them (all of
  // them for UINT64_MAX).
  bool ReadChain(uint32_t start, uint64_t size, std::vector<uint8_t>* out) {
    out->clear();
    uint32_t s = start;
    for (uint32_t steps = 0; s != kEndOfChain && out->size() < size; steps++) {
      if (s > kMaxRegSect || s >= nsect_ || s >= fat_.size() || steps > nsect_)
        return Fail("broken sector chain");
      out->insert(out->end(), Sector(s), Sector(s) + ss_);
      s = fat_[s];
    }
    if (size != UINT64_MAX) {
      if (out->size() < size) return Fail("stream shorter than its size");
      out->resize(size_t(size));
    }
    return true;
  }

  bool ReadMini(uint32_t start, uint64_t size, std::vector<uint8_t>* out) {
    out->clear();
    uint32_t s = start;
    for (uint32_t steps = 0; out->size() < size; steps++) {
      const size_t at = size_t(s) * kMiniSectorSize;
      if (s >= minifat_.size() || at + kMiniSectorSize > ministream_.size() ||
          steps > minifat_.size())
        return Fail("broken mini sector chain");
      out->insert(out->end(), ministream_.begin() + ptrdiff_t(at),
                  ministream_.begin() + ptrdiff_t(at + kMiniSectorSize));
      s = minifat_[s];
    }
    out->resize(size_t(size));
    return true;
  }

  static RawEntry Parse(const uint8_t* p, uint16_t major) {
    RawEntry r;
    const uint16_t bytes = Le16(p + 0x40);
    for (uint16_t i = 0; i + 2 < bytes && i < 62; i += 2) r.name.push_back(char16_t(Le16(p + i)));
    r.type = p[0x42];
    r.left = Le32(p + 0x44);
    r.right = Le32(p + 0x48);
    r.child = Le32(p + 0x4C);
    std::memcpy(r.clsid, p + 0x50, 16);
    r.state_bits = Le32(p + 0x60);
    r.ctime = Le64(p + 0x64);
    r.mtime = Le64(p + 0x6C);
    r.start = Le32(p + 0x74);
    // Version 3 files may hold anything in the size's high half.
    r.size = major == 3 ? Le32(p + 0x78) : Le64(p + 0x78);
    return r;
  }

  static void Fill(const RawEntry& r, Entry* e) {
    e->type = Entry::Type(r.type);
    e->name = r.name;
    std::memcpy(e->clsid, r.clsid, 16);
    e->state_bits = r.state_bits;
    e->ctime = r.ctime;
    e->mtime = r.mtime;
  }

  // The entries of the sibling tree under `id`, as children of `parent`.
  bool Children(uint32_t id, Entry* parent) {
    std::vector<uint32_t> todo;
    if (id != kNoStream) todo.push_back(id);
    while (!todo.empty()) {
      const uint32_t i = todo.back();
      todo.pop_back();
      if (i >= raw_.size() || visited_[i]) return Fail("broken directory tree");
      visited_[i] = true;
      const RawEntry& r = raw_[i];
      if (r.left != kNoStream) todo.push_back(r.left);
      if (r.right != kNoStream) todo.push_back(r.right);
      if (r.type != uint8_t(Entry::Type::Storage) && r.type != uint8_t(Entry::Type::Stream))
        return Fail("directory entry of an unknown type");
      auto e = std::make_unique<Entry>();
      Fill(r, e.get());
      if (r.type == uint8_t(Entry::Type::Stream)) {
        const bool ok = r.size < cutoff_ ? ReadMini(r.start, r.size, &e->data)
                                         : ReadChain(r.start, r.size, &e->data);
        if (!ok) return false;
      } else if (!Children(r.child, e.get())) {
        return false;
      }
      parent->children.push_back(std::move(e));
    }
    return true;
  }

  const std::vector<uint8_t>& f_;
  std::string* err_;
  uint32_t ss_ = kSectorSize, nsect_ = 0, cutoff_ = kMiniStreamCutoff;
  std::vector<uint32_t> fat_, minifat_;
  std::vector<uint8_t> ministream_;
  std::vector<RawEntry> raw_;
  std::vector<bool> visited_;
};

// ---- writing -----------------------------------------------------------------

struct Node {
  const Entry* e;
  uint32_t left = kNoStream, right = kNoStream, child = kNoStream;
  uint8_t color = kBlack;
  uint32_t start = 0;
  uint64_t size = 0;
};

// The directory: the root first, each storage's children after it as a
// balanced tree (a valid red-black tree: the deepest level red unless the
// tree is complete).
void AddChildren(std::vector<Node>* nodes, uint32_t parent) {
  std::vector<const Entry*> kids;
  for (const auto& c : (*nodes)[parent].e->children) kids.push_back(c.get());
  std::sort(kids.begin(), kids.end(), [](const Entry* a, const Entry* b) {
    return CompareNames(a->name, b->name) < 0;
  });
  if (kids.empty()) return;
  const uint32_t first = uint32_t(nodes->size());
  for (const Entry* k : kids) nodes->push_back(Node{k});
  int max_depth = 0;
  std::vector<int> depth(kids.size());
  std::function<uint32_t(int, int, int)> build = [&](int lo, int hi, int d) -> uint32_t {
    if (lo > hi) return kNoStream;
    const int mid = (lo + hi) / 2;
    depth[size_t(mid)] = d;
    max_depth = std::max(max_depth, d);
    Node& n = (*nodes)[first + uint32_t(mid)];
    const uint32_t l = build(lo, mid - 1, d + 1), r = build(mid + 1, hi, d + 1);
    Node& m = (*nodes)[first + uint32_t(mid)];  // (no reallocation; for clarity)
    (void)n;
    m.left = l;
    m.right = r;
    return first + uint32_t(mid);
  };
  (*nodes)[parent].child = build(0, int(kids.size()) - 1, 0);
  const size_t n = kids.size();
  const bool complete = ((n + 1) & n) == 0;
  for (size_t i = 0; i < n; i++)
    (*nodes)[first + i].color = !complete && depth[i] == max_depth ? kRed : kBlack;
  for (size_t i = 0; i < n; i++)
    if (kids[i]->IsStorage()) AddChildren(nodes, first + uint32_t(i));
}

uint32_t Sectors(uint64_t bytes, uint32_t unit) { return uint32_t((bytes + unit - 1) / unit); }

}  // namespace

Entry* Entry::Find(const std::u16string& n) const {
  for (const auto& c : children)
    if (CompareNames(c->name, n) == 0) return c.get();
  return nullptr;
}

int CompareNames(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = 0; i < a.size(); i++) {
    const char16_t x = Upper(a[i]), y = Upper(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

bool ValidName(const std::u16string& name) {
  if (name.empty() || name.size() > kMaxNameChars) return false;
  for (char16_t c : name)
    if (c == u'/' || c == u'\\' || c == u':' || c == u'!') return false;
  return true;
}

bool Read(const std::vector<uint8_t>& file, Entry* root, std::string* err) {
  *root = Entry{};
  return Reader(file, err).Run(root);
}

std::vector<uint8_t> Write(const Entry& root) {
  std::vector<Node> nodes{Node{&root}};
  AddChildren(&nodes, 0);

  // Where the streams go: small ones in the mini stream, the rest in
  // sectors of their own.
  uint32_t mini_sectors = 0, big_sectors = 0;
  for (Node& n : nodes) {
    if (n.e->IsStorage()) continue;
    n.size = n.e->data.size();
    if (!n.size) n.start = kEndOfChain;
    else if (n.size < kMiniStreamCutoff) n.start = mini_sectors, mini_sectors += Sectors(n.size, kMiniSectorSize);
    else big_sectors += Sectors(n.size, kSectorSize);
  }
  const uint32_t per = kSectorSize / 4;
  const uint32_t dir_sectors = Sectors(uint64_t(nodes.size()) * kDirEntrySize, kSectorSize);
  const uint32_t minifat_sectors = Sectors(uint64_t(mini_sectors) * 4, kSectorSize);
  const uint32_t ministream_sectors = Sectors(uint64_t(mini_sectors) * kMiniSectorSize, kSectorSize);
  const uint32_t data_sectors = dir_sectors + minifat_sectors + ministream_sectors + big_sectors;
  // The FAT covers every sector, its own and the DIFAT's included.
  uint32_t nfat = 0, ndifat = 0;
  for (;;) {
    const uint32_t f = Sectors(uint64_t(data_sectors) + nfat + ndifat, per);
    const uint32_t d = f > kHeaderDifat ? Sectors(f - kHeaderDifat, per - 1) : 0;
    if (f == nfat && d == ndifat) break;
    nfat = f;
    ndifat = d;
  }
  const uint32_t total = nfat + ndifat + data_sectors;
  const uint32_t difat0 = nfat, dir0 = difat0 + ndifat, minifat0 = dir0 + dir_sectors,
                 ministream0 = minifat0 + minifat_sectors, big0 = ministream0 + ministream_sectors;

  std::vector<uint32_t> fat(size_t(nfat) * per, kFreeSect);
  for (uint32_t i = 0; i < nfat; i++) fat[i] = kFatSect;
  for (uint32_t i = 0; i < ndifat; i++) fat[difat0 + i] = kDifSect;
  auto chain = [&](uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) fat[first + i] = i + 1 < count ? first + i + 1 : kEndOfChain;
  };
  chain(dir0, dir_sectors);
  chain(minifat0, minifat_sectors);
  chain(ministream0, ministream_sectors);
  std::vector<uint8_t> out(size_t(total + 1) * kSectorSize, 0);
  auto sector = [&](uint32_t s) { return out.data() + size_t(s + 1) * kSectorSize; };
  std::vector<uint32_t> minifat(size_t(minifat_sectors) * per, kFreeSect);
  uint32_t next_big = big0;
  for (Node& n : nodes) {
    if (n.e->IsStorage() || !n.size) continue;
    const uint8_t* src = n.e->data.data();
    if (n.size < kMiniStreamCutoff) {
      const uint32_t count = Sectors(n.size, kMiniSectorSize);
      for (uint32_t i = 0; i < count; i++)
        minifat[n.start + i] = i + 1 < count ? n.start + i + 1 : kEndOfChain;
      std::memcpy(sector(ministream0) + size_t(n.start) * kMiniSectorSize, src, size_t(n.size));
    } else {
      const uint32_t count = Sectors(n.size, kSectorSize);
      n.start = next_big;
      chain(next_big, count);
      std::memcpy(sector(next_big), src, size_t(n.size));
      next_big += count;
    }
  }
  // The root's stream is the mini stream.
  nodes[0].start = mini_sectors ? ministream0 : kEndOfChain;
  nodes[0].size = uint64_t(mini_sectors) * kMiniSectorSize;

  // Header.
  uint8_t* h = out.data();
  std::memcpy(h, kSignature, 8);
  Put16(h + 0x18, 0x003E);  // minor version
  Put16(h + 0x1A, 3);       // major version
  Put16(h + 0x1C, 0xFFFE);  // little-endian
  Put16(h + 0x1E, 9);       // 512-byte sectors
  Put16(h + 0x20, 6);       // 64-byte mini sectors
  Put32(h + 0x2C, nfat);
  Put32(h + 0x30, dir0);
  Put32(h + 0x38, kMiniStreamCutoff);
  Put32(h + 0x3C, minifat_sectors ? minifat0 : kEndOfChain);
  Put32(h + 0x40, minifat_sectors);
  Put32(h + 0x44, ndifat ? difat0 : kEndOfChain);
  Put32(h + 0x48, ndifat);
  for (uint32_t i = 0; i < kHeaderDifat; i++) Put32(h + 0x4C + 4 * i, i < nfat ? i : kFreeSect);
  for (uint32_t k = 0; k < ndifat; k++) {
    uint8_t* p = sector(difat0 + k);
    for (uint32_t i = 0; i + 1 < per; i++) {
      const uint32_t f = kHeaderDifat + k * (per - 1) + i;
      Put32(p + 4 * i, f < nfat ? f : kFreeSect);
    }
    Put32(p + 4 * (per - 1), k + 1 < ndifat ? difat0 + k + 1 : kEndOfChain);
  }
  for (uint32_t i = 0; i < nfat * per; i++) Put32(sector(i / per) + 4 * (i % per), fat[i]);
  for (uint32_t i = 0; i < minifat_sectors * per; i++)
    Put32(sector(minifat0 + i / per) + 4 * (i % per), minifat[i]);

  // Directory (the slots past the last entry are free: no siblings, no child).
  uint8_t* dir = sector(dir0);
  for (uint32_t i = 0; i < dir_sectors * (kSectorSize / kDirEntrySize); i++) {
    uint8_t* p = dir + size_t(i) * kDirEntrySize;
    Put32(p + 0x44, kNoStream);
    Put32(p + 0x48, kNoStream);
    Put32(p + 0x4C, kNoStream);
    if (i >= nodes.size()) continue;
    const Node& n = nodes[i];
    const Entry& e = *n.e;
    const size_t len = std::min(e.name.size(), kMaxNameChars);
    for (size_t k = 0; k < len; k++) Put16(p + 2 * k, uint16_t(e.name[k]));
    Put16(p + 0x40, uint16_t((len + 1) * 2));
    p[0x42] = uint8_t(e.type);
    p[0x43] = n.color;
    Put32(p + 0x44, n.left);
    Put32(p + 0x48, n.right);
    Put32(p + 0x4C, e.IsStorage() ? n.child : kNoStream);
    if (e.IsStorage()) {
      std::memcpy(p + 0x50, e.clsid, 16);
      Put32(p + 0x60, e.state_bits);
      // The root's creation time is the file's own ([MS-CFB] 2.6.3).
      Put64(p + 0x64, e.type == Entry::Type::Root ? 0 : e.ctime);
      Put64(p + 0x6C, e.mtime);
    }
    if (e.type != Entry::Type::Storage) {
      Put32(p + 0x74, n.start);
      Put64(p + 0x78, n.size);
    }
  }
  return out;
}

}  // namespace recomp::cfb

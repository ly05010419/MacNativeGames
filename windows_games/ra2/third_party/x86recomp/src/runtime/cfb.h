#pragma once
// Compound files ([MS-CFB], "structured storage"): the file format behind
// IStorage/IStream (ole32 StgCreateDocfile, StgOpenStorage). A file is
// read whole into a tree of storages and streams, and written whole from
// one; hle/ole32_storage.cpp keeps the tree while a file is open.
//
// Written files are version 3 (512-byte sectors): a header, the FAT (and
// DIFAT sectors past 109 FAT sectors), the directory, the mini FAT and the
// mini stream (streams under 4096 bytes, in 64-byte mini sectors), then the
// other streams. Version 3 and 4 files are read.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recomp::cfb {

constexpr uint32_t kMiniStreamCutoff = 4096;
constexpr size_t kMaxNameChars = 31;  // without the terminating NUL

struct Entry {
  enum class Type : uint8_t { Storage = 1, Stream = 2, Root = 5 };
  Type type = Type::Stream;
  std::u16string name;
  uint8_t clsid[16] = {};  // storages
  uint32_t state_bits = 0;
  uint64_t ctime = 0, mtime = 0;  // FILETIMEs (storages)
  std::vector<uint8_t> data;      // streams
  std::vector<std::unique_ptr<Entry>> children;  // storages, in no order

  bool IsStorage() const { return type != Type::Stream; }
  // The child named `name` (compared as CompareNames does), or null.
  Entry* Find(const std::u16string& name) const;
};

// [MS-CFB] 2.6.4: names order by length, then by their code units
// upper-cased (ASCII letters; other characters as they are). <0, 0, >0.
int CompareNames(const std::u16string& a, const std::u16string& b);

// Whether `name` may name an element: 1 to 31 characters, none of
// '/', '\\', ':', '!'.
bool ValidName(const std::u16string& name);

// Parses a compound file into *root (a Root entry). False, with *err
// saying why, for anything that is not a well-formed compound file.
bool Read(const std::vector<uint8_t>& file, Entry* root, std::string* err);

// The compound file holding `root` and everything under it.
std::vector<uint8_t> Write(const Entry& root);

}  // namespace recomp::cfb

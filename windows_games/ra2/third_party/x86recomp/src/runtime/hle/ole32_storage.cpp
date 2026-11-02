// ole32: structured storage. StgCreateDocfile and StgOpenStorage over
// compound files (runtime/cfb.h), with their IStorage and IStream.
//
// An open file is held whole in memory as a tree; the storages and
// streams opened from it point into that tree. In direct mode (no
// STGM_TRANSACTED) the file is written when it is created, when the root
// storage is committed and when the last object of the file is let go; in
// transacted mode only on the root's Commit, and Revert goes back to the
// file as it is on disk. Elements below the root are opened exclusively,
// as compound files require (STGM_SHARE_EXCLUSIVE): one open twice, or
// destroyed or renamed while open, is STG_E_ACCESSDENIED.
//
// Methods not implemented here (IStorage::CopyTo, MoveElementTo,
// EnumElements) have no handler and fail by name when called.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/cfb.h"
#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

using cfb::Entry;

constexpr uint32_t kS_OK = 0, kE_POINTER = 0x80004003,
                   kE_NOINTERFACE = 0x80004002, kSTG_E_INVALIDFUNCTION = 0x80030001,
                   kSTG_E_FILENOTFOUND = 0x80030002, kSTG_E_ACCESSDENIED = 0x80030005,
                   kSTG_E_INVALIDPOINTER = 0x80030009, kSTG_E_WRITEFAULT = 0x8003001D,
                   kSTG_E_READFAULT = 0x8003001E, kSTG_E_FILEALREADYEXISTS = 0x80030050,
                   kSTG_E_INVALIDNAME = 0x800300FC,
                   kSTG_E_INVALIDFLAG = 0x800300FF, kSTG_E_DOCFILECORRUPT = 0x80030109;

// grfMode.
constexpr uint32_t kRwMask = 0x3, kRead = 0x0, kShareMask = 0x70, kShareExclusive = 0x10,
                   kCreate = 0x1000, kTransacted = 0x10000;
// Modes this implementation does not have; asked for, they are fatal.
struct Unsupported {
  uint32_t bit;
  const char* name;
};
constexpr Unsupported kUnsupported[] = {
    {0x20000, "STGM_CONVERT"},         {0x40000, "STGM_PRIORITY"},
    {0x100000, "STGM_NOSCRATCH"},      {0x200000, "STGM_NOSNAPSHOT"},
    {0x400000, "STGM_DIRECT_SWMR"},    {0x4000000, "STGM_DELETEONRELEASE"},
    {0x8000000, "STGM_SIMPLE"}};

// STATSTG (32-bit): pwcsName, type, cbSize, mtime, ctime, atime, grfMode,
// grfLocksSupported, clsid, grfStateBits, reserved.
constexpr uint32_t kStatSize = 72, kStgtyStorage = 1, kStgtyStream = 2, kStatflagNoname = 1;

constexpr guest::GUID OleGuid(uint32_t d1) { return {d1, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}}; }
constexpr guest::GUID kIID_IUnknown = OleGuid(0x00000000);
constexpr guest::GUID kIID_IStorage = OleGuid(0x0000000B);
constexpr guest::GUID kIID_IStream = OleGuid(0x0000000C);
constexpr guest::GUID kIID_ISequentialStream = {
    0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};

uint64_t FileTimeNow() {
  using namespace std::chrono;
  return kFileTimeUnixEpoch +
         uint64_t(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()) * 10;
}

bool Writable(uint32_t mode) { return (mode & kRwMask) != kRead; }

// Fatal for a mode this implementation does not provide.
void CheckSupported(uint32_t mode, const char* fn) {
  for (const Unsupported& u : kUnsupported)
    if (mode & u.bit) FatalF("%s: %s is not implemented", fn, u.name);
}

std::recursive_mutex g_mu;

// An open compound file.
struct File {
  std::string guest_path, host_path;
  Entry root;
  bool writable = false, transacted = false, dirty = false;
  std::map<const Entry*, int> open;  // elements below the root that are open

  // Writes the tree out (whole, replacing the file).
  bool Save() {
    root.mtime = FileTimeNow();
    const std::vector<uint8_t> bytes = cfb::Write(root);
    const std::string tmp = host_path + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())))
        return false;
    }
    if (std::rename(tmp.c_str(), host_path.c_str()) != 0) return false;
    dirty = false;
    return true;
  }

  ~File() {
    if (writable && dirty && !transacted && !Save())
      WarnF("ole32: cannot write %s\n", host_path.c_str());
  }
};

struct Element : com::Object {
  std::shared_ptr<File> file;
  Entry* e = nullptr;
  uint32_t mode = 0;
  Element(std::shared_ptr<File> f, Entry* en, uint32_t m) : file(std::move(f)), e(en), mode(m) {
    if (e != &file->root) file->open[e]++;
  }
  ~Element() override {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (e != &file->root && !--file->open[e]) file->open.erase(e);
  }
};
struct Storage : Element {
  using Element::Element;
};
struct Stream : Element {
  using Element::Element;
  uint64_t pos = 0;
};

uint32_t StorageVtable() {
  return com::Vtable("ole32.dll", "IStorage",
                     {"QueryInterface", "AddRef", "Release", "CreateStream", "OpenStream",
                      "CreateStorage", "OpenStorage", "CopyTo", "MoveElementTo", "Commit",
                      "Revert", "EnumElements", "DestroyElement", "RenameElement",
                      "SetElementTimes", "SetClass", "SetStateBits", "Stat"});
}

uint32_t StreamVtable() {
  return com::Vtable("ole32.dll", "IStream",
                     {"QueryInterface", "AddRef", "Release", "Read", "Write", "Seek", "SetSize",
                      "CopyTo", "Commit", "Revert", "LockRegion", "UnlockRegion", "Stat",
                      "Clone"});
}

template <typename T>
T* Self(Ctx& c, const char* iface) {
  T* o = com::As<T>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

uint32_t PublishStorage(std::shared_ptr<File> f, Entry* e, uint32_t mode) {
  return com::Publish(std::make_shared<Storage>(std::move(f), e, mode), StorageVtable());
}

uint32_t PublishStream(std::shared_ptr<File> f, Entry* e, uint32_t mode) {
  return com::Publish(std::make_shared<Stream>(std::move(f), e, mode), StreamVtable());
}

// An element name from the guest; *bad gets the HRESULT for one that
// cannot name an element.
bool ElementName(gaddr p, std::u16string* name, uint32_t* bad) {
  if (!p) {
    *bad = kSTG_E_INVALIDPOINTER;
    return false;
  }
  *name = GWStr(p);
  if (!cfb::ValidName(*name)) {
    *bad = kSTG_E_INVALIDNAME;
    return false;
  }
  return true;
}

// The mode an element below a storage may be opened with.
uint32_t ElementMode(const Storage& parent, uint32_t mode, const char* fn) {
  CheckSupported(mode, fn);
  if (mode & kTransacted) FatalF("%s: transacted elements below the root are not implemented", fn);
  if ((mode & kShareMask) != kShareExclusive) return kSTG_E_INVALIDFLAG;
  if ((mode & kRwMask) == kRwMask) return kSTG_E_INVALIDFLAG;
  if (Writable(mode) && !Writable(parent.mode)) return kSTG_E_ACCESSDENIED;
  return kS_OK;
}

bool IsOpen(const File& f, const Entry* e) { return f.open.count(e) != 0; }

void RemoveChild(Entry* parent, const Entry* child) {
  auto& v = parent->children;
  for (auto it = v.begin(); it != v.end(); ++it)
    if (it->get() == child) {
      v.erase(it);
      return;
    }
}

// Fills STATSTG at `out` for element e (named `name` unless the flag says
// not to).
void FillStat(gaddr out, const Entry& e, const std::u16string& name, uint32_t mode, uint32_t flag) {
  std::memset(GPtr(out), 0, kStatSize);
  if (!(flag & kStatflagNoname)) {
    const gaddr s = HeapAllocProcess(uint32_t((name.size() + 1) * 2), false);  // CoTaskMemAlloc
    for (size_t i = 0; i <= name.size(); i++) Wr16(s + 2 * uint32_t(i), i < name.size() ? name[i] : 0);
    Wr32(out, s);
  }
  Wr32(out + 4, e.IsStorage() ? kStgtyStorage : kStgtyStream);
  const uint64_t size = e.data.size();
  Wr32(out + 8, uint32_t(size));
  Wr32(out + 12, uint32_t(size >> 32));
  Wr32(out + 16, uint32_t(e.mtime));
  Wr32(out + 20, uint32_t(e.mtime >> 32));
  Wr32(out + 24, uint32_t(e.ctime));
  Wr32(out + 28, uint32_t(e.ctime >> 32));
  Wr32(out + 40, mode);
  std::memcpy(GPtr(out + 48), e.clsid, 16);
  Wr32(out + 64, e.state_bits);
}

uint32_t QueryInterface(Ctx& c, const guest::GUID& iid, const guest::GUID* also) {
  const gaddr riid = Arg(c, 1), out = Arg(c, 2);
  if (!out) return kE_POINTER;
  if (com::GuidEq(riid, iid) || com::GuidEq(riid, kIID_IUnknown) || (also && com::GuidEq(riid, *also))) {
    com::AddRef(Arg(c, 0));
    Wr32(out, Arg(c, 0));
    return kS_OK;
  }
  // Not IPropertySetStorage either: games that ask fall back to streams.
  Wr32(out, 0);
  return kE_NOINTERFACE;
}

// ---- StgCreateDocfile, StgOpenStorage ------------------------------------------

uint32_t OpenFile(const std::u16string& wname, uint32_t mode, bool create, gaddr out) {
  CheckSupported(mode, create ? "StgCreateDocfile" : "StgOpenStorage");
  if ((mode & kRwMask) == kRwMask) return kSTG_E_INVALIDFLAG;
  auto f = std::make_shared<File>();
  f->guest_path = NarrowPath(wname);
  f->writable = Writable(mode);
  f->transacted = mode & kTransacted;
  const std::string existing = HostPath(f->guest_path, PathAccess::kRead);
  std::ifstream in(existing, std::ios::binary);
  if (create) {
    if (!f->writable) return kSTG_E_INVALIDFLAG;
    if (in && !(mode & kCreate)) return kSTG_E_FILEALREADYEXISTS;
    f->host_path = HostPath(f->guest_path, PathAccess::kWrite);
    f->root.type = Entry::Type::Root;
    f->root.name = u"Root Entry";
    f->root.ctime = f->root.mtime = FileTimeNow();
    f->dirty = true;
    // The file exists from now on, as an empty compound file.
    if (!f->transacted && !f->Save()) return kSTG_E_WRITEFAULT;
  } else {
    if (!in) return kSTG_E_FILENOTFOUND;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    std::string err;
    if (!cfb::Read(bytes, &f->root, &err)) {
      WarnF("ole32: StgOpenStorage(%s): %s\n", f->guest_path.c_str(), err.c_str());
      // A file that is no compound file at all, or a broken one.
      static const uint8_t kSig[4] = {0xD0, 0xCF, 0x11, 0xE0};
      const bool signed_ = bytes.size() >= 4 && std::memcmp(bytes.data(), kSig, 4) == 0;
      return signed_ ? kSTG_E_DOCFILECORRUPT : kSTG_E_FILEALREADYEXISTS;
    }
    f->host_path = f->writable ? HostPath(f->guest_path, PathAccess::kUpdate) : existing;
  }
  Wr32(out, PublishStorage(f, &f->root, mode));
  return kS_OK;
}

// (pwcsName, grfMode, reserved, ppstgOpen)
HLE("ole32.dll", StgCreateDocfile, 4) {
  const gaddr name = Arg(c, 0), out = Arg(c, 3);
  if (!out) return kSTG_E_INVALIDPOINTER;
  Wr32(out, 0);
  if (!name) FatalF("StgCreateDocfile: a temporary file (no name) is not implemented");
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return OpenFile(GWStr(name), Arg(c, 1), true, out);
}

// (pwcsName, pstgPriority, grfMode, snbExclude, reserved, ppstgOpen)
HLE("ole32.dll", StgOpenStorage, 6) {
  const gaddr name = Arg(c, 0), out = Arg(c, 5);
  if (!out) return kSTG_E_INVALIDPOINTER;
  Wr32(out, 0);
  if (Arg(c, 1) || Arg(c, 3))
    FatalF("StgOpenStorage: a priority storage or excluded elements are not implemented");
  if (!name) return kSTG_E_INVALIDNAME;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return OpenFile(GWStr(name), Arg(c, 2), false, out);
}

// ---- IStorage --------------------------------------------------------------------

HLE_NAMED("ole32.dll", "IStorage::QueryInterface", Stg_QueryInterface, 3) {
  return QueryInterface(c, kIID_IStorage, nullptr);
}
HLE_NAMED("ole32.dll", "IStorage::AddRef", Stg_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ole32.dll", "IStorage::Release", Stg_Release, 1) { return com::Release(Arg(c, 0)); }

// (pwcsName, grfMode, reserved1, reserved2, ppstm) and the storage form
// (ppstg): a new element, replacing one of that name with STGM_CREATE.
uint32_t CreateElement(Ctx& c, bool stream, const char* fn) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  const uint32_t mode = Arg(c, 2), out = Arg(c, 5);
  if (!out) return kSTG_E_INVALIDPOINTER;
  Wr32(out, 0);
  std::u16string name;
  uint32_t hr;
  if (!ElementName(Arg(c, 1), &name, &hr)) return hr;
  if ((hr = ElementMode(*s, mode, fn)) != kS_OK) return hr;
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  if (Entry* old = s->e->Find(name)) {
    if (!(mode & kCreate)) return kSTG_E_FILEALREADYEXISTS;
    if (IsOpen(*s->file, old)) return kSTG_E_ACCESSDENIED;
    RemoveChild(s->e, old);
  }
  auto e = std::make_unique<Entry>();
  e->type = stream ? Entry::Type::Stream : Entry::Type::Storage;
  e->name = name;
  if (!stream) e->ctime = e->mtime = FileTimeNow();
  Entry* raw = e.get();
  s->e->children.push_back(std::move(e));
  s->file->dirty = true;
  Wr32(out, stream ? PublishStream(s->file, raw, mode) : PublishStorage(s->file, raw, mode));
  return kS_OK;
}

HLE_NAMED("ole32.dll", "IStorage::CreateStream", Stg_CreateStream, 6) {
  return CreateElement(c, true, "IStorage::CreateStream");
}
HLE_NAMED("ole32.dll", "IStorage::CreateStorage", Stg_CreateStorage, 6) {
  return CreateElement(c, false, "IStorage::CreateStorage");
}

// An existing element: (pwcsName, reserved1, grfMode, reserved2, ppstm)
// for a stream, (pwcsName, pstgPriority, grfMode, snbExclude, reserved,
// ppstg) for a storage.
uint32_t OpenElement(Ctx& c, bool stream, const char* fn) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  const uint32_t mode = Arg(c, 3), out = Arg(c, stream ? 5 : 6);
  if (!out) return kSTG_E_INVALIDPOINTER;
  Wr32(out, 0);
  if (!stream && (Arg(c, 2) || Arg(c, 4)))
    FatalF("%s: a priority storage or excluded elements are not implemented", fn);
  std::u16string name;
  uint32_t hr;
  if (!ElementName(Arg(c, 1), &name, &hr)) return hr;
  if ((hr = ElementMode(*s, mode, fn)) != kS_OK) return hr;
  Entry* e = s->e->Find(name);
  if (!e || e->IsStorage() == stream) return kSTG_E_FILENOTFOUND;
  if (IsOpen(*s->file, e)) return kSTG_E_ACCESSDENIED;
  Wr32(out, stream ? PublishStream(s->file, e, mode) : PublishStorage(s->file, e, mode));
  return kS_OK;
}

HLE_NAMED("ole32.dll", "IStorage::OpenStream", Stg_OpenStream, 6) {
  return OpenElement(c, true, "IStorage::OpenStream");
}
HLE_NAMED("ole32.dll", "IStorage::OpenStorage", Stg_OpenStorage, 7) {
  return OpenElement(c, false, "IStorage::OpenStorage");
}

// (grfCommitFlags): the root writes the file; storages below it are
// direct, their changes already in the tree.
HLE_NAMED("ole32.dll", "IStorage::Commit", Stg_Commit, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  if (s->e != &s->file->root || !s->file->writable) return kS_OK;
  return s->file->Save() ? kS_OK : kSTG_E_WRITEFAULT;
}

// (): a transacted root goes back to the file on disk; direct storages
// have nothing to undo.
HLE_NAMED("ole32.dll", "IStorage::Revert", Stg_Revert, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  File& f = *s->file;
  if (s->e != &f.root || !f.transacted) return kS_OK;
  if (!f.open.empty()) FatalF("IStorage::Revert with elements still open is not implemented");
  std::ifstream in(f.host_path, std::ios::binary);
  if (!in) {  // never committed: back to empty
    const uint64_t ctime = f.root.ctime;
    f.root = Entry{};
    f.root.type = Entry::Type::Root;
    f.root.name = u"Root Entry";
    f.root.ctime = f.root.mtime = ctime;
  } else {
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    std::string err;
    if (!cfb::Read(bytes, &f.root, &err)) return kSTG_E_READFAULT;
  }
  f.dirty = false;
  return kS_OK;
}

// (pwcsName)
HLE_NAMED("ole32.dll", "IStorage::DestroyElement", Stg_DestroyElement, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  std::u16string name;
  uint32_t hr;
  if (!ElementName(Arg(c, 1), &name, &hr)) return hr;
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  Entry* e = s->e->Find(name);
  if (!e) return kSTG_E_FILENOTFOUND;
  // An element open, or with something open below it.
  std::vector<const Entry*> todo{e};
  while (!todo.empty()) {
    const Entry* x = todo.back();
    todo.pop_back();
    if (IsOpen(*s->file, x)) return kSTG_E_ACCESSDENIED;
    for (const auto& k : x->children) todo.push_back(k.get());
  }
  RemoveChild(s->e, e);
  s->file->dirty = true;
  return kS_OK;
}

// (pwcsOldName, pwcsNewName)
HLE_NAMED("ole32.dll", "IStorage::RenameElement", Stg_RenameElement, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  std::u16string from, to;
  uint32_t hr;
  if (!ElementName(Arg(c, 1), &from, &hr) || !ElementName(Arg(c, 2), &to, &hr)) return hr;
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  Entry* e = s->e->Find(from);
  if (!e) return kSTG_E_FILENOTFOUND;
  if (s->e->Find(to)) return kSTG_E_FILEALREADYEXISTS;
  if (IsOpen(*s->file, e)) return kSTG_E_ACCESSDENIED;
  e->name = to;
  s->file->dirty = true;
  return kS_OK;
}

// (pwcsName, pctime, patime, pmtime): a storage's (this one for no name)
// creation and modification times; streams and access times have none in
// a compound file.
HLE_NAMED("ole32.dll", "IStorage::SetElementTimes", Stg_SetElementTimes, 5) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  Entry* e = s->e;
  if (Arg(c, 1)) {
    std::u16string name;
    uint32_t hr;
    if (!ElementName(Arg(c, 1), &name, &hr)) return hr;
    if (!(e = s->e->Find(name))) return kSTG_E_FILENOTFOUND;
  }
  if (!e->IsStorage()) return kS_OK;
  auto ft = [](gaddr p) { return Rd32(p) | uint64_t(Rd32(p + 4)) << 32; };
  if (Arg(c, 2)) e->ctime = ft(Arg(c, 2));
  if (Arg(c, 4)) e->mtime = ft(Arg(c, 4));
  s->file->dirty = true;
  return kS_OK;
}

// (clsid)
HLE_NAMED("ole32.dll", "IStorage::SetClass", Stg_SetClass, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  if (!Arg(c, 1)) return kSTG_E_INVALIDPOINTER;
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  std::memcpy(s->e->clsid, GPtr(Arg(c, 1)), 16);
  s->file->dirty = true;
  return kS_OK;
}

// (grfStateBits, grfMask)
HLE_NAMED("ole32.dll", "IStorage::SetStateBits", Stg_SetStateBits, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  s->e->state_bits = (s->e->state_bits & ~Arg(c, 2)) | (Arg(c, 1) & Arg(c, 2));
  s->file->dirty = true;
  return kS_OK;
}

// (pstatstg, grfStatFlag): the root is named by the file's path.
HLE_NAMED("ole32.dll", "IStorage::Stat", Stg_Stat, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Storage* s = Self<Storage>(c, "IStorage");
  if (!Arg(c, 1)) return kSTG_E_INVALIDPOINTER;
  const std::u16string name = s->e == &s->file->root
                                  ? win32::Widen(s->file->guest_path)
                                  : s->e->name;
  FillStat(Arg(c, 1), *s->e, name, s->mode, Arg(c, 2));
  return kS_OK;
}

// ---- IStream -----------------------------------------------------------------------

HLE_NAMED("ole32.dll", "IStream::QueryInterface", Stm_QueryInterface, 3) {
  return QueryInterface(c, kIID_IStream, &kIID_ISequentialStream);
}
HLE_NAMED("ole32.dll", "IStream::AddRef", Stm_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ole32.dll", "IStream::Release", Stm_Release, 1) { return com::Release(Arg(c, 0)); }

// (pv, cb, pcbRead): S_OK also when fewer bytes are left than asked for.
HLE_NAMED("ole32.dll", "IStream::Read", Stm_Read, 4) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  const gaddr pv = Arg(c, 1), pread = Arg(c, 3);
  const uint32_t cb = Arg(c, 2);
  if (!pv && cb) return kSTG_E_INVALIDPOINTER;
  const uint64_t size = s->e->data.size();
  const uint32_t n = s->pos >= size ? 0 : uint32_t(std::min<uint64_t>(cb, size - s->pos));
  if (n) std::memcpy(GPtr(pv), s->e->data.data() + s->pos, n);
  s->pos += n;
  if (pread) Wr32(pread, n);
  return kS_OK;
}

// (pv, cb, pcbWritten): past the end the stream grows.
HLE_NAMED("ole32.dll", "IStream::Write", Stm_Write, 4) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  const gaddr pv = Arg(c, 1), pwritten = Arg(c, 3);
  const uint32_t cb = Arg(c, 2);
  if (pwritten) Wr32(pwritten, 0);
  if (!pv && cb) return kSTG_E_INVALIDPOINTER;
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  if (cb) {
    if (!EnsureReadable(c, pv, cb)) return kSTG_E_INVALIDPOINTER;
    std::vector<uint8_t>& d = s->e->data;
    if (d.size() < s->pos + cb) d.resize(size_t(s->pos + cb));
    std::memcpy(d.data() + s->pos, GPtr(pv), cb);
    s->pos += cb;
    s->file->dirty = true;
  }
  if (pwritten) Wr32(pwritten, cb);
  return kS_OK;
}

// (dlibMove (LARGE_INTEGER), dwOrigin, plibNewPosition)
HLE_NAMED("ole32.dll", "IStream::Seek", Stm_Seek, 5) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  const int64_t move = int64_t(Arg(c, 1) | uint64_t(Arg(c, 2)) << 32);
  int64_t base;
  switch (Arg(c, 3)) {
    case 0: base = 0; break;                               // STREAM_SEEK_SET
    case 1: base = int64_t(s->pos); break;                 // STREAM_SEEK_CUR
    case 2: base = int64_t(s->e->data.size()); break;      // STREAM_SEEK_END
    default: return kSTG_E_INVALIDFUNCTION;
  }
  if (base + move < 0) return kSTG_E_INVALIDFUNCTION;
  s->pos = uint64_t(base + move);
  if (const gaddr out = Arg(c, 4)) {
    Wr32(out, uint32_t(s->pos));
    Wr32(out + 4, uint32_t(s->pos >> 32));
  }
  return kS_OK;
}

// (libNewSize (ULARGE_INTEGER)): new bytes are zero; the position stays.
HLE_NAMED("ole32.dll", "IStream::SetSize", Stm_SetSize, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  if (!Writable(s->mode)) return kSTG_E_ACCESSDENIED;
  const uint64_t size = Arg(c, 1) | uint64_t(Arg(c, 2)) << 32;
  if (size > 0xFFFFFFFFull) return kSTG_E_INVALIDFUNCTION;  // version 3 files: 32-bit sizes
  s->e->data.resize(size_t(size), 0);
  s->file->dirty = true;
  return kS_OK;
}

// (pstm, cb (ULARGE_INTEGER), pcbRead, pcbWritten): through the target's
// Write (any IStream), in pieces.
HLE_NAMED("ole32.dll", "IStream::CopyTo", Stm_CopyTo, 6) {
  Stream* s;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    s = Self<Stream>(c, "IStream");
  }
  const gaddr dst = Arg(c, 1), pread = Arg(c, 4), pwritten = Arg(c, 5);
  if (!dst) return kSTG_E_INVALIDPOINTER;
  uint64_t want = Arg(c, 2) | uint64_t(Arg(c, 3)) << 32, read = 0, written = 0;
  constexpr uint32_t kPiece = 65536;
  const gaddr buf = HeapAllocProcess(kPiece + 4, false);
  uint32_t hr = kS_OK;
  while (want) {
    uint32_t n;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      const uint64_t size = s->e->data.size();
      n = s->pos >= size ? 0 : uint32_t(std::min<uint64_t>({want, kPiece, size - s->pos}));
      if (n) std::memcpy(GPtr(buf), s->e->data.data() + s->pos, n);
      s->pos += n;
    }
    if (!n) break;
    read += n;
    want -= n;
    Wr32(buf + kPiece, 0);
    const gaddr write = Rd32(Rd32(dst) + 4 * 4);  // ISequentialStream::Write
    hr = uint32_t(CallGuest(c, write, {dst, buf, n, buf + kPiece}));
    written += Rd32(buf + kPiece);
    if (hr & 0x80000000) break;
  }
  HeapFreeProcess(buf);
  auto put64 = [](gaddr p, uint64_t v) {
    if (p) Wr32(p, uint32_t(v)), Wr32(p + 4, uint32_t(v >> 32));
  };
  put64(pread, read);
  put64(pwritten, written);
  return hr & 0x80000000 ? hr : kS_OK;
}

// (grfCommitFlags), (): a stream of a compound file is direct.
HLE_NAMED("ole32.dll", "IStream::Commit", Stm_Commit, 2) {
  Self<Stream>(c, "IStream");
  return kS_OK;
}
HLE_NAMED("ole32.dll", "IStream::Revert", Stm_Revert, 1) {
  Self<Stream>(c, "IStream");
  return kS_OK;
}

// (libOffset, cb, dwLockType): compound file streams have no region locks.
HLE_NAMED("ole32.dll", "IStream::LockRegion", Stm_LockRegion, 6) {
  Self<Stream>(c, "IStream");
  return kSTG_E_INVALIDFUNCTION;
}
HLE_NAMED("ole32.dll", "IStream::UnlockRegion", Stm_UnlockRegion, 6) {
  Self<Stream>(c, "IStream");
  return kSTG_E_INVALIDFUNCTION;
}

// (pstatstg, grfStatFlag)
HLE_NAMED("ole32.dll", "IStream::Stat", Stm_Stat, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  if (!Arg(c, 1)) return kSTG_E_INVALIDPOINTER;
  FillStat(Arg(c, 1), *s->e, s->e->name, s->mode, Arg(c, 2));
  return kS_OK;
}

// (ppstm): another stream object on the same data, at the same position.
HLE_NAMED("ole32.dll", "IStream::Clone", Stm_Clone, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Stream* s = Self<Stream>(c, "IStream");
  const gaddr out = Arg(c, 1);
  if (!out) return kSTG_E_INVALIDPOINTER;
  auto clone = std::make_shared<Stream>(s->file, s->e, s->mode);
  clone->pos = s->pos;
  Wr32(out, com::Publish(std::move(clone), StreamVtable()));
  return kS_OK;
}

}  // namespace
}  // namespace recomp::hle

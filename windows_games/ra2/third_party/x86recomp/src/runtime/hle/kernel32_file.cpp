// kernel32: handles, guest<->host paths, files, directories, find.
//
// Guest paths are Windows paths. GuestGameDir() (GameConfig::guest_dir)
// maps onto the game install, read-only, with writes going to an overlay
// (see HostPath); any other path maps below the per-user data directory
// ($XDG_DATA_HOME/<id>/drive_c/...), where saves and
// settings end up. Lookups are case-insensitive like NTFS.

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/lazy_mem.h"
#include "runtime/platform.h"
#include "runtime/log.h"

namespace recomp {

namespace {

std::mutex g_handles_mu;
std::map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x100;

std::mutex g_cwd_mu;
std::string g_cwd;  // guest current directory; empty: GuestGameDir()

struct StdIoObject : KObject {
  explicit StdIoObject(int f) : KObject(Type::StdIo), fd(f) {}
  int fd;
};

struct FileObject : KObject {
  FileObject() : KObject(Type::File) {}
  ~FileObject() override {
    if (fd >= 0) close(fd);
  }
  int fd = -1;
};

struct FindObject : KObject {
  FindObject() : KObject(Type::Find) {}
  // Matching entries: host directory and name (as on disk).
  std::vector<std::pair<std::string, std::string>> entries;
  size_t next = 0;
};

uint32_t g_std_handles[3];

std::string UserDataRoot() { return DataDir() + "/drive_c"; }

// Splits a normalised guest path ("C:\a\b") into components after the drive.
std::vector<std::string> Components(const std::string& p) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 3; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '\\') {
      if (cur == "..") {
        if (!out.empty()) out.pop_back();
      } else if (!cur.empty() && cur != ".") {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur += p[i];
    }
  }
  return out;
}

// Absolute, normalised guest path: "C:\x\y".
std::string AbsGuest(std::string p) {
  std::replace(p.begin(), p.end(), '/', '\\');
  if (p.size() >= 2 && p[1] == ':') {
    if (p.size() == 2) p += '\\';
  } else if (!p.empty() && p[0] == '\\') {
    p = "C:" + p;
  } else {
    p = GuestCurrentDir() + "\\" + p;
  }
  std::string drive = p.substr(0, 3);
  drive[0] = char(std::toupper(drive[0]));
  std::string out = drive;
  auto comps = Components(p);
  for (size_t i = 0; i < comps.size(); i++) {
    if (i) out += '\\';
    out += comps[i];
  }
  return out;
}

// Case-insensitive resolution of path components `comps` under host dir
// `root`.
std::string Resolve(const std::string& root, const std::vector<std::string>& comps) {
  std::string cur = root;
  for (size_t i = 0; i < comps.size(); i++) {
    std::string want = comps[i];
    std::string next = cur + "/" + want;
    struct stat st;
    if (stat(next.c_str(), &st) != 0) {
      if (DIR* d = opendir(cur.c_str())) {
        std::string lw = win32::Lower(want);
        while (dirent* e = readdir(d)) {
          if (win32::Lower(e->d_name) == lw) {
            next = cur + "/" + e->d_name;
            break;
          }
        }
        closedir(d);
      }
    }
    cur = next;
  }
  return cur;
}

}  // namespace

const std::string& GuestGameDir() { return Game().guest_dir; }

std::string GuestExePath() { return GuestGameDir() + "\\" + Game().exe_name; }

std::string GuestCurrentDir() {
  std::lock_guard<std::mutex> l(g_cwd_mu);
  return g_cwd.empty() ? GuestGameDir() : g_cwd;
}

namespace {

std::string OverlayRoot() { return DataDir() + "/install"; }

// Components below the guest install directory, or false if g (absolute)
// is not inside it.
bool InstallComponents(const std::string& g, std::vector<std::string>* comps) {
  std::string lg = win32::Lower(g);
  std::string lroot = win32::Lower(GuestGameDir());
  if (lg != lroot && lg.rfind(lroot + "\\", 0) != 0) return false;
  *comps = Components("C:\\" + g.substr(std::min(g.size(), lroot.size() + 1)));
  return true;
}

bool Exists(const std::string& host, struct stat* st = nullptr) {
  struct stat s;
  return stat(host.c_str(), st ? st : &s) == 0;
}

// Copies regular file `from` to `to` (created or truncated).
bool CopyHostFile(const std::string& from, const std::string& to) {
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    close(in);
    return false;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, size_t(n)) != n) ok = false;
  close(in);
  close(out);
  return ok && n == 0;
}

}  // namespace

std::string HostPath(const std::string& guest_path, PathAccess access) {
  std::string g = AbsGuest(guest_path);
  std::vector<std::string> comps;
  if (InstallComponents(g, &comps)) {
    std::string over = Resolve(OverlayRoot(), comps);
    if (access == PathAccess::kRead) {
      if (Exists(over)) return over;
      // GameConfig::install_files: the launcher's file for this one, or
      // none ("" names no file).
      if (comps.size() == 1) {
        const auto it = Game().install_files.find(win32::Lower(comps[0]));
        if (it != Game().install_files.end()) return it->second;
      }
      return Resolve(g_proc->game_dir, comps);
    }
    // Mirror the parent directory into the overlay if the guest sees it.
    std::string parent = over.substr(0, over.find_last_of('/'));
    std::vector<std::string> pcomps(comps.begin(), comps.end() - (comps.empty() ? 0 : 1));
    struct stat st;
    if (!Exists(parent) && Exists(Resolve(g_proc->game_dir, pcomps), &st) &&
        S_ISDIR(st.st_mode))
      MakeDirs(parent);
    std::string inst = Resolve(g_proc->game_dir, comps);
    if (access == PathAccess::kUpdate && !Exists(over) && Exists(inst, &st) &&
        S_ISREG(st.st_mode))
      CopyHostFile(inst, over);
    return over;
  }
  // Everything else lives in the per-user tree, by drive letter.
  std::string root = UserDataRoot();
  if (g[0] != 'C') root += std::string("/drive_") + char(std::tolower(g[0]));
  return Resolve(root, Components(g));
}

std::vector<std::string> HostDirs(const std::string& guest_dir) {
  std::string g = AbsGuest(guest_dir);
  std::vector<std::string> comps;
  if (!InstallComponents(g, &comps)) return {HostPath(g)};
  return {Resolve(OverlayRoot(), comps), Resolve(g_proc->game_dir, comps)};
}

std::string NarrowPath(const std::u16string& path) {
  for (char16_t ch : path) {
    bool ok;
    win32::UnicodeToCp1252(ch, &ok);
    if (!ok) FatalF("path with a character outside Windows-1252 (U+%04X): not supported", unsigned(ch));
  }
  return win32::Narrow(path);
}

uint32_t NewHandle(std::shared_ptr<KObject> obj) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = std::move(obj);
  return h;
}

std::shared_ptr<KObject> GetHandle(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

int FileFd(KObject* o) {
  if (o->type == KObject::Type::File) return static_cast<FileObject*>(o)->fd;
  if (o->type == KObject::Type::StdIo) return static_cast<StdIoObject*>(o)->fd;
  return -1;
}

bool CloseHandleValue(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  return g_handles.erase(h) != 0;
}

namespace hle {
namespace {

using namespace win32;

constexpr uint32_t kInvalidHandle = 0xFFFFFFFF;
constexpr uint32_t kVolumeSerial = 0x1DEADD51;  // drive C:

// FILETIME from a POSIX time.
void PutFileTime(gaddr va, const timespec& ts) {
  Wr64(va, uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100 +
               kFileTimeUnixEpoch);
}

uint32_t AttributesOf(const struct stat& st, const std::string& name) {
  uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // DIRECTORY : NORMAL
  if (!(st.st_mode & S_IWUSR)) a = (a & ~0x80u) | 0x01;  // READONLY
  if (!name.empty() && name[0] == '.' && name != "." && name != "..")
    a |= 0x02;  // HIDDEN
  return a;
}

uint32_t ErrnoToWin(int e) {
  switch (e) {
    case ENOENT: return kErrorFileNotFound;
    case ENOTDIR: return kErrorPathNotFound;
    case EACCES:
    case EPERM: return kErrorAccessDenied;
    case EEXIST: return kErrorFileExists;
    default: return kErrorAccessDenied;
  }
}

std::string WidePath(gaddr p) { return NarrowPath(GWStr(p)); }

HLE64("kernel32.dll", GetStdHandle, 1) {
  int32_t which = int32_t(Arg(c, 0));
  int idx = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
  if (idx < 0) return HandleRet(kInvalidHandle);
  if (!g_std_handles[idx])
    g_std_handles[idx] = NewHandle(std::make_shared<StdIoObject>(idx));
  return HandleRet(g_std_handles[idx]);
}

HLE("kernel32.dll", SetStdHandle, 2) { return 1; }

HLE("kernel32.dll", GetFileType, 1) {
  auto o = GetHandle(Arg(c, 0));
  if (!o) {
    SetLastError(kErrorInvalidHandle);
    return 0;  // FILE_TYPE_UNKNOWN
  }
  return o->type == KObject::Type::StdIo ? 2 : 1;  // CHAR : DISK
}

HLE64("kernel32.dll", CloseHandle, 1) {
  if (!CloseHandleValue(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", DuplicateHandle, 7) {
  auto o = GetHandle(Arg(c, 1));
  if (!o) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 3)) Wr32(Arg(c, 3), NewHandle(o));
  return 1;
}

// CreateFile(path, access, share, security, disposition, flags, template).
uint32_t CreateFile(Ctx& c, const std::string& gpath) {
  uint32_t access = uint32_t(Arg(c, 1)), disp = uint32_t(Arg(c, 4));
  std::string path = HostPath(gpath);
  bool write = access & 0x40000000;  // GENERIC_WRITE
  bool read = access & 0x80000000;   // GENERIC_READ
  int flags = write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
  struct stat st;
  bool exists = stat(path.c_str(), &st) == 0;
  // Writes never touch the install (see HostPath): truncating opens get a
  // fresh overlay file, others a copy of the install's.
  if (write && !(exists && S_ISDIR(st.st_mode)))
    path = HostPath(gpath, disp == 2 || disp == 5 || !exists ? PathAccess::kWrite
                                                             : PathAccess::kUpdate);
  switch (disp) {
    case 1: if (exists) { SetLastError(kErrorFileExists); return kInvalidHandle; }
            flags |= O_CREAT; break;                  // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;        // CREATE_ALWAYS
    case 3: break;                                    // OPEN_EXISTING
    case 4: flags |= O_CREAT; break;                  // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;                  // TRUNCATE_EXISTING
  }
  if (exists && S_ISDIR(st.st_mode)) {
    SetLastError(kErrorAccessDenied);
    return kInvalidHandle;
  }
  if (flags & O_CREAT) {
    // Make sure the parent exists in the per-user tree.
    MakeDirs(path.substr(0, path.find_last_of('/')));
  }
  int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) {
    RECOMP_TRACE("  CreateFile(%s) -> %s: %s\n", gpath.c_str(), path.c_str(),
              std::strerror(errno));
    SetLastError(ErrnoToWin(errno));
    return kInvalidHandle;
  }
  RECOMP_TRACE("  CreateFile(%s) -> %s\n", gpath.c_str(), path.c_str());
  auto f = std::make_shared<FileObject>();
  f->fd = fd;
  SetLastError(exists && (disp == 2 || disp == 4) ? kErrorAlreadyExists : 0);
  return NewHandle(f);
}

HLE64("kernel32.dll", CreateFileA, 7) { return HandleRet(CreateFile(c, GStr(Arg(c, 0)))); }
HLE64("kernel32.dll", CreateFileW, 7) { return HandleRet(CreateFile(c, WidePath(Arg(c, 0)))); }

int FdOf(uint64_t h) {
  auto o = GetHandle(h);
  return o ? FileFd(o.get()) : -1;
}

}  // namespace

int HostFdOfHandle(uint64_t h) { return FdOf(h); }

namespace {

HLE64("kernel32.dll", ReadFile, 5) {
  int fd = FdOf(Arg(c, 0));
  gaddr buf = Arg(c, 1), out = Arg(c, 3);
  uint32_t n = uint32_t(Arg(c, 2));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 4)) FatalF("ReadFile with OVERLAPPED is not supported");
  lazy::Touch(buf, n);  // the kernel does not fault on lazy pages
  ssize_t r = n ? read(fd, GPtr(buf), n) : 0;
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  return 1;
}

HLE64("kernel32.dll", WriteFile, 5) {
  int fd = FdOf(Arg(c, 0));
  gaddr buf = Arg(c, 1), out = Arg(c, 3);
  uint32_t n = uint32_t(Arg(c, 2));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  lazy::Touch(buf, n);
  ssize_t r = n ? write(fd, GPtr(buf), n) : 0;
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  return 1;
}

HLE64("kernel32.dll", SetFilePointer, 4) {
  int fd = FdOf(Arg(c, 0));
  int64_t dist = int32_t(Arg(c, 1));
  gaddr hi = Arg(c, 2);
  uint32_t method = uint32_t(Arg(c, 3));
  if (hi) dist = int64_t((uint64_t(Rd32(hi)) << 32) | uint32_t(Arg(c, 1)));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  off_t r = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) {
    SetLastError(kErrorInvalidParameter);
    return 0xFFFFFFFF;
  }
  if (hi) Wr32(hi, uint32_t(uint64_t(r) >> 32));
  SetLastError(0);
  return uint32_t(r);
}

// (hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
// dwMoveMethod). The LARGE_INTEGER takes two stack slots on x86, one
// register on x64.
HLE64("kernel32.dll", SetFilePointerEx, 5) {
  int fd = FdOf(Arg(c, 0));
  const int k = g_proc->x64 ? 0 : 1;
  int64_t dist = g_proc->x64 ? int64_t(Arg(c, 1))
                             : int64_t((uint64_t(Arg(c, 2)) << 32) | uint32_t(Arg(c, 1)));
  gaddr out = Arg(c, 2 + k);
  uint32_t method = uint32_t(Arg(c, 3 + k));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  off_t r = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  if (out) {
    Wr32(out, uint32_t(r));
    Wr32(out + 4, uint32_t(uint64_t(r) >> 32));
  }
  return 1;
}

HLE("kernel32.dll", SetEndOfFile, 1) {
  int fd = FdOf(Arg(c, 0));
  off_t pos = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
  if (pos < 0 || ftruncate(fd, pos) != 0) {
    SetLastError(fd < 0 ? kErrorInvalidHandle : ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", GetFileSize, 2) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(uint64_t(st.st_size) >> 32));
  return uint32_t(st.st_size);
}

HLE("kernel32.dll", GetFileTime, 4) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 1)) PutFileTime(Arg(c, 1), platform::StatModifyTime(st));
  if (Arg(c, 2)) PutFileTime(Arg(c, 2), platform::StatAccessTime(st));
  if (Arg(c, 3)) PutFileTime(Arg(c, 3), platform::StatModifyTime(st));
  return 1;
}

HLE64("kernel32.dll", FlushFileBuffers, 1) {
  int fd = FdOf(Arg(c, 0));
  if (fd >= 0) fsync(fd);
  return 1;
}

// The host file behind guest path g: false (last error set) if none.
bool StatGuest(const std::string& g, struct stat* st, uint32_t* attrs) {
  std::string path = HostPath(g);
  if (stat(path.c_str(), st) != 0) {
    SetLastError(ErrnoToWin(errno));
    return false;
  }
  *attrs = AttributesOf(*st, path.substr(path.find_last_of('/') + 1));
  return true;
}

uint32_t GetFileAttributes(const std::string& g) {
  struct stat st;
  uint32_t a;
  return StatGuest(g, &st, &a) ? a : 0xFFFFFFFF;
}
HLE64("kernel32.dll", GetFileAttributesA, 1) { return GetFileAttributes(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", GetFileAttributesW, 1) { return GetFileAttributes(WidePath(Arg(c, 0))); }

// GetFileAttributesExW(path, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA*).
HLE64("kernel32.dll", GetFileAttributesExW, 3) {
  if (Arg(c, 1) != 0) FatalF("GetFileAttributesExW: info level %llu", (unsigned long long)Arg(c, 1));
  struct stat st;
  uint32_t a;
  if (!StatGuest(WidePath(Arg(c, 0)), &st, &a)) return 0;
  gaddr d = Arg(c, 2);
  Wr32(d + 0, a);
  PutFileTime(d + 4, platform::StatModifyTime(st));   // creation
  PutFileTime(d + 12, platform::StatAccessTime(st));  // access
  PutFileTime(d + 20, platform::StatModifyTime(st));  // write
  Wr32(d + 28, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(d + 32, uint32_t(st.st_size));
  return 1;
}

// GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*). The volume
// serial is GetVolumeInformation's; the file index the host inode.
HLE64("kernel32.dll", GetFileInformationByHandle, 2) {
  auto o = GetHandle(Arg(c, 0));
  struct stat st;
  if (!o || o->type != KObject::Type::File || fstat(FileFd(o.get()), &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  gaddr d = Arg(c, 1);
  Wr32(d + 0, AttributesOf(st, {}));
  PutFileTime(d + 4, platform::StatModifyTime(st));
  PutFileTime(d + 12, platform::StatAccessTime(st));
  PutFileTime(d + 20, platform::StatModifyTime(st));
  Wr32(d + 28, kVolumeSerial);
  Wr32(d + 32, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(d + 36, uint32_t(st.st_size));
  Wr32(d + 40, uint32_t(st.st_nlink));
  Wr32(d + 44, uint32_t(uint64_t(st.st_ino) >> 32));
  Wr32(d + 48, uint32_t(st.st_ino));
  return 1;
}

HLE("kernel32.dll", SetFileAttributesA, 2) { return 1; }
HLE64("kernel32.dll", SetFileAttributesW, 2) { return 1; }

// A file that exists only in the (read-only) install cannot be deleted
// or moved away.
bool InstallOnly(const std::string& gpath) {
  return HostPath(gpath) != HostPath(gpath, PathAccess::kWrite);
}

uint32_t DeleteFile(const std::string& g) {
  if (InstallOnly(g) && Exists(HostPath(g))) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  if (unlink(HostPath(g, PathAccess::kWrite).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", DeleteFileA, 1) { return DeleteFile(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", DeleteFileW, 1) { return DeleteFile(WidePath(Arg(c, 0))); }

HLE("kernel32.dll", CopyFileA, 3) {
  std::string from = HostPath(GStr(Arg(c, 0)));
  std::string to = HostPath(GStr(Arg(c, 1)), PathAccess::kWrite);
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (Arg(c, 2) ? O_EXCL : 0);
  int out = open(to.c_str(), flags, 0644);
  if (out < 0) {
    SetLastError(errno == EEXIST ? 80 /* ERROR_FILE_EXISTS */ : ErrnoToWin(errno));
    close(in);
    return 0;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, size_t(n)) != n) ok = false;
  if (n < 0) ok = false;
  close(in);
  close(out);
  if (!ok) SetLastError(ErrnoToWin(errno));
  return ok ? 1 : 0;
}

uint32_t MoveFile(const std::string& gfrom, const std::string& gto, bool replace) {
  if (InstallOnly(gfrom) && Exists(HostPath(gfrom))) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  std::string from = HostPath(gfrom, PathAccess::kWrite);
  std::string to = HostPath(gto, PathAccess::kWrite);
  struct stat st;
  if (stat(HostPath(gto).c_str(), &st) == 0 && (!replace || S_ISDIR(st.st_mode))) {
    SetLastError(replace ? kErrorAccessDenied : kErrorAlreadyExists);
    return 0;
  }
  if (rename(from.c_str(), to.c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", MoveFileA, 2) { return MoveFile(GStr(Arg(c, 0)), GStr(Arg(c, 1)), false); }

// MoveFileExW(from, to, flags): MOVEFILE_REPLACE_EXISTING, and
// MOVEFILE_COPY_ALLOWED / MOVEFILE_WRITE_THROUGH, which change nothing
// here (one volume; host renames are durable once they return).
HLE64("kernel32.dll", MoveFileExW, 3) {
  uint32_t flags = uint32_t(Arg(c, 2));
  if (flags & ~0xBu) FatalF("MoveFileExW: flags %X not supported", flags);
  return MoveFile(WidePath(Arg(c, 0)), WidePath(Arg(c, 1)), flags & 1);
}

uint32_t CreateDirectory(const std::string& g) {
  if (Exists(HostPath(g))) {
    SetLastError(kErrorAlreadyExists);
    return 0;
  }
  std::string path = HostPath(g, PathAccess::kWrite);
  MakeDirs(path.substr(0, path.find_last_of('/')));
  if (mkdir(path.c_str(), 0755) != 0) {
    SetLastError(errno == EEXIST ? kErrorAlreadyExists : ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", CreateDirectoryA, 2) { return CreateDirectory(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", CreateDirectoryW, 2) { return CreateDirectory(WidePath(Arg(c, 0))); }

uint32_t RemoveDirectory(const std::string& g) {
  if (rmdir(HostPath(g, PathAccess::kWrite).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", RemoveDirectoryA, 1) { return RemoveDirectory(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", RemoveDirectoryW, 1) { return RemoveDirectory(WidePath(Arg(c, 0))); }

HLE("kernel32.dll", GetCurrentDirectoryA, 2) {
  uint32_t size = Arg(c, 0), buf = Arg(c, 1);
  std::string d = GuestCurrentDir();
  return CopyOut(buf, size, d);
}

HLE("kernel32.dll", GetFullPathNameA, 4) {
  std::string full = AbsGuest(GStr(Arg(c, 0)));
  uint32_t size = Arg(c, 1), buf = Arg(c, 2), filepart = Arg(c, 3);
  uint32_t r = CopyOut(buf, size, full);
  if (r == full.size() && filepart) {
    size_t s = full.find_last_of('\\');
    Wr32(filepart, s == std::string::npos || s + 1 == full.size()
                       ? 0
                       : buf + uint32_t(s + 1));
  }
  return r;
}

HLE("kernel32.dll", GetLongPathNameA, 3) {
  std::string full = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  return CopyOut(buf, size, full);
}

HLE("kernel32.dll", GetTempPathA, 2) {
  std::string t = "C:\\TEMP\\";
  MakeDirs(UserDataRoot() + "/TEMP");
  return CopyOut(Arg(c, 1), Arg(c, 0), t);
}

HLE("kernel32.dll", GetTempFileNameA, 4) {
  static uint32_t counter = 1;
  std::string dir = GStr(Arg(c, 0)), prefix = GStr(Arg(c, 1)).substr(0, 3);
  uint32_t unique = Arg(c, 2) ? Arg(c, 2) : counter++;
  char name[32];
  std::snprintf(name, sizeof name, "%s%X.TMP", prefix.c_str(), unique & 0xFFFF);
  std::string full = dir;
  if (!full.empty() && full.back() != '\\') full += '\\';
  full += name;
  PutStr(Arg(c, 3), full);
  if (!Arg(c, 2)) {
    int fd = open(HostPath(full, PathAccess::kWrite).c_str(),
                  O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
  }
  return unique;
}

// ---- find ------------------------------------------------------------------

void FillFindData(uint32_t fd, const std::string& host_dir, const std::string& name) {
  struct stat st{};
  stat((host_dir + "/" + name).c_str(), &st);
  std::memset(GPtr(fd), 0, 320);
  Wr32(fd + 0, AttributesOf(st, name));
  PutFileTime(fd + 4, platform::StatModifyTime(st));   // creation
  PutFileTime(fd + 12, platform::StatAccessTime(st));  // access
  PutFileTime(fd + 20, platform::StatModifyTime(st));  // write
  Wr32(fd + 28, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(fd + 32, uint32_t(st.st_size));
  PutStr(fd + 44, name.substr(0, 259));  // cFileName
  // cAlternateFileName (8.3) left empty.
}

HLE("kernel32.dll", FindFirstFileA, 2) {
  std::string pattern = AbsGuest(GStr(Arg(c, 0)));
  size_t s = pattern.find_last_of('\\');
  std::string gdir = pattern.substr(0, s), mask = pattern.substr(s + 1);
  if (gdir.size() == 2) gdir += '\\';
  auto f = std::make_shared<FindObject>();
  std::string lmask = Lower(mask);
  if (lmask == "*.*") lmask = "*";
  std::set<std::string> seen;  // lower case: the overlay hides the install
  for (const std::string& hdir : HostDirs(gdir)) {
    DIR* d = opendir(hdir.c_str());
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      std::string ln = Lower(e->d_name);
      if (fnmatch(lmask.c_str(), ln.c_str(), 0) == 0 && seen.insert(ln).second)
        f->entries.push_back({hdir, e->d_name});
    }
    closedir(d);
  }
  if (f->entries.empty()) {
    SetLastError(kErrorFileNotFound);
    return kInvalidHandle;
  }
  FillFindData(Arg(c, 1), f->entries[0].first, f->entries[0].second);
  f->next = 1;
  return NewHandle(f);
}

HLE("kernel32.dll", FindNextFileA, 2) {
  auto o = GetHandle(Arg(c, 0));
  if (!o || o->type != KObject::Type::Find) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  auto* f = static_cast<FindObject*>(o.get());
  if (f->next >= f->entries.size()) {
    SetLastError(kErrorNoMoreFiles);
    return 0;
  }
  const auto& [hdir, name] = f->entries[f->next++];
  FillFindData(Arg(c, 1), hdir, name);
  return 1;
}

HLE("kernel32.dll", FindClose, 1) {
  return CloseHandleValue(Arg(c, 0)) ? 1 : 0;
}

// ---- volumes -----------------------------------------------------------------

HLE("kernel32.dll", GetDriveTypeA, 1) { return 3; }  // DRIVE_FIXED

HLE("kernel32.dll", GetLogicalDriveStringsA, 2) {
  const char s[] = "C:\\\0";  // double-NUL terminated list
  if (Arg(c, 0) < sizeof s) return sizeof s;
  std::memcpy(GPtr(Arg(c, 1)), s, sizeof s);
  return sizeof s - 1;
}

HLE("kernel32.dll", GetVolumeInformationA, 8) {
  if (Arg(c, 1) && Arg(c, 2)) PutStr(Arg(c, 1), win32::Upper(Game().id));
  if (Arg(c, 3)) Wr32(Arg(c, 3), kVolumeSerial);
  if (Arg(c, 4)) Wr32(Arg(c, 4), 255);
  if (Arg(c, 5)) Wr32(Arg(c, 5), 0x700FF);
  if (Arg(c, 6) && Arg(c, 7)) PutStr(Arg(c, 6), "NTFS");
  return 1;
}

// (lpPathName): the directory must exist (in the install or the per-user
// tree, as for any read); relative paths resolve against the current one.
HLE("kernel32.dll", SetCurrentDirectoryA, 1) {
  const std::string g = AbsGuest(GStr(Arg(c, 0)));
  struct stat st;
  if (!Exists(HostPath(g), &st) || !S_ISDIR(st.st_mode)) {
    SetLastError(kErrorPathNotFound);
    return 0;
  }
  std::lock_guard<std::mutex> l(g_cwd_mu);
  g_cwd = g;
  return 1;
}

// (lpRootPathName, lpSectorsPerCluster, lpBytesPerSector,
// lpNumberOfFreeClusters, lpTotalNumberOfClusters): 4 KiB clusters and
// just under 2 GiB free of 4 GiB. Games of the GetDiskFreeSpace era
// multiply the three into 32 bits; a larger free figure (Ex: 8 GiB) wraps
// there, to zero at exact multiples of 4 GiB.
HLE("kernel32.dll", GetDiskFreeSpaceA, 5) {
  constexpr uint32_t kSectors = 8, kBytes = 512;
  if (Arg(c, 1)) Wr32(Arg(c, 1), kSectors);
  if (Arg(c, 2)) Wr32(Arg(c, 2), kBytes);
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0x7FFF0000u / (kSectors * kBytes));
  if (Arg(c, 4)) Wr32(Arg(c, 4), 0x100000u);
  return 1;
}

// (hFile, lpCreationTime, lpLastAccessTime, lpLastWriteTime): access and
// write times are set (a host file has no settable creation time); a NULL
// pointer leaves that time unchanged.
HLE("kernel32.dll", SetFileTime, 4) {
  const int fd = FdOf(Arg(c, 0));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  auto ts = [](gaddr ft) {
    timespec t{0, UTIME_OMIT};
    if (!ft) return t;
    const uint64_t v = Rd32(ft) | uint64_t(Rd32(ft + 4)) << 32;
    if (v < kFileTimeUnixEpoch) return timespec{0, 0};
    const uint64_t u = v - kFileTimeUnixEpoch;  // 100 ns units since 1970
    return timespec{time_t(u / 10000000), long(u % 10000000) * 100};
  };
  const timespec times[2] = {ts(Arg(c, 2)), ts(Arg(c, 3))};
  if (futimens(fd, times) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

// (wFatDate, wFatTime, lpFileTime): date bits 0-4 day, 5-8 month, 9-15
// years since 1980; time bits 0-4 seconds / 2, 5-10 minutes, 11-15 hours.
// The FILETIME counts from 1601 without a time zone, like the DOS fields.
HLE("kernel32.dll", DosDateTimeToFileTime, 3) {
  const uint32_t d = Arg(c, 0) & 0xFFFF, t = Arg(c, 1) & 0xFFFF;
  const int day = d & 0x1F, month = (d >> 5) & 0xF, year = 1980 + (d >> 9);
  const int sec = (t & 0x1F) * 2, min = (t >> 5) & 0x3F, hour = t >> 11;
  if (day < 1 || month < 1 || month > 12 || hour > 23 || min > 59 || sec > 59) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  // Days from 1970-01-01 to year-month-day (proleptic Gregorian).
  const int y = year - (month <= 2);
  const int era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t days = int64_t(era) * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  const uint64_t secs = uint64_t(days * 86400 + hour * 3600 + min * 60 + sec);
  const uint64_t ft = kFileTimeUnixEpoch + secs * 10000000;
  Wr32(Arg(c, 2), uint32_t(ft));
  Wr32(Arg(c, 2) + 4, uint32_t(ft >> 32));
  return 1;
}

HLE("kernel32.dll", GetDiskFreeSpaceExA, 4) {
  // 8 GiB free of 64 GiB, reported in 64-bit ULARGE_INTEGERs.
  if (Arg(c, 1)) Wr64(Arg(c, 1), 8ull << 30);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 64ull << 30);
  if (Arg(c, 3)) Wr64(Arg(c, 3), 8ull << 30);
  return 1;
}

}  // namespace
}  // namespace hle
}  // namespace recomp

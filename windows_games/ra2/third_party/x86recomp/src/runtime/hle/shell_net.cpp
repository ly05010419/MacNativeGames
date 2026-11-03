// shfolder and shell32 (special folders, directory trees, command lines)
// and wsock32 (by ordinal).
//
// Special folders live in the per-user tree (see HostPath): a game that
// saves under My Documents writes to $XDG_DATA_HOME/<id>/drive_c/
// Documents and Settings/Player/My Documents/...
//
// Networking: wsock32 only answers host-name queries (DirectPlay8 is not
// provided).

#include <sys/stat.h>

#include <cstring>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

std::string FolderFor(uint32_t csidl) {
  const std::string user = "C:\\Documents and Settings\\Player";
  switch (csidl & 0xFF) {
    case 0x05: return user + "\\My Documents";               // CSIDL_PERSONAL
    case 0x1A: return user + "\\Application Data";           // CSIDL_APPDATA
    case 0x1C: return user + "\\Local Settings\\Application Data";
    case 0x23: return "C:\\Documents and Settings\\All Users\\Application Data";
    case 0x2E: return "C:\\Documents and Settings\\All Users\\Documents";
    case 0x26: return "C:\\Program Files";
    case 0x24: return "C:\\WINDOWS";
    case 0x25: return "C:\\WINDOWS\\system32";
    case 0x10: return user + "\\Desktop";
    default: return {};
  }
}

uint32_t GetFolderPathA(Ctx& c) {
  uint32_t csidl = Arg(c, 1), out = Arg(c, 4);
  std::string p = FolderFor(csidl);
  if (p.empty()) {
    WarnF("SHGetFolderPathA(CSIDL %X): unknown folder\n", csidl);
    return win32::kEInvalidArg;
  }
  // Create it on the host either way: Windows has these folders.
  MakeDirs(HostPath(p));
  PutStr(out, p);
  return 0;  // S_OK
}

// shfolder.dll is the redistributable for systems before Windows 2000;
// later games import the same function from shell32.
HLE_NAMED("shfolder.dll", "SHGetFolderPathA", ShfolderGetFolderPathA, 5) {
  return GetFolderPathA(c);
}
HLE_NAMED("shell32.dll", "SHGetFolderPathA", Shell32GetFolderPathA, 5) {
  return GetFolderPathA(c);
}

HLE("shfolder.dll", SHGetFolderPathW, 5) {
  uint32_t csidl = Arg(c, 1), out = Arg(c, 4);
  std::string p = FolderFor(csidl);
  if (p.empty()) {
    WarnF("SHGetFolderPathW(CSIDL %X): unknown folder\n", csidl);
    return win32::kEInvalidArg;
  }
  MakeDirs(HostPath(p));
  PutWStr(out, p);
  return 0;
}

// SHGetKnownFolderPath(id, flags, token, &path): Vista and later, so the
// layout is Windows 10's (C:\Users\<user>). The path is CoTaskMemAlloc'd.
// Like SHGetFolderPath it creates the folder whatever the flags say.
HLE64("shell32.dll", SHGetKnownFolderPath, 4) {
  struct Known {
    uint8_t id[16];  // the GUID as stored
    const char* path;
  };
  static const Known kKnown[] = {
      {{0xFF, 0x32, 0x5C, 0x4C, 0x9D, 0xBB, 0xB0, 0x43, 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E,
        0xAA, 0xA4},
       "C:\\Users\\Player\\Saved Games"},  // FOLDERID_SavedGames
  };
  gaddr id = Arg(c, 0), out = Arg(c, 3);
  WrPtr(out, 0);
  for (const Known& k : kKnown) {
    if (std::memcmp(GPtr(id), k.id, 16)) continue;
    MakeDirs(HostPath(k.path));
    gaddr buf = HeapAllocProcess(2 * (std::strlen(k.path) + 1), false);
    PutWStr(buf, k.path);
    WrPtr(out, buf);
    return 0;
  }
  WarnF("SHGetKnownFolderPath({%08X-...}): unknown folder\n", Rd32(id));
  return 0x80070002;  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
}

// SHCreateDirectoryExA(hwnd, path, security): the directory and any missing
// parents. Returns a Win32 error code, not a BOOL.
HLE("shell32.dll", SHCreateDirectoryExA, 3) {
  std::string g = GStr(Arg(c, 1));
  bool absolute = (g.size() > 2 && g[1] == ':' && (g[2] == '\\' || g[2] == '/')) ||
                  g.rfind("\\\\", 0) == 0;
  if (!absolute) return 161;  // ERROR_BAD_PATHNAME
  struct stat st;
  if (stat(HostPath(g).c_str(), &st) == 0)
    return S_ISDIR(st.st_mode) ? win32::kErrorAlreadyExists
                               : win32::kErrorFileExists;
  MakeDirs(HostPath(g, PathAccess::kWrite));
  return win32::kErrorSuccess;
}

// ---- wsock32 ------------------------------------------------------------------------

HLE_NAMED("wsock32.dll", "#115", WSAStartup, 2) {  // WSAStartup(version, WSADATA*)
  uint32_t wsa = Arg(c, 1);
  std::memset(GPtr(wsa), 0, 400);
  Wr16(wsa + 0, 0x0101);
  Wr16(wsa + 2, 0x0202);
  PutStr(wsa + 4, "WinSock 2.0");
  PutStr(wsa + 261, "Running");
  Wr16(wsa + 390, 32767);  // iMaxSockets
  Wr16(wsa + 392, 65467);  // iMaxUdpDg
  return 0;
}

HLE_NAMED("wsock32.dll", "#116", WSACleanup, 0) { return 0; }  // WSACleanup

HLE_NAMED("wsock32.dll", "#57", gethostname, 2) {  // gethostname(buf, len)
  std::string n = "localhost";
  if (Arg(c, 1) < n.size() + 1) return 0xFFFFFFFF;
  PutStr(Arg(c, 0), n);
  return 0;
}

// gethostbyname: a static HOSTENT for 127.0.0.1, whatever the name.
HLE_NAMED("wsock32.dll", "#52", gethostbyname, 1) {
  static uint32_t hostent = 0;
  if (!hostent) {
    hostent = HeapAllocProcess(64, true);
    uint32_t name = hostent + 16, addr = hostent + 32, list = hostent + 40;
    PutStr(name, "localhost");
    Wr8(addr + 0, 127);
    Wr8(addr + 3, 1);
    Wr32(list, addr);
    Wr32(list + 4, 0);
    Wr32(hostent + 0, name);     // h_name
    Wr32(hostent + 4, list + 4); // h_aliases: empty list
    Wr16(hostent + 8, 2);        // h_addrtype AF_INET
    Wr16(hostent + 10, 4);       // h_length
    Wr32(hostent + 12, list);    // h_addr_list
  }
  return hostent;
}

HLE_NAMED("wsock32.dll", "#14", htonl, 1) { return __builtin_bswap32(Arg(c, 0)); }  // htonl

HLE_NAMED("wsock32.dll", "#10", ioctlsocket, 3) {  // ioctlsocket: no sockets exist
  return 0xFFFFFFFF;
}

}  // namespace
// CommandLineToArgvW(line, &argc): one LocalAlloc block holding the
// pointer array (null-terminated) and the strings; LocalFree frees it.
HLE64("shell32.dll", CommandLineToArgvW, 2) {
  std::vector<std::u16string> args = win32::SplitCommandLine(GWStr(Arg(c, 0)));
  const uint32_t ptr = g_proc->x64 ? 8 : 4;
  uint64_t size = ptr * (args.size() + 1);
  for (const auto& a : args) size += 2 * (a.size() + 1);
  gaddr block = HeapAllocProcess(size, true);
  gaddr str = block + ptr * (args.size() + 1);
  for (size_t i = 0; i < args.size(); i++) {
    WrPtr(block + ptr * i, str);
    std::memcpy(GPtr(str), args[i].data(), 2 * args[i].size());
    str += 2 * (args[i].size() + 1);
  }
  Wr32(Arg(c, 1), uint32_t(args.size()));
  return block;
}

}  // namespace recomp::hle

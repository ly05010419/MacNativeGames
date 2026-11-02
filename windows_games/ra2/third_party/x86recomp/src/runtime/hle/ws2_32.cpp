// ws2_32 (Winsock 2): UDP and TCP sockets on host BSD sockets. SOCKETs are
// handles in the kernel32 table; sockaddrs are converted (the families'
// values differ, and BSD's have a length byte). Imported by ordinal or by
// name, so each function is registered under both.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

// Windows' address families and the WSA error codes used here.
constexpr uint16_t kAfInet = 2, kAfInet6 = 23;
constexpr uint32_t kWsaEInval = 10022, kWsaEWouldBlock = 10035, kWsaEMsgSize = 10040,
                   kWsaEAfNoSupport = 10047, kWsaEAddrInUse = 10048,
                   kWsaEAddrNotAvail = 10049, kWsaENetDown = 10050, kWsaENetUnreach = 10051,
                   kWsaEConnReset = 10054, kWsaENotSock = 10038, kWsaEAcces = 10013,
                   kWsaEConnRefused = 10061, kWsaEHostUnreach = 10065;

struct SocketObject : KObject {
  SocketObject() : KObject(Type::Socket) {}
  ~SocketObject() override {
    if (fd >= 0) close(fd);
  }
  int fd = -1;
};

thread_local uint32_t t_wsa_error = 0;

uint32_t WsaError(int e) {
  switch (e) {
    case EAGAIN: return kWsaEWouldBlock;
    case EINVAL: return kWsaEInval;
    case EMSGSIZE: return kWsaEMsgSize;
    case EAFNOSUPPORT: return kWsaEAfNoSupport;
    case EADDRINUSE: return kWsaEAddrInUse;
    case EADDRNOTAVAIL: return kWsaEAddrNotAvail;
    case ENETUNREACH: return kWsaENetUnreach;
    case EHOSTUNREACH: return kWsaEHostUnreach;
    case ECONNRESET: return kWsaEConnReset;
    case ECONNREFUSED: return kWsaEConnRefused;
    case EACCES: return kWsaEAcces;
    case ENOTSOCK: return kWsaENotSock;
    default: return kWsaENetDown;
  }
}

constexpr uint32_t kSocketError = 0xFFFFFFFF;  // SOCKET_ERROR (int -1)

// Sends never raise SIGPIPE (Winsock reports the error instead).
#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;  // macOS: SO_NOSIGPIPE on the socket
#endif

uint64_t Fail(uint32_t wsa) {
  t_wsa_error = wsa;
  return kSocketError;
}
uint64_t FailErrno() { return Fail(WsaError(errno)); }

uint64_t InvalidSocket() { return g_proc->x64 ? ~0ull : 0xFFFFFFFFu; }

int SocketFd(uint64_t s) {
  auto o = std::dynamic_pointer_cast<SocketObject>(GetHandle(uint32_t(s)));
  return o ? o->fd : -1;
}

int HostFamily(uint16_t af) {
  return af == kAfInet ? AF_INET : af == kAfInet6 ? AF_INET6 : -1;
}

// Guest sockaddr (Windows layout) to host; false if the family is unknown.
bool ToHost(gaddr sa, uint32_t len, sockaddr_storage* out, socklen_t* out_len) {
  std::memset(out, 0, sizeof *out);
  uint16_t af = Rd16(sa);
  if (af == kAfInet && len >= 16) {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    a->sin_family = AF_INET;
    std::memcpy(&a->sin_port, GPtr(sa + 2), 2);
    std::memcpy(&a->sin_addr, GPtr(sa + 4), 4);
    *out_len = sizeof *a;
    return true;
  }
  if (af == kAfInet6 && len >= 28) {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    a->sin6_family = AF_INET6;
    std::memcpy(&a->sin6_port, GPtr(sa + 2), 2);
    std::memcpy(&a->sin6_flowinfo, GPtr(sa + 4), 4);
    std::memcpy(&a->sin6_addr, GPtr(sa + 8), 16);
    std::memcpy(&a->sin6_scope_id, GPtr(sa + 24), 4);
    *out_len = sizeof *a;
    return true;
  }
  return false;
}

// Host sockaddr to the guest buffer `sa` of *len bytes (updated).
void ToGuest(const sockaddr_storage& in, gaddr sa, gaddr len_va) {
  if (!sa || !len_va) return;
  uint32_t cap = Rd32(len_va);
  uint8_t b[28] = {};
  uint32_t n = 0;
  if (in.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&in);
    b[0] = kAfInet;
    std::memcpy(b + 2, &a->sin_port, 2);
    std::memcpy(b + 4, &a->sin_addr, 4);
    n = 16;
  } else if (in.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&in);
    b[0] = kAfInet6;
    std::memcpy(b + 2, &a->sin6_port, 2);
    std::memcpy(b + 4, &a->sin6_flowinfo, 4);
    std::memcpy(b + 8, &a->sin6_addr, 16);
    std::memcpy(b + 24, &a->sin6_scope_id, 4);
    n = 28;
  }
  std::memcpy(GPtr(sa), b, std::min(cap, n));
  Wr32(len_va, n);
}

}  // namespace

// Every function here is also a Winsock 1.1 export of wsock32.dll: the
// ordinals 1-23, 51-57 and 101-116 are the same in both DLLs.
#define RECOMP_WS2(ord, name, nargs)                                                \
  static uint64_t ws_##name(::recomp::Ctx& c);                                      \
  static ::recomp::hle::Registrar ws_reg_##name{"ws2_32.dll", #name, nargs,         \
                                                &ws_##name, true};                  \
  static ::recomp::hle::Registrar ws_ord_##name{"ws2_32.dll", "#" #ord, nargs,      \
                                                &ws_##name, true};                  \
  static ::recomp::hle::Registrar ws1_reg_##name{"wsock32.dll", #name, nargs,       \
                                                 &ws_##name};                       \
  static ::recomp::hle::Registrar ws1_ord_##name{"wsock32.dll", "#" #ord, nargs,    \
                                                 &ws_##name};                       \
  static uint64_t ws_##name([[maybe_unused]] ::recomp::Ctx& c)

// WSADATA. wVersion is the version the caller asked for when it is 2.2 or
// lower (a Winsock 1.1 caller gets 1.1), else 2.2; a request below 1.0 is
// WSAVERNOTSUPPORTED. Win64 moves iMaxSockets, iMaxUdpDg and lpVendorInfo
// before the strings.
RECOMP_WS2(115, WSAStartup, 2) {
  constexpr uint32_t kWsaVerNotSupported = 10092;
  const uint16_t want = uint16_t(Arg(c, 0));
  const uint8_t major = want & 0xFF, minor = want >> 8;
  if (major < 1) return kWsaVerNotSupported;
  const uint16_t version = major < 2 || (major == 2 && minor <= 2) ? want : 0x0202;
  gaddr d = Arg(c, 1);
  const bool x64 = g_proc->x64;
  std::memset(GPtr(d), 0, x64 ? 408 : 400);
  Wr16(d + 0, version);
  Wr16(d + 2, 0x0202);
  PutStr(d + (x64 ? 16 : 4), "WinSock 2.0");
  PutStr(d + (x64 ? 16 + 257 : 4 + 257), "Running");
  return 0;
}
RECOMP_WS2(116, WSACleanup, 0) { return 0; }
RECOMP_WS2(111, WSAGetLastError, 0) { return t_wsa_error; }
RECOMP_WS2(112, WSASetLastError, 1) {
  t_wsa_error = uint32_t(Arg(c, 0));
  return 0;
}

RECOMP_WS2(23, socket, 3) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return InvalidSocket();
  }
  int fd = ::socket(af, int(Arg(c, 1)), int(Arg(c, 2)));
  if (fd < 0) {
    t_wsa_error = WsaError(errno);
    return InvalidSocket();
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  auto o = std::make_shared<SocketObject>();
  o->fd = fd;
  return NewHandle(o);
}

RECOMP_WS2(3, closesocket, 1) {
  if (SocketFd(Arg(c, 0)) < 0) return Fail(kWsaENotSock);
  CloseHandleValue(uint32_t(Arg(c, 0)));
  return 0;
}

RECOMP_WS2(2, bind, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa;
  socklen_t len;
  if (!ToHost(Arg(c, 1), uint32_t(Arg(c, 2)), &sa, &len)) return Fail(kWsaEAfNoSupport);
  return ::bind(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0 ? 0 : FailErrno();
}

// ioctlsocket: FIONBIO (non-blocking on/off) and FIONREAD.
RECOMP_WS2(10, ioctlsocket, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  uint32_t cmd = uint32_t(Arg(c, 1));
  gaddr argp = Arg(c, 2);
  if (cmd == 0x8004667E) {  // FIONBIO
    int fl = fcntl(fd, F_GETFL);
    fl = Rd32(argp) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl) == 0 ? 0 : FailErrno();
  }
  if (cmd == 0x4004667F) {  // FIONREAD
    int n = 0;
    if (ioctl(fd, FIONREAD, &n) != 0) return FailErrno();
    Wr32(argp, uint32_t(n));
    return 0;
  }
  WarnF("ioctlsocket(%08X): not supported\n", cmd);
  return Fail(kWsaEInval);
}

RECOMP_WS2(20, sendto, 6) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa;
  socklen_t len = 0;
  gaddr to = Arg(c, 4);
  if (to && !ToHost(to, uint32_t(Arg(c, 5)), &sa, &len)) return Fail(kWsaEAfNoSupport);
  ssize_t n = ::sendto(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))),
                       int(Arg(c, 3)) | kNoSignal,
                       to ? reinterpret_cast<sockaddr*>(&sa) : nullptr, len);
  return n < 0 ? FailErrno() : uint64_t(uint32_t(n));
}

RECOMP_WS2(17, recvfrom, 6) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa{};
  socklen_t len = sizeof sa;
  ssize_t n = ::recvfrom(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))), int(Arg(c, 3)),
                         reinterpret_cast<sockaddr*>(&sa), &len);
  if (n < 0) return FailErrno();
  ToGuest(sa, Arg(c, 4), Arg(c, 5));
  return uint32_t(n);
}

// Byte order: the guest is little-endian, network order big-endian.
RECOMP_WS2(8, htonl, 1) { return __builtin_bswap32(uint32_t(Arg(c, 0))); }
RECOMP_WS2(14, ntohl, 1) { return __builtin_bswap32(uint32_t(Arg(c, 0))); }
RECOMP_WS2(9, htons, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }
RECOMP_WS2(15, ntohs, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }

// inet_pton / inet_ntop: the host's, with Windows' family values.
HLE64("ws2_32.dll", inet_pton, 3) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return kSocketError;
  }
  return uint32_t(::inet_pton(af, GStr(Arg(c, 1)).c_str(), GPtr(Arg(c, 2))));
}
HLE64("ws2_32.dll", inet_ntop, 4) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return 0;
  }
  char buf[64];
  if (!::inet_ntop(af, GPtr(Arg(c, 1)), buf, sizeof buf)) {
    t_wsa_error = WsaError(errno);
    return 0;
  }
  uint64_t cap = Arg(c, 3);
  if (std::strlen(buf) + 1 > cap) {
    t_wsa_error = kWsaEInval;
    return 0;
  }
  PutStr(Arg(c, 2), buf);
  return Arg(c, 2);
}

// wsock32 EnumProtocolsA(lpiProtocols, lpProtocolBuffer, lpdwBufferLength):
// the transports this runtime provides, TCP and UDP over IPv4, as
// PROTOCOL_INFOA records (32 bytes: dwServiceFlags, iAddressFamily,
// iMaxSockAddr, iMinSockAddr, iSocketType, iProtocol, dwMessageSize,
// lpProtocol) followed by their names. lpiProtocols, when given, is a
// zero-terminated list of protocol numbers to report. A buffer too small
// for them sets *lpdwBufferLength to the size needed and fails with
// ERROR_INSUFFICIENT_BUFFER. Returns the number of records.
HLE_NAMED("wsock32.dll", "#1111", EnumProtocolsA_ord, 3) {
  constexpr uint32_t kErrorInsufficientBuffer = 122;
  struct Proto {
    uint32_t flags, type, proto, msg_size;
    const char* name;
  };
  // XP_ flags: GUARANTEED_DELIVERY 0x2, GUARANTEED_ORDER 0x4, GRACEFUL_CLOSE
  // 0x20, EXPEDITED_DATA 0x40, FRAGMENTATION 0x1000; CONNECTIONLESS 0x1,
  // MESSAGE_ORIENTED 0x8, SUPPORTS_BROADCAST 0x200, SUPPORTS_MULTICAST 0x400.
  // UDP's largest message: 65535 - 20 (IPv4 header) - 8 (UDP header).
  static constexpr Proto kProtos[] = {
      {0x1066, 1 /*SOCK_STREAM*/, 6 /*IPPROTO_TCP*/, 0, "TCP/IP"},
      {0x1609, 2 /*SOCK_DGRAM*/, 17 /*IPPROTO_UDP*/, 65507, "UDP/IP"},
  };
  const gaddr wanted = Arg(c, 0), buf = Arg(c, 1), len_va = Arg(c, 2);
  auto selected = [&](uint32_t proto) {
    if (!wanted) return true;
    for (gaddr p = wanted; Rd32(p); p += 4)
      if (Rd32(p) == proto) return true;
    return false;
  };
  uint32_t count = 0, need = 0;
  for (const Proto& p : kProtos)
    if (selected(p.proto)) {
      count++;
      need += 32 + uint32_t(std::strlen(p.name)) + 1;
    }
  if (!len_va || Rd32(len_va) < need || (!buf && need)) {
    if (len_va) Wr32(len_va, need);
    t_wsa_error = kErrorInsufficientBuffer;
    return kSocketError;
  }
  gaddr rec = buf, str = buf + 32 * count;
  for (const Proto& p : kProtos) {
    if (!selected(p.proto)) continue;
    Wr32(rec + 0, p.flags);
    Wr32(rec + 4, kAfInet);
    Wr32(rec + 8, 16);   // sizeof(sockaddr_in)
    Wr32(rec + 12, 16);
    Wr32(rec + 16, p.type);
    Wr32(rec + 20, p.proto);
    Wr32(rec + 24, p.msg_size);
    Wr32(rec + 28, uint32_t(str));
    PutStr(str, p.name);
    rec += 32;
    str += uint32_t(std::strlen(p.name)) + 1;
  }
  return count;
}

}  // namespace recomp::hle

// Galaxy64.dll (the GOG Galaxy SDK's C++ API; docs/galaxy.md) for x64
// guests, answering as a PC without the Galaxy service: Init and
// ProcessData succeed, sign-in never does. The accessors (User(),
// Utils()...) return guest objects whose vtables are thunks named
// "<Interface>::#<slot>": a method gets a handler once a game is seen to
// call it, and calling one without is fatal and names it, as an
// unimplemented import is.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "galaxy64.dll";
constexpr int kSlots = 64;  // more than any Galaxy interface has

// The guest object for interface `name`: {vtable} with kSlots thunks.
gaddr Interface(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, gaddr> objs;
  std::lock_guard<std::mutex> l(mu);
  gaddr& o = objs[name];
  if (o) return o;
  gaddr vt = HeapAllocProcess(8 * kSlots, true);
  for (int i = 0; i < kSlots; i++)
    Wr64(vt + 8 * i, ThunkFor(kDll, name + "::#" + std::to_string(i)));
  o = HeapAllocProcess(16, true);
  Wr64(o, vt);
  return o;
}

// Registered listeners by ListenerType (IListenerRegistrar::Register).
std::mutex g_listeners_mu;
std::multimap<uint32_t, gaddr> g_listeners;
bool g_auth_failure_pending = false;  // delivered by the next ProcessData

constexpr uint32_t kListenerAuth = 7;
// IAuthListener: [1] OnAuthSuccess, [2] OnAuthFailure(FailureReason),
// [3] OnAuthLost.
constexpr int kOnAuthFailure = 2;
constexpr uint32_t kFailureServiceNotAvailable = 1;  // FAILURE_REASON_GALAXY_SERVICE_NOT_AVAILABLE

}  // namespace

// IListenerRegistrar: [1] Register(ListenerType, IGalaxyListener*),
// [2] Unregister(ListenerType, IGalaxyListener*).
HLE_NAMED_X(kDll, "IListenerRegistrar::#1", GalaxyRegister, kCdecl, true) {
  uint32_t type = uint32_t(Arg(c, 1));
  gaddr listener = Arg(c, 2);
  RECOMP_TRACE("  galaxy: listener %08llX registered for type %u\n",
               (unsigned long long)listener, type);
  std::lock_guard<std::mutex> l(g_listeners_mu);
  g_listeners.emplace(type, listener);
  return 0;
}
HLE_NAMED_X(kDll, "IListenerRegistrar::#2", GalaxyUnregister, kCdecl, true) {
  uint32_t type = uint32_t(Arg(c, 1));
  gaddr listener = Arg(c, 2);
  std::lock_guard<std::mutex> l(g_listeners_mu);
  for (auto it = g_listeners.lower_bound(type); it != g_listeners.end() && it->first == type; ++it)
    if (it->second == listener) {
      g_listeners.erase(it);
      break;
    }
  return 0;
}

// IUser: [7] SignInGalaxy(bool requireOnline, IAuthListener*). Without the
// service it fails: the next ProcessData calls the registered auth
// listeners' OnAuthFailure(GALAXY_SERVICE_NOT_AVAILABLE).
HLE_NAMED_X(kDll, "IUser::#7", GalaxySignInGalaxy, kCdecl, true) {
  if (Arg(c, 2)) FatalF("galaxy64: SignInGalaxy with a specific listener is not supported");
  RECOMP_TRACE("  galaxy: SignInGalaxy(requireOnline=%u): no service\n",
               unsigned(Arg(c, 1) & 0xFF));
  std::lock_guard<std::mutex> l(g_listeners_mu);
  g_auth_failure_pending = true;
  return 0;
}

// IApps: [5] the current game language (ProductID = 0 -> const char*),
// GetCurrentGameLanguage or its language-code form: identified by the call
// shape only. No service answers: NULL, which callers check for.
HLE_NAMED_X(kDll, "IApps::#5", GalaxyGameLanguage, kCdecl, true) { return 0; }

HLE_NAMED_X(kDll, "?Init@api@galaxy@@YAXAEBUInitOptions@12@@Z", GalaxyInit, kCdecl, true) {
  RECOMP_TRACE("  galaxy: Init (no Galaxy service: offline)\n");
  return 0;
}
HLE_NAMED_X(kDll, "?ProcessData@api@galaxy@@YAXXZ", GalaxyProcessData, kCdecl, true) {
  std::vector<gaddr> auth;
  {
    std::lock_guard<std::mutex> l(g_listeners_mu);
    if (!g_auth_failure_pending) return 0;
    g_auth_failure_pending = false;
    for (auto it = g_listeners.lower_bound(kListenerAuth);
         it != g_listeners.end() && it->first == kListenerAuth; ++it)
      auth.push_back(it->second);
  }
  // Unlocked: a callback may register or unregister listeners.
  for (gaddr listener : auth) {
    RECOMP_TRACE("  galaxy: OnAuthFailure(service not available) -> %08llX\n",
                 (unsigned long long)listener);
    CallGuest(c, Rd64(Rd64(listener) + 8 * kOnAuthFailure),
              {listener, kFailureServiceNotAvailable});
  }
  return 0;
}
HLE_NAMED_X(kDll, "?Shutdown@api@galaxy@@YAXXZ", GalaxyShutdown, kCdecl, true) { return 0; }

#define RECOMP_GALAXY_ACCESSOR(fn, iface)                                       \
  HLE_NAMED_X(kDll, "?" #fn "@api@galaxy@@YAPEAV" #iface "@12@XZ", Galaxy##fn, \
              kCdecl, true) {                                                   \
    return Interface(#iface);                                                   \
  }
RECOMP_GALAXY_ACCESSOR(Apps, IApps)
RECOMP_GALAXY_ACCESSOR(Friends, IFriends)
RECOMP_GALAXY_ACCESSOR(ListenerRegistrar, IListenerRegistrar)
RECOMP_GALAXY_ACCESSOR(Stats, IStats)
RECOMP_GALAXY_ACCESSOR(User, IUser)
RECOMP_GALAXY_ACCESSOR(Utils, IUtils)
#undef RECOMP_GALAXY_ACCESSOR

}  // namespace recomp::hle

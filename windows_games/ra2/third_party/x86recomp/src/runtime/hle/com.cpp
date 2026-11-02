#include "runtime/hle/com.h"

#include <cstring>
#include <map>
#include <mutex>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::com {

namespace {

constexpr uint32_t kMagic = 0x4D4F4352;  // 'RCOM'

std::recursive_mutex g_mu;
std::map<std::string, uint32_t> g_vtables;
std::map<uint32_t, std::shared_ptr<Object>> g_objects;  // by guest pointer
std::vector<std::pair<guest::GUID, CreateFn>> g_classes;

}  // namespace

uint32_t Vtable(const char* dll, const char* iface,
                const std::vector<const char*>& methods) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_vtables.find(iface);
  if (it != g_vtables.end()) return it->second;
  uint32_t vt = HeapAllocProcess(uint32_t(methods.size() * 4), false);
  for (size_t i = 0; i < methods.size(); i++)
    Wr32(vt + 4 * uint32_t(i),
         hle::ThunkFor(dll, std::string(iface) + "::" + methods[i]));
  g_vtables[iface] = vt;
  return vt;
}

uint32_t Publish(std::shared_ptr<Object> obj, uint32_t vtable) {
  uint32_t p = HeapAllocProcess(16, true);
  Wr32(p, vtable);
  Wr32(p + 4, kMagic);
  Wr32(p + 8, p);
  obj->guest = p;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_objects[p] = std::move(obj);
  return p;
}

std::shared_ptr<Object> Lookup(uint32_t guest) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_objects.find(guest);
  return it == g_objects.end() ? nullptr : it->second;
}

uint32_t AddRef(uint32_t guest) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_objects.find(guest);
  if (it == g_objects.end()) FatalF("AddRef on unknown COM object %08X", guest);
  return ++it->second->refs;
}

uint32_t Release(uint32_t guest) {
  std::shared_ptr<Object> dead;
  uint32_t left;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_objects.find(guest);
    if (it == g_objects.end()) FatalF("Release on unknown COM object %08X", guest);
    left = --it->second->refs;
    if (!left) {
      dead = it->second;
      g_objects.erase(it);
      // Poison the guest block so a use-after-release is caught.
      Wr32(guest, 0);
      Wr32(guest + 4, 0);
      HeapFreeProcess(guest);
    }
  }
  return left;  // `dead` destroys the host object outside the lock
}

guest::GUID ReadGuid(uint32_t va) {
  guest::GUID g;
  std::memcpy(&g, GPtr(va), sizeof g);
  return g;
}

bool GuidEq(uint32_t guest_guid, const guest::GUID& g) {
  return guest_guid && std::memcmp(GPtr(guest_guid), &g, sizeof g) == 0;
}

void RegisterCoClass(const guest::GUID& clsid, CreateFn create) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_classes.emplace_back(clsid, create);
}

CreateFn FindCoClass(uint32_t clsid) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (const auto& [g, create] : g_classes)
    if (GuidEq(clsid, g)) return create;
  return nullptr;
}

uint32_t AllocBstr(const std::u16string& s) {
  const uint32_t bytes = uint32_t(s.size() * 2);
  const uint32_t p = HeapAllocProcess(4 + bytes + 2, false);
  Wr32(p, bytes);
  std::memcpy(GPtr(p + 4), s.data(), bytes);
  Wr16(p + 4 + bytes, 0);
  return p + 4;
}

void FreeBstr(uint32_t bstr) {
  if (bstr) HeapFreeProcess(bstr - 4);
}

}  // namespace recomp::com

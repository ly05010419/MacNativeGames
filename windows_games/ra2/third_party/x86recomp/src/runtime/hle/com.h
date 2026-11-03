#pragma once
// Guest-visible COM objects backed by host C++ objects.
//
// A guest interface pointer points at a small block in guest memory:
//   +0 vtable (guest array of thunk addresses, one per method)
//   +4 magic 'RCOM'
//   +8 object id (host table key)
// Methods are HLE handlers named "Interface::Method" (in the DLL that
// provides the interface), so an unimplemented method fails by name.
// Each host object has exactly one guest pointer; QueryInterface for the
// same interface returns it again, other interfaces are separate objects.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/hle/ddraw_types.h"

namespace recomp::com {

struct Object {
  virtual ~Object() = default;
  uint32_t guest = 0;  // the guest interface pointer
  uint32_t refs = 1;
};

// Builds (once per name) the guest vtable for `iface` with the given
// method names, in vtable order.
uint32_t Vtable(const char* dll, const char* iface,
                const std::vector<const char*>& methods);

// Gives `obj` its guest pointer with `vtable`. Returns the pointer.
uint32_t Publish(std::shared_ptr<Object> obj, uint32_t vtable);

// Host object behind a guest interface pointer (nullptr if not ours).
std::shared_ptr<Object> Lookup(uint32_t guest);

template <typename T>
T* As(uint32_t guest) {
  auto o = Lookup(guest);
  return dynamic_cast<T*>(o.get());
}

uint32_t AddRef(uint32_t guest);
// Drops a reference; destroys the object at zero. Returns the new count.
uint32_t Release(uint32_t guest);

bool GuidEq(uint32_t guest_guid, const guest::GUID& g);
guest::GUID ReadGuid(uint32_t va);

// In-process classes for CoCreateInstance, registered by the HLE module
// that provides them. `create` gets the requested IID (a guest GUID*),
// writes the interface pointer (or 0) to *out and returns the HRESULT.
using CreateFn = uint32_t (*)(uint32_t iid, uint32_t* out);
void RegisterCoClass(const guest::GUID& clsid, CreateFn create);
// The class for a guest CLSID*; null if none is registered.
CreateFn FindCoClass(uint32_t clsid);
// Registers a class at load time (a static of the providing module).
struct CoClassRegistrar {
  CoClassRegistrar(const guest::GUID& clsid, CreateFn create) {
    RegisterCoClass(clsid, create);
  }
};

// BSTRs (oleaut32 SysAllocString/SysFreeString): the byte length, then
// the characters and a NUL; the pointer is to the characters.
uint32_t AllocBstr(const std::u16string& s);
void FreeBstr(uint32_t bstr);

constexpr uint32_t kS_OK = 0, kE_NOINTERFACE = 0x80004002;

}  // namespace recomp::com

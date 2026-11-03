// WMI as far as games use it to tell XInput pads from other DirectInput
// game controllers (the DirectX SDK's IsXInputDevice): the WbemLocator
// class, the root\cimv2 namespace and its Win32_PNPEntity instances'
// DeviceID. The PnP devices listed are the XInput pads (xinput.h), with
// the DeviceID Windows gives an XInput HID gamepad,
// HID\VID_045E&PID_028E&IG_00\<instance>: "IG_" marks an XInput
// interface, and VID/PID match the product GUID DirectInput reports for
// the pad (dinput8.cpp).

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/win32.h"
#include "runtime/hle/xinput.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kCLSID_WbemLocator = {0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IWbemLocator = {0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
constexpr GUID kIID_IWbemServices = {0x9556DC99, 0x828C, 0x11CF, {0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x32, 0x40, 0xC7}};
constexpr GUID kIID_IEnumWbemClassObject = {0x027947E1, 0xD731, 0x11CE, {0xA3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
constexpr GUID kIID_IWbemClassObject = {0xDC12A681, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};

constexpr uint32_t S_OK = 0, WBEM_S_FALSE = 1, E_NOINTERFACE = 0x80004002,
                   E_POINTER = 0x80004003, WBEM_E_NOT_FOUND = 0x80041002,
                   WBEM_E_INVALID_NAMESPACE = 0x8004100E,
                   WBEM_E_INVALID_CLASS = 0x80041010;
constexpr uint16_t VT_BSTR = 8;
constexpr uint32_t CIM_STRING = 8;

struct Locator : com::Object {};
struct Services : com::Object {};
struct InstanceEnum : com::Object {
  std::vector<std::u16string> device_ids;
  size_t next = 0;
};
struct Instance : com::Object {
  std::u16string device_id;
};

uint32_t LocatorVtable() {
  return com::Vtable("wbemprox.dll", "IWbemLocator",
                     {"QueryInterface", "AddRef", "Release", "ConnectServer"});
}

uint32_t ServicesVtable() {
  return com::Vtable(
      "wbemprox.dll", "IWbemServices",
      {"QueryInterface", "AddRef", "Release", "OpenNamespace", "CancelAsyncCall",
       "QueryObjectSink", "GetObject", "GetObjectAsync", "PutClass",
       "PutClassAsync", "DeleteClass", "DeleteClassAsync", "CreateClassEnum",
       "CreateClassEnumAsync", "PutInstance", "PutInstanceAsync",
       "DeleteInstance", "DeleteInstanceAsync", "CreateInstanceEnum",
       "CreateInstanceEnumAsync", "ExecQuery", "ExecQueryAsync",
       "ExecNotificationQuery", "ExecNotificationQueryAsync", "ExecMethod",
       "ExecMethodAsync"});
}

uint32_t EnumVtable() {
  return com::Vtable("wbemprox.dll", "IEnumWbemClassObject",
                     {"QueryInterface", "AddRef", "Release", "Reset", "Next",
                      "NextAsync", "Clone", "Skip"});
}

uint32_t InstanceVtable() {
  return com::Vtable(
      "wbemprox.dll", "IWbemClassObject",
      {"QueryInterface", "AddRef", "Release", "GetQualifierSet", "Get", "Put",
       "Delete", "GetNames", "BeginEnumeration", "Next", "EndEnumeration",
       "GetPropertyQualifierSet", "Clone", "GetObjectText", "SpawnDerivedClass",
       "SpawnInstance", "CompareTo", "GetPropertyOrigin", "InheritsFrom",
       "GetMethod", "PutMethod", "DeleteMethod", "BeginMethodEnumeration",
       "NextMethod", "EndMethodEnumeration", "GetMethodQualifierSet",
       "GetMethodOrigin"});
}

template <typename T>
T* Self(Ctx& c, const char* iface) {
  T* o = com::As<T>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

uint32_t QueryInterfaceFor(Ctx& c, const GUID& iid) {
  if (com::GuidEq(Arg(c, 1), iid) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return S_OK;
  }
  Wr32(Arg(c, 2), 0);
  return E_NOINTERFACE;
}

bool EqualsIgnoreCase(const std::u16string& a, const char* b) {
  std::string n = win32::Narrow(a);
  if (n.size() != std::strlen(b)) return false;
  for (size_t i = 0; i < n.size(); i++)
    if (std::tolower(uint8_t(n[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}

uint32_t CreateLocator(uint32_t iid, uint32_t* out) {
  if (!com::GuidEq(iid, kIID_IWbemLocator) && !com::GuidEq(iid, kIID_IUnknown)) {
    *out = 0;
    return E_NOINTERFACE;
  }
  *out = com::Publish(std::make_shared<Locator>(), LocatorVtable());
  return S_OK;
}

com::CoClassRegistrar reg_locator{kCLSID_WbemLocator, &CreateLocator};

// ---- IWbemLocator -------------------------------------------------------------

HLE_NAMED("wbemprox.dll", "IWbemLocator::QueryInterface", Loc_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IWbemLocator);
}
HLE_NAMED("wbemprox.dll", "IWbemLocator::AddRef", Loc_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wbemprox.dll", "IWbemLocator::Release", Loc_Release, 1) { return com::Release(Arg(c, 0)); }

// (strNetworkResource, strUser, strPassword, strLocale, lSecurityFlags,
// strAuthority, pCtx, ppNamespace): the local root\cimv2 only.
HLE_NAMED("wbemprox.dll", "IWbemLocator::ConnectServer", Loc_ConnectServer, 9) {
  Self<Locator>(c, "IWbemLocator");
  uint32_t out = Arg(c, 8);
  if (!out) return E_POINTER;
  Wr32(out, 0);
  std::u16string ns = Arg(c, 1) ? GWStr(Arg(c, 1)) : u"";
  if (!EqualsIgnoreCase(ns, "\\\\.\\root\\cimv2") && !EqualsIgnoreCase(ns, "root\\cimv2")) {
    WarnF("wbem: ConnectServer(%s): namespace not available\n", win32::Narrow(ns).c_str());
    return WBEM_E_INVALID_NAMESPACE;
  }
  Wr32(out, com::Publish(std::make_shared<Services>(), ServicesVtable()));
  return S_OK;
}

// ---- IWbemServices ------------------------------------------------------------

HLE_NAMED("wbemprox.dll", "IWbemServices::QueryInterface", Svc_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IWbemServices);
}
HLE_NAMED("wbemprox.dll", "IWbemServices::AddRef", Svc_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wbemprox.dll", "IWbemServices::Release", Svc_Release, 1) { return com::Release(Arg(c, 0)); }

// (strFilter, lFlags, pCtx, ppEnum): Win32_PNPEntity, the XInput pads.
HLE_NAMED("wbemprox.dll", "IWbemServices::CreateInstanceEnum", Svc_CreateInstanceEnum, 5) {
  Self<Services>(c, "IWbemServices");
  uint32_t out = Arg(c, 4);
  if (!out) return E_POINTER;
  Wr32(out, 0);
  std::u16string cls = Arg(c, 1) ? GWStr(Arg(c, 1)) : u"";
  if (!EqualsIgnoreCase(cls, "Win32_PNPEntity")) {
    WarnF("wbem: CreateInstanceEnum(%s): class not available\n", win32::Narrow(cls).c_str());
    return WBEM_E_INVALID_CLASS;
  }
  auto e = std::make_shared<InstanceEnum>();
  for (const XInputPad& p : XInputPads()) {
    char id[64];
    std::snprintf(id, sizeof id, "HID\\VID_%04X&PID_%04X&IG_00\\8&%X&0&0000", p.vid,
                  p.pid, p.index + 1);
    e->device_ids.push_back(win32::Widen(id));
  }
  Wr32(out, com::Publish(e, EnumVtable()));
  return S_OK;
}

// ---- IEnumWbemClassObject -----------------------------------------------------

HLE_NAMED("wbemprox.dll", "IEnumWbemClassObject::QueryInterface", Enum_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IEnumWbemClassObject);
}
HLE_NAMED("wbemprox.dll", "IEnumWbemClassObject::AddRef", Enum_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wbemprox.dll", "IEnumWbemClassObject::Release", Enum_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("wbemprox.dll", "IEnumWbemClassObject::Reset", Enum_Reset, 1) {
  Self<InstanceEnum>(c, "IEnumWbemClassObject")->next = 0;
  return S_OK;
}

// (lTimeout, uCount, apObjects, puReturned): WBEM_S_FALSE once fewer than
// uCount are left.
HLE_NAMED("wbemprox.dll", "IEnumWbemClassObject::Next", Enum_Next, 5) {
  InstanceEnum* e = Self<InstanceEnum>(c, "IEnumWbemClassObject");
  uint32_t count = Arg(c, 2), objects = Arg(c, 3), returned = Arg(c, 4);
  if (!objects || !returned) return E_POINTER;
  uint32_t n = 0;
  for (; n < count && e->next < e->device_ids.size(); n++) {
    auto inst = std::make_shared<Instance>();
    inst->device_id = e->device_ids[e->next++];
    Wr32(objects + 4 * n, com::Publish(inst, InstanceVtable()));
  }
  Wr32(returned, n);
  return n == count ? S_OK : WBEM_S_FALSE;
}

// ---- IWbemClassObject ---------------------------------------------------------

HLE_NAMED("wbemprox.dll", "IWbemClassObject::QueryInterface", Obj_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IWbemClassObject);
}
HLE_NAMED("wbemprox.dll", "IWbemClassObject::AddRef", Obj_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wbemprox.dll", "IWbemClassObject::Release", Obj_Release, 1) { return com::Release(Arg(c, 0)); }

// (wszName, lFlags, pVal, pType, plFlavor): DeviceID, as a VT_BSTR VARIANT
// the caller frees (VariantClear).
HLE_NAMED("wbemprox.dll", "IWbemClassObject::Get", Obj_Get, 6) {
  Instance* o = Self<Instance>(c, "IWbemClassObject");
  std::u16string name = Arg(c, 1) ? GWStr(Arg(c, 1)) : u"";
  if (!EqualsIgnoreCase(name, "DeviceID")) {
    WarnF("wbem: Win32_PNPEntity property %s not available\n", win32::Narrow(name).c_str());
    return WBEM_E_NOT_FOUND;
  }
  if (uint32_t v = Arg(c, 3)) {
    std::memset(GPtr(v), 0, 16);
    Wr16(v, VT_BSTR);
    Wr32(v + 8, com::AllocBstr(o->device_id));
  }
  if (Arg(c, 4)) Wr32(Arg(c, 4), CIM_STRING);
  if (Arg(c, 5)) Wr32(Arg(c, 5), 0);
  return S_OK;
}

}  // namespace
}  // namespace recomp::hle

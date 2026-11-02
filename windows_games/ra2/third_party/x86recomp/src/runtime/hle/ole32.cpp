// ole32: class activation and OLE initialisation.
//
// CoCreateInstance looks for the class in this order, as COM does for an
// in-process activation: classes an HLE module provides (com.h), class
// objects the process registered itself (CoRegisterClassObject), then the
// registry's HKCR\CLSID\{clsid}\InprocServer32, whose DLL (a lifted
// module) is loaded and asked for its class factory (DllGetClassObject).
// Guest interfaces are called through their vtables (stdcall, `this`
// first): IUnknown QueryInterface 0, AddRef 1, Release 2; IClassFactory
// CreateInstance 3.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

using namespace win32;

constexpr uint32_t kSFalse = 1, kENoInterface = 0x80004002, kEPointer = 0x80004003,
                   kClassENoAggregation = 0x80040110, kRegdbEClassNotReg = 0x80040154,
                   kCoEClassString = 0x800401F3;

// {xxxxxxxx-0000-0000-C000-000000000046}: the OLE interfaces' GUIDs.
constexpr guest::GUID OleGuid(uint32_t d1) {
  return {d1, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
}
constexpr guest::GUID kIID_IClassFactory = OleGuid(0x00000001);
constexpr guest::GUID kIID_IRunnableObject = OleGuid(0x00000126);

template <typename... A>
uint32_t Method(Ctx& c, gaddr obj, int index, A... args) {
  const gaddr fn = Rd32(Rd32(obj) + 4 * index);
  return uint32_t(CallGuest(c, fn, {uint64_t(obj), uint64_t(args)...}));
}

// A GUID and an out pointer in guest memory for calls host code makes.
struct Scratch {
  explicit Scratch(const guest::GUID& g) : va(HeapAllocProcess(24, true)) {
    std::memcpy(GPtr(va), &g, 16);
  }
  ~Scratch() { HeapFreeProcess(va); }
  gaddr guid() const { return va; }
  gaddr out() const { return va + 16; }
  gaddr va;
};

std::string GuidString(gaddr g) {
  char buf[40];
  const uint8_t* b = GPtr(g + 8);
  std::snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                Rd32(g), Rd16(g + 4), Rd16(g + 6), b[0], b[1], b[2], b[3], b[4], b[5],
                b[6], b[7]);
  return buf;
}

// ---- class objects the process registered -----------------------------------

struct ClassEntry {
  guest::GUID clsid;
  gaddr unk;
  uint32_t cookie;
};
std::mutex g_classes_mu;
std::vector<ClassEntry> g_classes;
uint32_t g_next_cookie = 0x100;

gaddr RegisteredClass(gaddr clsid) {
  std::lock_guard<std::mutex> l(g_classes_mu);
  for (const ClassEntry& e : g_classes)
    if (com::GuidEq(clsid, e.clsid)) return e.unk;
  return 0;
}

// (rclsid, pUnk, dwClsContext, flags, lpdwRegister): the object is kept
// (AddRef) until CoRevokeClassObject.
HLE("ole32.dll", CoRegisterClassObject, 5) {
  const gaddr unk = Arg(c, 1), cookie_out = Arg(c, 4);
  if (!unk || !cookie_out) return kEPointer;
  Method(c, unk, 1);  // AddRef
  std::lock_guard<std::mutex> l(g_classes_mu);
  const uint32_t cookie = g_next_cookie++;
  g_classes.push_back({com::ReadGuid(Arg(c, 0)), unk, cookie});
  Wr32(cookie_out, cookie);
  return com::kS_OK;
}

// (dwRegister)
HLE("ole32.dll", CoRevokeClassObject, 1) {
  gaddr unk = 0;
  {
    std::lock_guard<std::mutex> l(g_classes_mu);
    for (auto it = g_classes.begin(); it != g_classes.end(); ++it)
      if (it->cookie == uint32_t(Arg(c, 0))) {
        unk = it->unk;
        g_classes.erase(it);
        break;
      }
  }
  if (!unk) return 0x80070057;  // E_INVALIDARG
  Method(c, unk, 2);  // Release
  return com::kS_OK;
}

// IClassFactory::CreateInstance on the factory behind `unk` (QueryInterface
// for IClassFactory first, as CoGetClassObject hands out).
uint32_t CreateFromFactory(Ctx& c, gaddr unk, gaddr outer, gaddr riid, gaddr ppv) {
  Scratch s(kIID_IClassFactory);
  Wr32(s.out(), 0);
  uint32_t hr = Method(c, unk, 0, s.guid(), s.out());
  if (hr & 0x80000000) return hr;
  const gaddr factory = Rd32(s.out());
  hr = Method(c, factory, 3, outer, riid, ppv);
  Method(c, factory, 2);  // Release
  return hr;
}

// The factory of an in-process server named in the registry, or 0.
gaddr InprocFactory(Ctx& c, gaddr clsid, uint32_t* hr) {
  std::string dll;
  if (!RegistryString("HKCR\\CLSID\\" + GuidString(clsid) + "\\InprocServer32", "", &dll)) {
    *hr = kRegdbEClassNotReg;
    return 0;
  }
  // The server is found by file name among the modules this process has.
  const size_t slash = dll.find_last_of("\\/");
  if (slash != std::string::npos) dll.erase(0, slash + 1);
  const gaddr module = LoadLibraryFor(c, "CoCreateInstance", dll);
  const gaddr get = module ? ModuleExport(module, "DllGetClassObject") : 0;
  if (!get) {
    WarnF("CoCreateInstance(%s): in-process server %s not available\n",
          GuidString(clsid).c_str(), dll.c_str());
    *hr = kRegdbEClassNotReg;
    return 0;
  }
  Scratch s(kIID_IClassFactory);
  Wr32(s.out(), 0);
  *hr = uint32_t(CallGuest(c, get, {clsid, s.guid(), s.out()}));
  return (*hr & 0x80000000) ? 0 : Rd32(s.out());
}

// (rclsid, pUnkOuter, dwClsContext, riid, ppv). Classes no one provides
// fail with REGDB_E_CLASSNOTREG, as on a system without them.
HLE("ole32.dll", CoCreateInstance, 5) {
  const gaddr clsid = Arg(c, 0), outer = Arg(c, 1), riid = Arg(c, 3), out = Arg(c, 4);
  if (!out) return kEPointer;
  Wr32(out, 0);
  if (com::CreateFn create = com::FindCoClass(clsid)) {
    if (outer) return kClassENoAggregation;
    uint32_t obj = 0;
    uint32_t hr = create(riid, &obj);
    Wr32(out, obj);
    return hr;
  }
  if (gaddr unk = RegisteredClass(clsid)) return CreateFromFactory(c, unk, outer, riid, out);
  uint32_t hr = 0;
  const gaddr factory = InprocFactory(c, clsid, &hr);
  if (!factory) {
    if (hr == kRegdbEClassNotReg)
      WarnF("CoCreateInstance(%s): class not available\n", GuidString(clsid).c_str());
    return hr;
  }
  hr = Method(c, factory, 3, outer, riid, out);
  Method(c, factory, 2);  // Release
  return hr;
}

// ---- OLE initialisation ------------------------------------------------------

thread_local uint32_t t_ole_inits = 0;

// (pvReserved): S_OK the first time on a thread, S_FALSE after.
HLE("ole32.dll", OleInitialize, 1) { return t_ole_inits++ ? kSFalse : com::kS_OK; }
HLE("ole32.dll", OleUninitialize, 0) {
  if (t_ole_inits) t_ole_inits--;
  return 0;
}

// (pUnk): an object of this process has no remote connections to break.
HLE("ole32.dll", CoDisconnectObject, 2) { return com::kS_OK; }

// (lpFileTime): the current time as a FILETIME (UTC).
HLE("ole32.dll", CoFileTimeNow, 1) {
  using namespace std::chrono;
  const uint64_t t =
      kFileTimeUnixEpoch +
      uint64_t(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()) * 10;
  Wr32(Arg(c, 0), uint32_t(t));
  Wr32(Arg(c, 0) + 4, uint32_t(t >> 32));
  return com::kS_OK;
}

// (pUnknown): runs an object that implements IRunnableObject; any other
// object is already running.
HLE("ole32.dll", OleRun, 1) {
  const gaddr unk = Arg(c, 0);
  if (!unk) return kEPointer;
  Scratch s(kIID_IRunnableObject);
  Wr32(s.out(), 0);
  if (Method(c, unk, 0, s.guid(), s.out()) & 0x80000000) return com::kS_OK;
  const gaddr runnable = Rd32(s.out());
  const uint32_t hr = Method(c, runnable, 4, 0u);  // IRunnableObject::Run(NULL)
  Method(c, runnable, 2);
  return hr;
}

// ---- CLSID strings -------------------------------------------------------------

// (lpsz, pclsid): "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" only (no ProgID
// lookup); anything else is CO_E_CLASSSTRING.
HLE("ole32.dll", CLSIDFromString, 2) {
  const std::string s = Narrow(GWStr(Arg(c, 0)));
  unsigned d1, d2, d3, b[8];
  char close = 0;
  if (s.size() != 38 ||
      std::sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x%c", &d1, &d2, &d3, &b[0],
                  &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &close) != 12 ||
      close != '}')
    return kCoEClassString;
  const gaddr g = Arg(c, 1);
  Wr32(g, d1);
  Wr16(g + 4, uint16_t(d2));
  Wr16(g + 6, uint16_t(d3));
  for (int i = 0; i < 8; i++) Wr8(g + 8 + i, uint8_t(b[i]));
  return com::kS_OK;
}

// (rclsid, lplpsz): the string in task-allocator memory (CoTaskMemFree).
HLE("ole32.dll", StringFromCLSID, 2) {
  const std::string s = GuidString(Arg(c, 0));
  const gaddr p = HeapAllocProcess(2 * (s.size() + 1), false);
  if (!p) return 0x8007000E;  // E_OUTOFMEMORY
  PutWStr(p, s);
  Wr32(Arg(c, 1), p);
  return com::kS_OK;
}

// ---- objects saved in streams -------------------------------------------------
//
// IPersistStream: GetClassID 3, IsDirty 4, Load 5, Save 6; a stream's
// Read 3 and Write 4 (ISequentialStream). Both work through the
// interfaces, so the stream may be any IStream (one of the game's own).

constexpr guest::GUID kIID_IPersistStream = OleGuid(0x00000109);
constexpr uint32_t kStgEReadFault = 0x8003001E, kStgEMediumFull = 0x80030070;
constexpr uint32_t kClsctxInproc = 0x3;  // CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER

bool Failed(uint32_t hr) { return hr & 0x80000000; }

// (pPStm, pStm): the object's CLSID (WriteClassStm), then the object
// (IPersistStream::Save with fClearDirty). No object: CLSID_NULL, S_FALSE.
HLE("ole32.dll", OleSaveToStream, 2) {
  const gaddr obj = Arg(c, 0), stm = Arg(c, 1);
  if (!stm) return kEPointer;
  Scratch s(guest::GUID{});
  uint32_t hr;
  if (obj && Failed(hr = Method(c, obj, 3, s.guid()))) return hr;
  if (Failed(hr = Method(c, stm, 4, s.guid(), 16u, s.out()))) return hr;
  if (Rd32(s.out()) != 16) return kStgEMediumFull;
  if (!obj) return kSFalse;
  return Method(c, obj, 6, stm, 1u);
}

// (pStm, iidInterface, ppvObj): the CLSID (ReadClassStm), a new object of
// that class (CoCreateInstance), loaded from the rest of the stream
// (IPersistStream::Load).
HLE("ole32.dll", OleLoadFromStream, 3) {
  const gaddr stm = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  if (!out) return kEPointer;
  Wr32(out, 0);
  if (!stm || !iid) return kEPointer;
  Scratch clsid(guest::GUID{});
  uint32_t hr;
  if (Failed(hr = Method(c, stm, 3, clsid.guid(), 16u, clsid.out()))) return hr;
  if (Rd32(clsid.out()) != 16) return kStgEReadFault;
  hr = uint32_t(CallGuest(c, ThunkFor("ole32.dll", "CoCreateInstance"),
                          {clsid.guid(), 0, kClsctxInproc, iid, out}));
  if (Failed(hr)) return hr;
  const gaddr obj = Rd32(out);
  Scratch ps(kIID_IPersistStream);
  if (Failed(hr = Method(c, obj, 0, ps.guid(), ps.out()))) {
    Method(c, obj, 2);
    Wr32(out, 0);
    return hr;
  }
  const gaddr persist = Rd32(ps.out());
  hr = Method(c, persist, 5, stm);
  Method(c, persist, 2);
  if (Failed(hr)) {
    Method(c, obj, 2);
    Wr32(out, 0);
    return hr;
  }
  return com::kS_OK;
}

// ---- oleaut32 (imported by ordinal) -----------------------------------------

// #161 LoadTypeLib(szFile, pptlib): type libraries are not implemented, so
// none can be loaded (TYPE_E_CANTLOADLIBRARY), as on a system without OLE
// Automation's type library support.
HLE_NAMED("oleaut32.dll", "#161", LoadTypeLib, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return 0x80029C4A;
}

// VARIANT: vt (2 bytes), 6 reserved, then 8 bytes of value. VT_EMPTY 0,
// VT_BSTR 8, VT_DISPATCH 9, VT_UNKNOWN 13; VT_BYREF 0x4000 owns nothing.
constexpr uint16_t kVtBstr = 8, kVtDispatch = 9, kVtUnknown = 13, kVtByref = 0x4000;

HLE_NAMED("oleaut32.dll", "#8", VariantInit, 1) {
  Wr16(Arg(c, 0), 0);
  return 0;
}

HLE_NAMED("oleaut32.dll", "#9", VariantClear, 1) {
  const gaddr v = Arg(c, 0);
  const uint16_t vt = Rd16(v);
  if (!(vt & kVtByref)) {
    if (vt == kVtBstr) {
      com::FreeBstr(Rd32(v + 8));
    } else if ((vt == kVtDispatch || vt == kVtUnknown) && Rd32(v + 8)) {
      Method(c, Rd32(v + 8), 2);  // Release
    } else if (vt > 23 && vt != 0x15 && vt != 0x13) {  // arrays, records, ...
      WarnF("VariantClear: VARTYPE %u not supported\n", vt);
      return 0x80020008;  // DISP_E_BADVARTYPE
    }
  }
  Wr16(v, 0);
  return com::kS_OK;
}

// The running object table: RegisterActiveObject(punk, rclsid, dwFlags,
// pdwRegister) keeps a reference until RevokeActiveObject(dwRegister,
// pvReserved).
std::mutex g_active_mu;
std::vector<std::pair<uint32_t, gaddr>> g_active;
uint32_t g_next_active = 0x200;

HLE_NAMED("oleaut32.dll", "#33", RegisterActiveObject, 4) {
  const gaddr unk = Arg(c, 0), out = Arg(c, 3);
  if (!unk || !out) return 0x80070057;  // E_INVALIDARG
  Method(c, unk, 1);  // AddRef
  std::lock_guard<std::mutex> l(g_active_mu);
  const uint32_t cookie = g_next_active++;
  g_active.push_back({cookie, unk});
  Wr32(out, cookie);
  return com::kS_OK;
}

HLE_NAMED("oleaut32.dll", "#34", RevokeActiveObject, 2) {
  gaddr unk = 0;
  {
    std::lock_guard<std::mutex> l(g_active_mu);
    for (auto it = g_active.begin(); it != g_active.end(); ++it)
      if (it->first == uint32_t(Arg(c, 0))) {
        unk = it->second;
        g_active.erase(it);
        break;
      }
  }
  if (!unk) return 0x80070057;
  Method(c, unk, 2);  // Release
  return com::kS_OK;
}

// The thread's error object: SetErrorInfo(dwReserved, perrinfo) replaces it
// (AddRef the new one, Release the old); GetErrorInfo(dwReserved, pperrinfo)
// hands it over and clears it, or S_FALSE with NULL when there is none.
thread_local gaddr t_error_info = 0;

HLE_NAMED("oleaut32.dll", "#201", SetErrorInfo, 2) {
  const gaddr info = Arg(c, 1);
  if (info) Method(c, info, 1);
  if (t_error_info) Method(c, t_error_info, 2);
  t_error_info = info;
  return com::kS_OK;
}

HLE_NAMED("oleaut32.dll", "#200", GetErrorInfo, 2) {
  const gaddr out = Arg(c, 1);
  if (!out) return kEPointer;
  Wr32(out, t_error_info);
  const bool had = t_error_info != 0;
  t_error_info = 0;
  return had ? com::kS_OK : kSFalse;
}

}  // namespace
}  // namespace recomp::hle

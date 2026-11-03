// comctl32: the version a game checks for, and common-control setup. The
// controls themselves (list views, image lists, ...) are not provided.

#include "runtime/hle/hle.h"

namespace recomp::hle {
namespace {

// #17 InitCommonControls(): registers the control window classes; nothing
// to do while no control class is provided.
HLE_NAMED("comctl32.dll", "#17", InitCommonControls, 0) { return 0; }
HLE_NAMED("comctl32.dll", "InitCommonControls", InitCommonControlsByName, 0) { return 0; }

// DllGetVersion(DLLVERSIONINFO*): cbSize, dwMajorVersion, dwMinorVersion,
// dwBuildNumber, dwPlatformID. Version 5.81 (build 4704), as shipped with
// Windows 2000, on the NT platform (DLLVER_PLATFORM_NT 2).
HLE("comctl32.dll", DllGetVersion, 1) {
  const gaddr info = Arg(c, 0);
  if (!info || Rd32(info) < 20) return 0x80070057;  // E_INVALIDARG
  Wr32(info + 4, 5);
  Wr32(info + 8, 81);
  Wr32(info + 12, 4704);
  Wr32(info + 16, 2);
  return 0;
}

}  // namespace
}  // namespace recomp::hle

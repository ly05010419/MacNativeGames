#pragma once
// The XInput pads (xinput.cpp) as other APIs list them: DirectInput's game
// controllers (dinput8.cpp) and WMI's PnP devices (wbem.cpp), which games
// match up to find which DirectInput devices are XInput ones.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::hle {

struct XInputPad {
  uint32_t index;     // XInput user index
  uint16_t vid, pid;  // USB vendor and product id
  std::string name;
};

// The connected pads in user index order. Refreshes the pad list on the
// main thread (elsewhere: the pads already open).
std::vector<XInputPad> XInputPads();

}  // namespace recomp::hle

// Development aid: RA2_DUMP_DIR=<dir> starts a thread that, whenever a
// file named "req" appears in <dir>, writes the game's .data section to
// <dir>/<label>.bin (label = the file's first line) and deletes the
// request. Diffing dumps taken around an action finds the variables it
// changes (reverse engineering for hooks; never shipped behaviour).

#include "devtools.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "addresses.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {


void DumpLoop(std::string dir) {
  const std::string req = dir + "/req";
  const std::string poke = dir + "/poke";
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // "poke" holding "dx dy": moves the tactical position by that much.
    if (std::ifstream pk{poke}) {
      int dx = 0, dy = 0;
      pk >> dx >> dy;
      pk.close();
      std::remove(poke.c_str());
      if (const uint32_t tac = recomp::Rd32(Addresses().tactical_ptr)) {
        recomp::Wr32(tac + 0xB0, recomp::Rd32(tac + 0xB0) + uint32_t(dx));
        recomp::Wr32(tac + 0xB4, recomp::Rd32(tac + 0xB4) + uint32_t(dy));
        std::fprintf(stderr, "ra2: poked tactical position by %d,%d\n", dx, dy);
      }
    }
    std::ifstream r(req);
    if (!r) continue;
    std::string label;
    std::getline(r, label);
    r.close();
    std::remove(req.c_str());
    if (label.empty()) label = "dump";
    std::ofstream out(dir + "/" + label + ".bin", std::ios::binary);
    const GameAddresses& a = Addresses();
    out.write(reinterpret_cast<const char*>(recomp::GPtr(a.data_va)), a.data_size);
    // The tactical view object (the pointer at Addresses().tactical_ptr), 4 KB of it.
    if (const uint32_t tac = recomp::Rd32(Addresses().tactical_ptr)) {
      std::ofstream t(dir + "/" + label + "_tac.bin", std::ios::binary);
      t.write(reinterpret_cast<const char*>(recomp::GPtr(tac)), 0x1000);
    }
    std::fprintf(stderr, "ra2: dumped .data to %s/%s.bin\n", dir.c_str(), label.c_str());
  }
}

}  // namespace

void StartDevTools() {
  const char* dir = std::getenv("RA2_DUMP_DIR");
  if (!dir || !*dir) return;
  std::thread(DumpLoop, std::string(dir)).detach();
}

}  // namespace ra2

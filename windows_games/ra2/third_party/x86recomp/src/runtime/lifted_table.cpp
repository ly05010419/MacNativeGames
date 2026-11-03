// Registers the lifter's function table (generated/table.cpp) with the
// runtime before main(). Compiled into each game's lifted code by
// recomp_add_lifted (CMakeLists.txt), not into the runtime library.

#include "runtime/lifted.h"

namespace recomp {
namespace {
const bool kRegistered =
    (RegisterLiftedFunctions(kLiftedFunctions, kLiftedFunctionCount), true);
}  // namespace
}  // namespace recomp

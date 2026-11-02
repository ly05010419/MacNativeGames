#pragma once
// Shared between the UCRT's HLE files (ucrt.cpp, ucrt_stdio.cpp).

#include "runtime/guest_memory.h"

namespace recomp::hle {

// The calling guest thread's errno (an int in guest memory: _errno()).
gaddr GuestErrno();

}  // namespace recomp::hle

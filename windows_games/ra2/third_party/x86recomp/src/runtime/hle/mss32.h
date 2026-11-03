#pragma once
// Miles state shared with the Bink HLE (Bink plays its audio through
// Miles' digital driver, like BinkSoundUseMiles on Windows).

namespace recomp::hle {

// True while AIL_open_digital_driver's driver is open (the mixer runs).
bool MilesDriverOpen();

}  // namespace recomp::hle

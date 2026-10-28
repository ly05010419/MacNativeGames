// Legacy mode values kept for the copied backend ABI. Odyssey maps nonzero requests
// to MFX_HALF (60+60 on a 120 Hz screen); key 2 toggles off/on. Guest logic stays 60 Hz.
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_NMODES };
int mfx_enabled(void);   // the mode of this game frame (0 = off)

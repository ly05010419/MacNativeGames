// Legacy mode values kept for the copied backend ABI. Odyssey maps nonzero requests
// to MFX_HALF (60+60 on a 120 Hz screen); the settings panel selects off/on. Guest logic stays 60 Hz.
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_NMODES };
int mfx_enabled(void);   // the mode of this game frame (0 = off)
int mfx_requested(void); // the panel's request, including when a display cannot generate frames

void mfx_set(int mode); // request; applied at the next game-frame boundary

// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
// Animal Crossing: TOTK_MFX and "flag mfx_interp" pick a mode (diagnostics); the settings panel shows interpolation as not
// available, because its scene / depth / camera / UI hooks do not find this game's passes (runtime/settings_panel.h).
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_NMODES };
int mfx_enabled(void);   // the mode of this game frame (0 = off)
int mfx_requested(void);   // the mode picked (the settings panel), taken over at the next game frame

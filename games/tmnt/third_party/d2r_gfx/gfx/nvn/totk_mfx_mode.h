// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
// Key 2 cycles off -> 30+30 -> half the refresh rate (50+50 at 100 Hz) -> uncapped -> off (user, 2026-10-16 night).
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_NMODES };
int mfx_enabled(void);   // the mode of this game frame (0 = off)

// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C), the pacing in hle_nvn.c (C) and the
// shortcut panel (runtime/frontend.m).
// Key 2 cycles off -> half -> full -> off (user, 2026-11-03). The game itself always runs at the key 8 frame rate
// (30 / 60 / 90 / 120); interpolation only adds pictures on top of it: half = one generated frame for every second game
// frame (60 -> "60+30"), full = one for every game frame (60 -> "60+60").
// Generation is independent of screen refresh; actual presentation remains refresh-limited.
#pragma once
enum { MFX_OFF, MFX_HALF, MFX_FULL, MFX_NMODES };
int mfx_enabled(void);       // the mode of this game frame (0 = off)

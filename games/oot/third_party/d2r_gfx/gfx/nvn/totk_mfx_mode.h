// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
// The settings panel picks off, 30+30, 40+40, 50+50 or uncapped (user 2026-10-25; before: 30+30, half the refresh rate,
// uncapped; 60+60 added for 120 Hz displays, 2026-10-31). 30+30 / 40+40 / 50+50 / 60+60: the game held at that rate (hle_nvn.c game_hold_fps), MetalFX makes the frame between.
#pragma once
enum { MFX_OFF, MFX_30, MFX_40, MFX_50, MFX_MAX, MFX_60, MFX_NMODES };   // MFX_60 after MFX_MAX: saved interp-<n> words keep their meaning
static inline int mfx_hold_fps(int m) { return m == MFX_30 ? 30 : m == MFX_40 ? 40 : m == MFX_50 ? 50 : m == MFX_60 ? 60 : 0; }   // the game's rate, 0 = not held
int mfx_enabled(void);   // the mode of this game frame (0 = off)
int mfx_requested(void);   // the mode the player picked (the settings panel), taken over at the next game frame

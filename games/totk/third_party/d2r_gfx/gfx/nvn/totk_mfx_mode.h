// TOTK: MetalFX frame interpolation modes, shared by totk_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
// The settings panel picks off, 30+30, 40+40, 50+50, half the refresh rate or uncapped (user 2026-10-25 left the
// half-rate entry out; it is back before uncapped, 2026-10-28). 30+30 / 40+40 / 50+50: the game held at that rate
// (hle_nvn.c game_hold_fps), MetalFX makes the frame between.
// MFX_HALF: the game held at half the display's rate (60 at 120 Hz, 50 at 100 Hz), so one generated frame per game
// frame and the screen shows one picture per refresh. It is the entry the panel had before 2026-10-25, and the only
// one whose rate always divides the display's: 40+40 and 50+50 do not at 120 Hz (80 and 100 pictures a second), so
// they judder there while 60+60 does not.
// MFX_HALF comes last on purpose: `flag mfx_interp 4` keeps meaning uncapped, as it did before this entry existed (the
// panel lists it before uncapped all the same: its order is its own).
#pragma once
enum { MFX_OFF, MFX_30, MFX_40, MFX_50, MFX_MAX, MFX_HALF, MFX_NMODES };
static inline int mfx_hold_fps(int m) { return m == MFX_30 ? 30 : m == MFX_40 ? 40 : m == MFX_50 ? 50 : 0; }   // the game's rate, 0 = not held (MFX_HALF: half the display's, hle_nvn.c game_hold_fps)
static inline int mfx_half(int m) { return m == MFX_HALF; }   // the game follows the display at half its rate
int mfx_enabled(void);   // the mode of this game frame (0 = off)
int mfx_requested(void);   // the mode the player picked (the settings panel), taken over at the next game frame

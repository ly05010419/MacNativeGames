#pragma once
/* Shared IDs for Odyssey's runtime picture settings and its native panel. */
enum { GX_AA, GX_CAS, GX_UP, GX_TEX, GX_RES, GX_FRAME, GX_N };
enum { GX_AA_OFF, GX_AA_FXAA, GX_AA_SMAA, GX_AA_TAA };
enum { GX_UP_METALFX, GX_UP_FSR1, GX_UP_LANCZOS, GX_UP_BILINEAR };
int dm_gfx_opt_get(int option);
void dm_gfx_opt_set(int option, int value);

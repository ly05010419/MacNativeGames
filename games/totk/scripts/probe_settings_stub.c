/* Test-only default keyboard map for the input/autoload probes.
 *
 * The real runtime resolves these through runtime/settings.m and the active
 * keymap profile.  The probes deliberately do not link the settings store
 * (Foundation, persistence and adapter callbacks), so they take the documented
 * built-in map instead.  Keep this in sync with the default profile in
 * runtime/settings.m; probe-input protects the physical-key behaviour, not the
 * profile UI. */
#include "../runtime/settings.h"

uint64_t totk_key_button(unsigned code) {
    switch (code) {
    case 38: return 1u << 0;           /* J -> A */
    case 40: case 49: return 1u << 1;  /* K / Space -> B */
    case 34: return 1u << 2;           /* I -> X */
    case 32: return 1u << 3;           /* U -> Y */
    case 12: return 1u << 4;           /* Q -> L */
    case 14: return 1u << 5;           /* E -> R */
    case 58: return 1u << 6;           /* Option -> ZL */
    case 56: return 1u << 7;           /* Shift -> ZR */
    case 36: return 1u << 8;           /* Return -> + */
    case 48: case 53: return 1u << 9;  /* Tab / Esc -> - */
    case 6: return 1u << 10;           /* Z -> left stick click */
    case 8: return 1u << 11;           /* C -> right stick click */
    case 3: return 1u << 12;           /* F -> d-pad left */
    case 17: return 1u << 13;          /* T -> d-pad up */
    case 4: return 1u << 14;           /* H -> d-pad right */
    case 5: return 1u << 15;           /* G -> d-pad down */
    default: return 0;
    }
}

int totk_key_axis(unsigned code, int *axis, int *sign) {
    switch (code) {
    case 0:   *axis = 0; *sign = -1; return 1; /* A -> left stick left */
    case 2:   *axis = 0; *sign =  1; return 1; /* D -> left stick right */
    case 13:  *axis = 1; *sign =  1; return 1; /* W -> left stick up */
    case 1:   *axis = 1; *sign = -1; return 1; /* S -> left stick down */
    case 123: *axis = 2; *sign = -1; return 1; /* Left -> right stick left */
    case 124: *axis = 2; *sign =  1; return 1; /* Right -> right stick right */
    case 126: *axis = 3; *sign =  1; return 1; /* Up -> right stick up */
    case 125: *axis = 3; *sign = -1; return 1; /* Down -> right stick down */
    default: return 0;
    }
}

int totk_profile_active(void) { return -1; }
const char *totk_profile_name(int index) { (void)index; return NULL; }

/* The in-game shortcut keys (2 frame interpolation, 3 picture mode, 4 ultrawide) now go through the settings store
 * instead of calling the adapters directly. The probes link this file instead of runtime/settings.m, so keep just
 * enough live state to observe one action per press. The real store also persists and applies through the adapters. */
static int probe_interp, probe_picture, probe_aspect;
int probe_interp_actions, probe_picture_actions, probe_aspect_actions;

int totk_setting_interp(void) { return probe_interp; }
int totk_setting_set_interp(int mode) {
    if (mode < TOTK_INTERP_OFF || mode > TOTK_INTERP_MAX) return TOTK_SET_ERR_INVALID;
    probe_interp = mode;
    probe_interp_actions++;
    return TOTK_SET_OK;
}

int totk_setting_picture(void) { return probe_picture; }
int totk_setting_set_picture(int mode) {
    if (mode < TOTK_PICTURE_OFF || mode > TOTK_PICTURE_NATIVE) return TOTK_SET_ERR_INVALID;
    probe_picture = mode;
    probe_picture_actions++;
    return TOTK_SET_OK;
}

int totk_setting_aspect(void) { return probe_aspect; }
int totk_setting_set_aspect(int on) { probe_aspect = on ? 1 : 0; probe_aspect_actions++; return TOTK_SET_OK; }

/* Rumble and gyro settings a player can keep (Mac): one-word files next to controls.txt, like controller-type.txt and
   mouse.txt (runtime/gamepad.m runtime_setting_word), so they can be set without the environment of a launch:
     rumble.txt       on | off    the same as TOTK_RUMBLE (runtime/hid_feedback.c)
     gyro.txt         on | off    the same as TOTK_GYRO
     rumble-gain.txt  a number in (0, 4], the same as TOTK_RUMBLE_GAIN (runtime/gamepad_feedback.m)
   The environment still decides first, and an agent's test session (TOTK_SLOT_OWNER claude / codex) still leaves rumble
   and gyro off unless the environment turns them on: the player's rumble.txt and gyro.txt do not decide for a test.
   rumble-gain.txt sets the strength whenever rumble is on, also in a test whose environment turned it on. A word the
   runtime does not know keeps the default and is logged, as mouse.txt does: a file never stops the game.
   Plain C, shared by the runtime and tests/test_feedback_settings.c. */
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { FEEDBACK_BAD = -1 };

/* The word in text without the blanks and newline around it: *start, length returned. */
static inline size_t feedback_word(const char *text, const char **start) {
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) n--;
    *start = text;
    return n;
}

/* "on" 1, "off" 0, anything else FEEDBACK_BAD (lower case, as setting_write saves them). */
static inline int feedback_word_on(const char *text) {
    if (!text) return FEEDBACK_BAD;
    const char *w; size_t n = feedback_word(text, &w);
    if (n == 2 && !strncmp(w, "on", 2)) return 1;
    if (n == 3 && !strncmp(w, "off", 3)) return 0;
    return FEEDBACK_BAD;
}

/* What decided feedback_switch, for the log. */
enum { FEEDBACK_FROM_DEFAULT, FEEDBACK_FROM_ENV, FEEDBACK_FROM_AGENT, FEEDBACK_FROM_FILE, FEEDBACK_FROM_BAD_FILE };

/* Rumble or gyro on (1) or off (0). env is TOTK_RUMBLE / TOTK_GYRO ("0" off, anything else on, as before), owner
   TOTK_SLOT_OWNER, file the text of rumble.txt / gyro.txt or NULL when there is none; *from says which of them decided
   (FEEDBACK_FROM_BAD_FILE: the file says something other than on or off, which counts as on). */
static inline int feedback_switch(const char *env, const char *owner, const char *file, int *from) {
    if (env) { *from = FEEDBACK_FROM_ENV; return strcmp(env, "0") != 0; }
    if (owner && (!strcmp(owner, "claude") || !strcmp(owner, "codex"))) { *from = FEEDBACK_FROM_AGENT; return 0; }
    if (!file) { *from = FEEDBACK_FROM_DEFAULT; return 1; }
    int on = feedback_word_on(file);
    *from = on == FEEDBACK_BAD ? FEEDBACK_FROM_BAD_FILE : FEEDBACK_FROM_FILE;
    return on != 0;
}

/* rumble-gain.txt: 1 with *gain set when text is one number in (0, 4], else 0 and *gain untouched. */
static inline int feedback_gain_word(const char *text, float *gain) {
    if (!text) return 0;
    const char *w; size_t n = feedback_word(text, &w);
    char word[32];
    if (!n || n >= sizeof word) return 0;
    memcpy(word, w, n); word[n] = 0;
    char *end; double v = strtod(word, &end);
    if (end != word + n || !isfinite(v) || v > 4 || !((float)v > 0)) return 0;   /* 1e-50 is 0 as a float: no rumble */
    *gain = (float)v;
    return 1;
}

/* The HDR / EDR choice (Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 on the Mac; PR #1129's engine, frontend.m front_hdr_*): off, on or
   auto. Auto is the switch on while the window's screen can show more than SDR white (its NSScreen
   maximumPotentialExtendedDynamicRangeColorComponentValue above 1: a MacBook's XDR screen gives 16, an SDR monitor 1)
   and off everywhere else, so a window moved to an SDR monitor shows the SDR picture as without HDR, costs nothing
   extra and may use frame interpolation. Plain C: tests/test_hdr_auto.c checks it without a window. */
#pragma once
#include <math.h>
#include <string.h>
enum { HDR_CHOICE_OFF, HDR_CHOICE_ON, HDR_CHOICE_AUTO };
/* The word hdr.txt holds for a choice. */
static inline const char *hdr_choice_word(int choice) {
    static const char *const words[] = {"off", "on", "auto"};
    return words[choice >= HDR_CHOICE_OFF && choice <= HDR_CHOICE_AUTO ? choice : HDR_CHOICE_OFF];
}
/* TOTK_HDR, set and not empty, wins: "1" on, "auto" auto, anything else off. Otherwise hdr.txt's first word (NULL or
   empty: no file): "on" on, "auto" auto, anything else off. */
static inline int hdr_choice_parse(const char *env, const char *word) {
    if (env && *env) return !strcmp(env, "1") ? HDR_CHOICE_ON : !strcmp(env, "auto") ? HDR_CHOICE_AUTO : HDR_CHOICE_OFF;
    if (!word) return HDR_CHOICE_OFF;
    return !strcmp(word, "on") ? HDR_CHOICE_ON : !strcmp(word, "auto") ? HDR_CHOICE_AUTO : HDR_CHOICE_OFF;
}
/* What the choice means on a screen whose potential EDR value is potential: the switch, front_hdr_enabled(). */
static inline int hdr_choice_effective(int choice, double potential) {
    return choice == HDR_CHOICE_ON || (choice == HDR_CHOICE_AUTO && isfinite(potential) && potential > 1.0);
}

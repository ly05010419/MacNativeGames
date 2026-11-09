// The stats pane's colour (front_settings.m DmPerformanceHUD), after Tears of the Kingdom's shader line: yellow for three seconds
// after an on-demand Metal library load or pipeline build attempt, so a stutter can be matched with a compile; orange while any
// load or pipeline error has been counted, whatever the activity. Pure: the caller passes monotonic seconds, tests their own.
#pragma once
#include <stdint.h>
enum { DM_ACTIVITY_GREEN, DM_ACTIVITY_YELLOW, DM_ACTIVITY_ORANGE };
#define DM_ACTIVITY_HOLD_S 3.0
typedef struct { uint64_t loads, builds; double rise_s; int primed, risen; } DmShaderActivity;
// The pane was hidden: its next sample is a new starting point, so counters that grew meanwhile do not light it.
static inline void dm_activity_reset(DmShaderActivity *a) { a->primed = a->risen = 0; }
// loads / builds: the cumulative on-demand counters (AocShaderStats library_loads / pipeline_builds); errors: load + pipeline errors.
// The first sample after a reset, or one where a counter went down, only sets the starting point.
static inline int dm_activity_step(DmShaderActivity *a, uint64_t loads, uint64_t builds, uint64_t errors, double now_s) {
    if (!a->primed || loads < a->loads || builds < a->builds) { a->primed = 1; a->risen = 0; }
    else if (loads > a->loads || builds > a->builds) { a->risen = 1; a->rise_s = now_s; }
    a->loads = loads; a->builds = builds;
    if (errors) return DM_ACTIVITY_ORANGE;
    return a->risen && now_s - a->rise_s < DM_ACTIVITY_HOLD_S ? DM_ACTIVITY_YELLOW : DM_ACTIVITY_GREEN;
}

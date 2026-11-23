// Odyssey (22 October 2026): os_signpost markers for stall attribution, subsystem "org.projectidea.odyssey".
// Interval names: shader-translate, shader-library, pipeline-render, pipeline-compute, texture-upload. Events: present,
// long-frame. Verified 22 Oct (see TECHNIQUES.md "Stall markers"):
//   unset / 0  off: one check per marker (~50 ns), nothing written to the log store.
//   1          on, category PointsOfInterest: ~1 us per interval + event, about 360 entries/s persisted to the unified log
//              in Cap Kingdom. Seen by Instruments (Game Performance, attach or launch) and by
//              `/usr/bin/log stream --signpost --predicate 'subsystem == "org.projectidea.odyssey"'`.
//   The DynamicTracing category was tried as a zero-cost default: neither xctrace attach nor launch enabled it.
//   xctrace: /Applications/Xcode.app/Contents/Developer/usr/bin/xctrace record --template "Game Performance" --attach <pid>
//            --time-limit 20s --output t.trace
#pragma once
#include <os/log.h>
#include <os/signpost.h>
#include <stdlib.h>
#include <string.h>
#include <dispatch/dispatch.h>

static inline os_log_t dm_sp_log(void) {
    static os_log_t log; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("DM_SIGNPOST");
        log = e && !strcmp(e, "1") ? os_log_create("org.projectidea.odyssey", OS_LOG_CATEGORY_POINTS_OF_INTEREST) : OS_LOG_DISABLED;
    });
    return log;
}
// Begin / end one interval in the same scope. name must be a string literal (os_signpost requirement).
#define DM_SP_BEGIN(var, name, ...) os_signpost_id_t var = OS_SIGNPOST_ID_NULL; \
    if (os_signpost_enabled(dm_sp_log())) { var = os_signpost_id_generate(dm_sp_log()); os_signpost_interval_begin(dm_sp_log(), var, name, __VA_ARGS__); }
#define DM_SP_END(var, name) do { if (var != OS_SIGNPOST_ID_NULL) os_signpost_interval_end(dm_sp_log(), var, name); } while (0)
#define DM_SP_EVENT(name, ...) do { if (os_signpost_enabled(dm_sp_log())) os_signpost_event_emit(dm_sp_log(), OS_SIGNPOST_ID_EXCLUSIVE, name, __VA_ARGS__); } while (0)

/* No game or platform APIs: drive the same acknowledgement-based coordinator
   used by the Mac frontend while backend operations remain in flight. */
#include <assert.h>
#include <stdio.h>
#include "../games/totk/runtime/pause_state.h"

static TotkPauseState supported(void) {
    return (TotkPauseState){.supported = 1};
}
static void stack_reasons(void) {
    TotkPauseState s = supported();
    assert(totk_pause_set_reason(&s, TOTK_PAUSE_MANUAL, 1));
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
    assert(s.phase == TOTK_PAUSE_PAUSING);   /* request is not an acknowledgement */
    assert(totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 1));
    assert(totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 1));
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    assert(totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, 0) == 0);
    assert(s.phase == TOTK_PAUSE_PAUSED);
    assert(totk_pause_toggle_manual(&s));   /* close settings and resume command */
    assert(s.reasons == TOTK_PAUSE_BACKGROUND);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    assert(totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 0));
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_RESUME);
    assert(totk_pause_complete(&s, TOTK_PAUSE_DO_RESUME, 0) == 0);
    assert(s.phase == TOTK_PAUSE_RUNNING && !s.reasons);
}
static void rollback_failure(void) {
    TotkPauseState s = supported();
    totk_pause_set_reason(&s, TOTK_PAUSE_MANUAL, 1);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 1);
    totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 1);
    assert(totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, -1) == -1);
    assert(s.phase == TOTK_PAUSE_RUNNING && s.reasons == 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);   /* no automatic retries */
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    assert(totk_pause_toggle_manual(&s));   /* a deliberate new request may retry */
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
}
static void release_only_owner(void) {
    TotkPauseState s = supported();
    totk_pause_set_reason(&s, TOTK_PAUSE_MANUAL, 1);
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 1);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
    totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, 0);
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 0);
    assert(s.reasons == TOTK_PAUSE_MANUAL);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 1);
    totk_pause_set_reason(&s, TOTK_PAUSE_MANUAL, 0);
    assert(s.reasons == TOTK_PAUSE_BACKGROUND);
    assert(totk_pause_toggle_manual(&s));
    assert(s.reasons == TOTK_PAUSE_BACKGROUND);   /* Resume cannot add a sticky manual bit */
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_RESUME);
}
static void rapid_requests(void) {
    TotkPauseState s = supported();
    assert(totk_pause_toggle_manual(&s));
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
    assert(totk_pause_toggle_manual(&s));
    assert(!s.reasons && s.phase == TOTK_PAUSE_PAUSING);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);   /* do not overlap operations */
    totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_RESUME);
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 1);   /* reopen during resume */
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
    totk_pause_complete(&s, TOTK_PAUSE_DO_RESUME, 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_PAUSE);
    totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 0);
    totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 1);
    totk_pause_set_reason(&s, TOTK_PAUSE_BACKGROUND, 0);   /* focus and preference off */
    totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, 0);
    assert(totk_pause_next(&s) == TOTK_PAUSE_DO_RESUME);
    totk_pause_complete(&s, TOTK_PAUSE_DO_RESUME, 0);
    assert(s.phase == TOTK_PAUSE_RUNNING && !s.reasons);
}
static void shutdown_each_phase(void) {
    for (int phase = TOTK_PAUSE_RUNNING; phase <= TOTK_PAUSE_RESUMING; phase++) {
        for (int failed = 0; failed < 2; failed++) {
            TotkPauseState s = {.phase = phase, .supported = 1, .reasons = 7};
            totk_pause_shutdown(&s);
            assert(s.closing && !s.reasons);
            assert(!totk_pause_set_reason(&s, TOTK_PAUSE_SETTINGS, 1));
            assert(!totk_pause_toggle_manual(&s));
            if (phase == TOTK_PAUSE_PAUSING)
                totk_pause_complete(&s, TOTK_PAUSE_DO_PAUSE, failed ? -1 : 0);
            if (phase == TOTK_PAUSE_RESUMING)
                totk_pause_complete(&s, TOTK_PAUSE_DO_RESUME, 0);
            TotkPauseWork work = totk_pause_next(&s);
            if (work == TOTK_PAUSE_DO_RESUME) totk_pause_complete(&s, work, 0);
            else assert(work == TOTK_PAUSE_NO_WORK);
            assert(s.phase == TOTK_PAUSE_RUNNING && !s.reasons);
            assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
        }
    }
}
static void unavailable_backend(void) {
    TotkPauseState s = {0};
    for (unsigned i = 0; i < 1000; i++) {
        unsigned reason = 1u << (i % 3);
        assert(!totk_pause_set_reason(&s, reason, i & 1));
        assert(!totk_pause_toggle_manual(&s));
        assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
        assert(!s.reasons && s.phase == TOTK_PAUSE_RUNNING);
    }
    totk_pause_shutdown(&s);
    assert(totk_pause_next(&s) == TOTK_PAUSE_NO_WORK);
}
int main(void) {
    stack_reasons(); rollback_failure(); release_only_owner(); rapid_requests();
    shutdown_each_phase(); unavailable_backend();
    puts("pass: pause state acknowledgements, stacked reasons, rollback, rapid requests, shutdown and unavailable backend");
}

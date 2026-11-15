/* Frontend pause coordination. Owned by one UI thread; runtime work is asynchronous.
   This tracks acknowledgements, never guesses that a requested pause succeeded.
   A backend may enable it only after its barrier also stops HLE work safely. */
#ifndef TOTK_PAUSE_STATE_H
#define TOTK_PAUSE_STATE_H

enum {
    TOTK_PAUSE_MANUAL = 1u << 0,
    TOTK_PAUSE_SETTINGS = 1u << 1,
    TOTK_PAUSE_BACKGROUND = 1u << 2,
};
typedef enum { TOTK_PAUSE_RUNNING, TOTK_PAUSE_PAUSING, TOTK_PAUSE_PAUSED, TOTK_PAUSE_RESUMING } TotkPausePhase;
typedef enum { TOTK_PAUSE_NO_WORK, TOTK_PAUSE_DO_PAUSE, TOTK_PAUSE_DO_RESUME } TotkPauseWork;
typedef struct {
    unsigned reasons;
    TotkPausePhase phase;
    int supported, closing;
} TotkPauseState;

static inline int totk_pause_set_reason(TotkPauseState *s, unsigned reason, int on) {
    if (!s->supported || s->closing) return 0;
    if (on) s->reasons |= reason;
    else s->reasons &= ~reason;
    return 1;
}
/* Resume from the menu also closes settings. Background ownership remains with
   focus notifications, so a manual command cannot silently defeat that policy. */
static inline int totk_pause_toggle_manual(TotkPauseState *s) {
    if (!s->supported || s->closing) return 0;
    if (s->reasons)
        s->reasons &= ~(TOTK_PAUSE_MANUAL | TOTK_PAUSE_SETTINGS);
    else s->reasons |= TOTK_PAUSE_MANUAL;
    return 1;
}
static inline void totk_pause_shutdown(TotkPauseState *s) {
    s->closing = 1;
    s->reasons = 0;
}
static inline TotkPauseWork totk_pause_next(TotkPauseState *s) {
    if (s->phase == TOTK_PAUSE_RUNNING && s->supported && !s->closing && s->reasons) {
        s->phase = TOTK_PAUSE_PAUSING;
        return TOTK_PAUSE_DO_PAUSE;
    }
    if (s->phase == TOTK_PAUSE_PAUSED && !s->reasons) {
        s->phase = TOTK_PAUSE_RESUMING;
        return TOTK_PAUSE_DO_RESUME;
    }
    return TOTK_PAUSE_NO_WORK;
}
/* Returns -1 only for a failed pause. Every outstanding reason is rolled back:
   later user input may retry, but focus/UI refresh cannot start a retry loop. */
static inline int totk_pause_complete(TotkPauseState *s, TotkPauseWork work, int result) {
    if (work == TOTK_PAUSE_DO_PAUSE && s->phase == TOTK_PAUSE_PAUSING) {
        s->phase = result == 0 ? TOTK_PAUSE_PAUSED : TOTK_PAUSE_RUNNING;
        if (result != 0) { s->reasons = 0; return -1; }
    } else if (work == TOTK_PAUSE_DO_RESUME && s->phase == TOTK_PAUSE_RESUMING) {
        s->phase = TOTK_PAUSE_RUNNING;
    }
    return 0;
}
#endif

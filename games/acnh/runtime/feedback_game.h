/* Animal Crossing: New Horizons' own part of the controller feedback (the shared runtime/gamepad_feedback.m, shared/README.md): nothing
   beyond the shared rumble and motion. Included once, by gamepad_feedback.m, after its rumble and motion code. */
#ifndef ACNH_FEEDBACK_GAME_H
#define ACNH_FEEDBACK_GAME_H

/* Rumble and motion go to the controller attached last, as in games/totk. */
#define FEEDBACK_KEEP_FIRST 0

static void feedback_game_attach(GCController *c, const char *name) { (void)c; (void)name; }
static void feedback_game_detach(GCController *c) { (void)c; }
#endif

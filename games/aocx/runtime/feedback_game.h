/* Super Mario Odyssey's own part of the controller feedback (the shared runtime/gamepad_feedback.m, shared/README.md).
   Included once, by gamepad_feedback.m, after its rumble and motion code. */
#ifndef ODYSSEY_FEEDBACK_GAME_H
#define ODYSSEY_FEEDBACK_GAME_H

/* Rumble and motion stay with the first controller that can take them, until it goes away (2026-11-03, commits 64d45be6
   and 7185ec5c): runtime/gamepad.m offers the pads in connection order, and again after one leaves, and in two-player
   mode only Mario's (seat 1) controller, so Cappy's controller never takes Mario's vibration or aim
   (tests/feedback_routing). */
#define FEEDBACK_KEEP_FIRST 1

/* The DualSense trigger resistance of games/totk follows Link's bow, read from that game's memory; Odyssey has no bow,
   so it is left out (PR #1564 copied it in, where bow_state_supported() was always 0) and the triggers stay as the pad
   has them. The calls stay for frontend.m. */
int runtime_triggers_available(void) { return 0; }
int runtime_triggers_locked(void) { return 1; }
int runtime_triggers_choice(void) { return 0; }
void runtime_triggers_choose(int on) { (void)on; }
void runtime_triggers_release(void) {}

static void feedback_game_attach(GCController *c, const char *name) { (void)c; (void)name; }
static void feedback_game_detach(GCController *c) { (void)c; }
#endif

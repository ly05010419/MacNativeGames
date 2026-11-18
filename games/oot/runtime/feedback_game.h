/* Ocarina of Time's own part of the controller feedback (the shared runtime/gamepad_feedback.m, shared/README.md).
   Included once, by gamepad_feedback.m, after its rumble and motion code. */
#ifndef OOT_FEEDBACK_GAME_H
#define OOT_FEEDBACK_GAME_H

/* Rumble and motion go to the controller attached last, as in games/totk. */
#define FEEDBACK_KEEP_FIRST 0

/* games/totk's DualSense bow triggers read Link's bow from that game's memory (Breath of the Wild 1.9.0, Tears of the
   Kingdom 1.4.3); this game has no bow state, so the triggers stay as the pad has them and the settings panel's row
   says "Not in this version" (runtime/settings_game.h). The copy this game had never turned them on either. */
int runtime_triggers_available(void) { return 0; }
int runtime_triggers_locked(void) { return 1; }
int runtime_triggers_choice(void) { return 0; }
void runtime_triggers_choose(int on) { (void)on; }
void runtime_triggers_release(void) {}

static void feedback_game_attach(GCController *c, const char *name) { (void)c; (void)name; }
static void feedback_game_detach(GCController *c) { (void)c; }
#endif

/* Pokémon Legends: Arceus's side of the shared iPhone / iPad app (games/totk/ios/Sources, which
   games/pla/ios/tools/build_ipa.py compiles with GAME_PLA, as Animal Crossing's and Super Mario Odyssey's): what
   runtime/frontend.m gives the Mac and the shared shell or the shared runtime/frontend_ios.m asks of a game runtime,
   answered here. */
#import <UIKit/UIKit.h>
#include <stdio.h>
#include "runtime.h"

/* The phone's frame rate as Tears of the Kingdom's touch controls and picture settings know it (its hle_nvn.c): Pokémon
   Legends: Arceus runs at its own 30 fps step (the Mac's 60 fps track runs the game too fast and rewrites code at run
   time, which the phone cannot); the touch controls show no button for it (touch_pad.m, port_features.h). */
int runtime_phone_fps(void) { return 30; }
void runtime_phone_fps_set(int fps) { (void)fps; }

/* The drawable's shape (shared frontend_ios.m): the game keeps its 16:9 picture and the present centres it with bars
   (nvn_mtl.m, "Keep the game picture's aspect ratio"); there is no camera patch to widen it (Super Mario Odyssey's
   odyssey_aspect.c). */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }

/* The safe area for the corner boxes (shared frontend_ios.m; the shared hud_panel.inc's mtl_hud_safe_area): this
   graphics layer still draws its own boxes (tests/test_hud_panel_shared.py PENDING), inside the 16:9 picture, which the
   bars keep away from the notch and the rounded corners. */
void mtl_hud_safe_area(int w, int h, int side, int bottom) { (void)w; (void)h; (void)side; (void)bottom; }

/* A UIKit text field (the shared text_input.m) takes the hardware keyboard while it is up: this runtime's gamepad.m reads
   controllers only (no hardware keyboard on the phone), so there is nothing to pause. */
void runtime_keyboard_capture(int on) { (void)on; }

/* The phone's own sensor (the shared phone_motion.m) feeds runtime_motion_feed while no controller with one is there; it
   says when it starts (1, re-aligned to gravity) and stops (0). This runtime's motion_fusion.c takes the samples as they
   come and keeps no per-source state (the shared runtime/motion_fusion.c's phone hand-off is not in its copy). */
void runtime_motion_phone(int on) { (void)on; }

/* Key 1 (input.c, a hardware keyboard or a synthetic key): the in-picture HUD on or off, as on the Mac, without its
   window's shortcut list (frontend.m), which the phone does not have. */
int fps_overlay_toggle_hints(void);
void runtime_shortcut_help_toggle(void) { (void)fps_overlay_toggle_hints(); }

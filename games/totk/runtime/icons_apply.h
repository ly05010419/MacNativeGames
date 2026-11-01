/* The settings panel's Apply button icons (Mac): draws the button icons for the keyboard layout in force into a staged
   keyboard icons mod, <data>/mods/<version>-keyboard.next, which the launcher puts in place at the next start
   (keyboard_mods.h). Runs make_button_icons.py --style plan from the app (build_app.place_icon_scripts) with the
   builder's icons Python, in the background; the game's own mod folders are never written while it runs. */
#pragma once
#import <Foundation/Foundation.h>

typedef NS_ENUM(int, IconsApplyState) { IconsApplyIdle, IconsApplyRunning, IconsApplyDone, IconsApplyFailed, IconsApplyNoTools };
/* On the main queue: the state, the step done of steps, and for a failure what went wrong. */
typedef void (^IconsApplyUpdate)(IconsApplyState state, int step, int steps, NSString *detail);

BOOL icons_apply_start(IconsApplyUpdate update);   /* NO while a run is going */
IconsApplyState icons_apply_state(int *step, int *steps);
void icons_apply_cancel(void);                     /* stops a run and removes what it made; also at exit */
int icons_apply_stale(void);                       /* 1: the keyboard icons predate a change to the game's mods */

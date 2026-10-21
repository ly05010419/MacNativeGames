/* The launcher's settings window (app/settings_window.m): the game's settings of app/launch_settings.h in four tabs. */
#pragma once
#import <AppKit/AppKit.h>

typedef NS_ENUM(NSInteger, LaunchChoice) { LaunchQuit, LaunchSave, LaunchOnce };

/* Shows the window and waits. values: the settings as saved (name -> value; a setting left out is the game's default), edited
   in place. fromEnv: names already in the launcher's environment (shown, not editable). problem: why the window opened by
   itself (the last start failed), or nil; highlight: the setting it names, or nil. showAlways: "show this window at every
   start", edited in place. editOnly (the game's Settings… while it runs): Save and Cancel instead of the launch buttons;
   Save returns LaunchSave. */
LaunchChoice launch_settings_window(NSMutableDictionary<NSString *, NSString *> *values, NSSet<NSString *> *fromEnv,
                                    NSString *problem, NSString *highlight, BOOL *showAlways, BOOL editOnly);

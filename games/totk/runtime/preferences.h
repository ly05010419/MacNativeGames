/* AppKit menu bar and preferences window.  This header is ObjC-only; the C
   runtime only calls totk_preferences_install() from frontend.m. */
#ifndef TOTK_PREFERENCES_H
#define TOTK_PREFERENCES_H

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>

/* Installs the application/game/edit/window/help menus and creates the
   singleton preferences window.  Must be called on the main thread after the
   game window exists. */
void totk_preferences_install(NSWindow *gameWindow);

/* Re-reads the settings model (for example after an external profile import). */
void totk_preferences_refresh(void);

/* Flushes pending settings before the game window exits with _exit(). */
void totk_preferences_flush(void);

/* Shows the preferences window (used by the menu and by external callers). */
void totk_preferences_show(void);
#endif

#endif

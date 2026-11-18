/* Tears of the Kingdom's and Breath of the Wild's own part of the controller feedback (the shared runtime/gamepad_feedback.m,
   shared/README.md): the DualSense's adaptive triggers follow Link's bow, read from the game's memory, and the settings
   panel's bow trigger row (runtime_triggers_*). Included once, by gamepad_feedback.m, after its rumble and motion code. */
#ifndef TOTK_FEEDBACK_GAME_H
#define TOTK_FEEDBACK_GAME_H

/* Rumble and motion go to the controller attached last (a new pad takes them; the pad before keeps neither). */
#define FEEDBACK_KEEP_FIRST 0

/* The trigger resistance follows the bow, which is read from the game's memory: macOS only (iOS: stubs, never on). */
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>   /* NSApp.active: R2 resists only while the app is in front */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "bow_state.h"
#include "game_version.h"   /* Tears of the Kingdom 1.4.3's scene slot and actor name offsets, for the bow state */
/* ---- DualSense adaptive triggers ----
   R2 (ZR: draw the bow) resists like a bowstring, more the further it is pressed, but only while Link is drawing the
   bow; at any other time both triggers are slack, and L2 never resists. The game sends a controller nothing for this, so
   a thread reads it from the game's memory every 10 ms (runtime/bow_state.h: Breath of the Wild 1.9.0 and Tears of the
   Kingdom 1.4.3; other versions leave the triggers alone) and R2 changes through GameController only when the state does.
   It follows the draw within a frame; over Bluetooth the controller takes another 30-45 ms, so the very start of the pull
   is slack. On by default: dualsense-triggers.txt next to controls.txt saying "off" turns it off (TOTK_DUALSENSE_TRIGGERS
   =on / off overrides the file; an agent's test session, TOTK_SLOT_OWNER claude / codex, leaves it off unless set).
   R2 resists only while bow_resists (bow_state.h) says so, and lets go the moment one part does not: the draw ends, a menu
   pauses the world (Tears of the Kingdom: BOW_PAUSED; Breath of the Wild's timer drops to 0), the state cannot be read (a
   load, the title screen), the settings panel or the on-screen keyboard takes the controller (until its buttons are
   released), or the app leaves the front (it lets go then, as macOS would, and sends the state again when it is back).
   Both triggers go back to off when the controller goes away (another DualSense still connected takes the bow), when the
   settings turn them off, or when the game ends. TOTK_TRIGGER_LOG=1 logs every change of the triggers' mode and status,
   and why R2 holds or lets go (diagnostics). */
enum { BOW_POLL_MS = 10 };
static GCDualSenseGamepad *triggerPad;   /* main queue */
static int bowState = BOW_UNKNOWN;       /* main queue: the bow thread's last reading */
static BOOL gameInput = YES, appActive;  /* main queue: the game gets the controllers; the app is in front */
static BOOL r2Resists;                   /* main queue: what R2 was last set to */
static int triggerChoice = -1;   /* main queue: the settings panel's pick, or the file's word once read; -1 not yet */
static int triggers_wanted(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS"), *owner = getenv("TOTK_SLOT_OWNER");
    if (e && *e) {
        if (!strcmp(e, "on") || !strcmp(e, "1")) return 1;
        if (!strcmp(e, "off") || !strcmp(e, "0")) return 0;
        runtime_fail("TOTK_DUALSENSE_TRIGGERS must be on or off");
    }
    if (owner && (!strcmp(owner, "claude") || !strcmp(owner, "codex"))) return 0;
    if (triggerChoice >= 0) return triggerChoice;
    char word[16] = {0}, *w = word;
    if (!runtime_setting_word("dualsense-triggers.txt", word, sizeof word)) return triggerChoice = 1;
    while (*w && strchr(" \t\n\v\f\r", *w)) w++;   /* the blanks around the word go, as for every one-word file */
    for (size_t n = strlen(w); n && strchr(" \t\n\v\f\r", w[n - 1]); n--) w[n - 1] = 0;
    if (!strcmp(w, "off")) return triggerChoice = 0;
    if (strcmp(w, "on")) fprintf(stderr, "[triggers] dualsense-triggers.txt: not on or off; on\n");
    return triggerChoice = 1;
}
static int bow_state_supported(void) {
#if defined(GAME_BOTW190) || defined(TOTK_GAME_V143)
    return 1;
#else
    return 0;
#endif
}
#if defined(GAME_BOTW190) || defined(TOTK_GAME_V143)
static int bow_read(uint64_t address, void *out, size_t size) {   /* fails instead of faulting on a bad address */
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
#endif
static int bow_state_now(void) {   /* the bow thread only */
#if defined(GAME_BOTW190)
    uint64_t main_base = (uintptr_t)runtime_module_base("main");
    return main_base ? bow_state_botw190(main_base, bow_read) : BOW_UNKNOWN;
#elif defined(TOTK_GAME_V143)
    static BowTotkCache player;
    uint64_t main_base = (uintptr_t)runtime_module_base("main");
    return main_base ? bow_state_totk143(main_base, GAME_ADDR(0x4663f08), GAME_ACTOR_NAME, bow_read, &player) : BOW_UNKNOWN;
#else
    return BOW_UNKNOWN;
#endif
}
/* main queue: R2 as bow_resists says; sent when that changes, or always (force: a new pad, the app active again, whose
   effects macOS dropped) */
static void triggers_r2(BOOL force) {
    if (!triggerPad) return;
    BOOL resists = bow_resists(bowState, gameInput, appActive);
    if (resists == r2Resists && !force) return;
    if (resists) [triggerPad.rightTrigger setModeSlopeFeedbackWithStartPosition:0.05f endPosition:0.95f startStrength:0.2f endStrength:0.8f];
    else [triggerPad.rightTrigger setModeOff];
    if (resists != r2Resists && getenv("TOTK_TRIGGER_LOG"))
        fprintf(stderr, "[triggers] R2 %s\n", resists ? "bowstring" : !appActive ? "slack: the app is not in front" :
                !gameInput ? "slack: the settings panel or keyboard has the controller" : bowState == BOW_PAUSED ? "slack: the game is paused" :
                bowState == BOW_UNKNOWN ? "slack: no bow state" : "slack: the bow is not drawn");
    r2Resists = resists;
}
static void triggers_send(void) {   /* main queue: both triggers, at attach and when the app is active again */
    if (!triggerPad) return;
    [triggerPad.leftTrigger setModeOff];
    triggers_r2(YES);
}
static void triggers_off(void) {
    if (!triggerPad) return;
    [triggerPad.leftTrigger setModeOff]; [triggerPad.rightTrigger setModeOff]; triggerPad = nil; r2Resists = NO;
}
/* The game ends: both triggers off, and a moment for the report to reach the controller. At exit, and when the window
   closes (frontend.m): its _exit skips atexit, and a save can keep the game running a while after it. */
void runtime_triggers_release(void) { if (triggerPad) { triggers_off(); usleep(50000); } }
/* The bow thread: every 10 ms the bow's state and whether the game gets the controllers; the main queue hears of a change.
   A line the first time the state can be read (the world is up), and every 30 s one with the draws that ended, if any;
   TOTK_TRIGGER_LOG=1 also logs each change of the state. */
static void *bow_watch(void *arg) {
    (void)arg;
    pthread_setname_np("bow-state");
    static const char *names[] = {"unknown", "not drawn", "drawn", "paused"};
    BOOL found = NO, log = getenv("TOTK_TRIGGER_LOG") != NULL;
    int last = BOW_UNKNOWN, lastInput = 1;
    unsigned draws = 0; uint64_t since = 0, longest = 0, logAt = runtime_now_ns();
    for (;;) {
        int state = bow_state_now(), input = runtime_gamepad_game_input();
        uint64_t t = runtime_now_ns();
        if (state != BOW_UNKNOWN && !found) { found = YES; fprintf(stderr, "[triggers] bow state found: R2 follows the bow\n"); }
        if (state != last || input != lastInput) {
            if ((state == BOW_DRAWN) != (last == BOW_DRAWN)) {
                if (state == BOW_DRAWN) since = t;
                else { draws++; if (t - since > longest) longest = t - since; }
            }
            if (log && state != last) fprintf(stderr, "[triggers] bow %s\n", names[state + 1]);
            if (log && input != lastInput) fprintf(stderr, "[triggers] the game %s the controllers\n", input ? "has" : "does not have");
            last = state; lastInput = input;
            dispatch_async(dispatch_get_main_queue(), ^{ bowState = state; gameInput = input; triggers_r2(NO); });
        }
        if (t - logAt >= 30000000000ull) {
            if (draws) fprintf(stderr, "[triggers] last 30 s: bow drawn %u times, longest %.1f s\n", draws, longest / 1e9);
            draws = 0; longest = 0; logAt = t;
        }
        usleep(BOW_POLL_MS * 1000);
    }
    return NULL;
}
/* TOTK_TRIGGER_LOG=1: a line whenever a trigger's mode or status changes or its arm moves a tenth of its travel (status:
   0/1 feedback no load / load applied, 7-9 slope feedback ready / applying load / finished, -1 unknown). */
static void triggers_watch(void) {   /* main queue, every 250 ms */
    static long last[2][3] = {{-9, -9, -9}, {-9, -9, -9}};
    GCDualSenseGamepad *pad = triggerPad;
    if (!pad) return;
    GCDualSenseAdaptiveTrigger *t[2] = {pad.leftTrigger, pad.rightTrigger};
    for (int i = 0; i < 2; i++) {
        long now[3] = {(long)t[i].mode, (long)t[i].status, lroundf(t[i].armPosition * 10)};
        if (!memcmp(now, last[i], sizeof now)) continue;
        memcpy(last[i], now, sizeof now);
        fprintf(stderr, "[triggers] %s mode %ld status %ld arm %.2f pressed %.2f\n", i ? "R2" : "L2", now[0], now[1], t[i].armPosition, t[i].value);
    }
}
static void triggers_apply(GCController *c, const char *name) {   /* main queue */
    if (![c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class] || !triggers_wanted()) return;
    if (!bow_state_supported()) { fprintf(stderr, "[triggers] %s: no bow state for this game version; triggers left alone\n", name); return; }
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        atexit(runtime_triggers_release);
        appActive = NSApp.active;   /* from here the two notifications keep it */
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *n) { (void)n; appActive = YES; triggers_send(); }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *n) { (void)n; appActive = NO; triggers_r2(YES); }];
        if (getenv("TOTK_TRIGGER_LOG")) {
            static dispatch_source_t watch;
            watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(watch, dispatch_time(DISPATCH_TIME_NOW, 0), 250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(watch, ^{ triggers_watch(); });
            dispatch_resume(watch);
        }
        pthread_t thread; pthread_attr_t attr;
        pthread_attr_init(&attr); pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
        int r = pthread_create(&thread, &attr, bow_watch, NULL);
        pthread_attr_destroy(&attr);
        if (r) fprintf(stderr, "[triggers] the bow thread could not start (%d): R2 stays slack\n", r);
        else pthread_detach(thread);
    });
    if (triggerPad && triggerPad != c.extendedGamepad) { [triggerPad.leftTrigger setModeOff]; [triggerPad.rightTrigger setModeOff]; }   /* a newer pad takes the bow; the old one goes slack */
    triggerPad = (GCDualSenseGamepad *)c.extendedGamepad;
    triggers_send();
    fprintf(stderr, "[triggers] %s: R2 is a bowstring while the bow is drawn (slope 0.2 -> 0.8 over 5-95 %%), L2 slack\n", name);
}
/* The DualSense to take the bow when none has it (the settings turned it on, or the one that had it went away): the pad in
   use, else any connected one; never `gone`, which is disconnecting. */
static void triggers_pick(GCController *gone) {   /* main queue */
    if (triggerPad) return;
    GCController *current = GCController.current, *pick = current != gone && [current.extendedGamepad isKindOfClass:GCDualSenseGamepad.class] ? current : nil;
    for (GCController *c in GCController.controllers) if (!pick && c != gone && [c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) pick = c;
    if (pick) triggers_apply(pick, (pick.vendorName ?: @"controller").UTF8String);
}

/* The settings panel (key 1, runtime/settings_panel.h; main queue). TOTK_DUALSENSE_TRIGGERS or an agent's test session decide
   first: the row then shows that value, greyed out. A pick is saved in dualsense-triggers.txt and acts at once: on puts the
   connected DualSense's R2 on the bow (as at its attach), off lets both triggers go. */
int runtime_triggers_available(void) { return bow_state_supported(); }
int runtime_triggers_locked(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS"), *owner = getenv("TOTK_SLOT_OWNER");
    return (e && *e) || (owner && (!strcmp(owner, "claude") || !strcmp(owner, "codex")));
}
int runtime_triggers_choice(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS");
    if (e && *e) return !strcmp(e, "on") || !strcmp(e, "1");   /* shown only: a bad value stops the game at a DualSense's attach, as before */
    return triggers_wanted();
}
void runtime_triggers_choose(int on) {
    if (runtime_triggers_locked() || !bow_state_supported()) return;
    triggerChoice = !!on;
    runtime_setting_save("dualsense-triggers.txt", on ? "on" : "off", "DualSense trigger setting");
    if (!on) {   /* every DualSense's triggers go slack, also one that an earlier pad took the bow from */
        for (GCController *c in GCController.controllers)
            if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) { GCDualSenseGamepad *p = (GCDualSenseGamepad *)c.extendedGamepad; [p.leftTrigger setModeOff]; [p.rightTrigger setModeOff]; }
        triggerPad = nil; r2Resists = NO; fprintf(stderr, "[triggers] off (settings): both triggers slack\n"); return;
    }
    triggers_pick(nil);
}
#else
int runtime_triggers_available(void) { return 0; }
int runtime_triggers_locked(void) { return 1; }
int runtime_triggers_choice(void) { return 0; }
void runtime_triggers_choose(int on) { (void)on; }
void runtime_triggers_release(void) {}
#endif

/* gamepad_feedback.m: a controller connected (its name for the log) or went away. */
static void feedback_game_attach(GCController *c, const char *name) {   /* main queue */
#if TARGET_OS_OSX
    triggers_apply(c, name);
#else
    (void)c; (void)name;
#endif
}
static void feedback_game_detach(GCController *c) {   /* main queue */
#if TARGET_OS_OSX
    if (triggerPad && c.extendedGamepad == triggerPad) { triggers_off(); triggers_pick(c); }   /* another DualSense takes the bow */
#else
    (void)c;
#endif
}
#endif

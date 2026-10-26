/* Language of the host's own text on screen (boot screen, warm-up progress, overlays, keyboard prompt): Chinese when the
   first of the user's preferred languages is Chinese, English otherwise. Not the current locale's language: an app with
   no Chinese localization (the iOS app) gets English there whatever the phone is set to, so the boot screen came out in
   English under the iOS app's own Chinese text (2026-10-23). TOTK_UI_LANG=zh or en overrides it (read on every call, so tests can
   switch). The game's own language is chosen separately (TOTK_LANG, services.c desired_language). */
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

static inline int totk_ui_zh(void) {
    const char *e = getenv("TOTK_UI_LANG");
    if (e && *e) return !strncmp(e, "zh", 2);
    static int system_zh = -1;
    if (system_zh < 0) {
        char language[16] = {0};
        CFArrayRef preferred = CFLocaleCopyPreferredLanguages();
        if (preferred && CFArrayGetCount(preferred) > 0)
            CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(preferred, 0), language, sizeof language, kCFStringEncodingUTF8);
        if (preferred) CFRelease(preferred);
        system_zh = !strncmp(language, "zh", 2);
    }
    return system_zh;
}
#define TOTK_UI(zh, en) (totk_ui_zh() ? (zh) : (en))

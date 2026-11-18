#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Use the same domain for the launcher, the bundled runtime and local test binaries. */
#define KIRBY_PREFERENCES CFSTR("local.projectidea.kirby")

static inline int kirby_language_choice(void) {
    char value[32] = {0};
    const char *override = getenv("KIRBY_LANGUAGE");
    if (override) snprintf(value, sizeof value, "%s", override);
    else {
        CFPropertyListRef saved = CFPreferencesCopyAppValue(CFSTR("KirbyLanguage"), KIRBY_PREFERENCES);
        if (saved && CFGetTypeID(saved) == CFStringGetTypeID())
            CFStringGetCString(saved, value, sizeof value, kCFStringEncodingUTF8);
        if (saved) CFRelease(saved);
    }
    return !strcmp(value, "zh-Hans") ? 1 : !strcmp(value, "en-US") ? 2 : 0;
}

static inline void kirby_language_save(int choice) {
    CFPreferencesSetAppValue(CFSTR("KirbyLanguage"), choice == 1 ? CFSTR("zh-Hans") :
                            choice == 2 ? CFSTR("en-US") : CFSTR("system"), KIRBY_PREFERENCES);
    CFPreferencesAppSynchronize(KIRBY_PREFERENCES);
}

static inline void kirby_language_apply(void) {
    int choice = kirby_language_choice();
    char preferred[128] = "en-US";
    CFArrayRef languages = CFLocaleCopyPreferredLanguages();
    if (languages && CFArrayGetCount(languages))
        CFStringGetCString(CFArrayGetValueAtIndex(languages, 0), preferred, sizeof preferred, kCFStringEncodingUTF8);
    if (languages) CFRelease(languages);
    const char *tag = choice == 1 ? "zh-Hans" : choice == 2 ? "en-US" : preferred;
    /* Explicit command-line environment overrides remain useful for diagnostics. */
    setenv("TOTK_LOCALE", tag, 0);
    if (choice) setenv("TOTK_LANG", tag, 0);
    const char *effective = getenv("TOTK_LANG");
    if (!effective || !*effective) effective = getenv("TOTK_LOCALE");
    setenv("TOTK_UI_LANG", effective && !strncmp(effective, "zh", 2) ? "zh" : "en", 0);
    const char *log = getenv("DM_NVN_LOG");
    if (log && !strcmp(log, "1")) fprintf(stderr, "[kirby] language choice %s, locale %s, game override %s, host UI %s\n",
            choice == 1 ? "zh-Hans" : choice == 2 ? "en-US" : "system", getenv("TOTK_LOCALE"),
            getenv("TOTK_LANG") ? getenv("TOTK_LANG") : "auto", getenv("TOTK_UI_LANG"));
}

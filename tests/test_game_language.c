/* Test the Mac locale -> Switch language and region mapping (games/totk/runtime/game_language.h). */
#include "../games/totk/runtime/game_language.h"
#include <stdio.h>

static int failures;
static void language(const char *lang, const char *script, const char *region, int brazil, const char *want) {
    const char *got = game_language(lang, script, region, brazil);
    if (strcmp(got, want)) { fprintf(stderr, "FAIL: %s-%s-%s (brazil %d): %s, expected %s\n", lang, script, region, brazil, got, want); failures++; }
}
static void region(const char *code, int want) {
    int got = game_language_region_code(code);
    if (got != want) { fprintf(stderr, "FAIL: region of %s: %d, expected %d\n", code, got, want); failures++; }
}

int main(void) {
    language("en", "", "", 0, "en-US");
    language("en", "", "US", 0, "en-US");
    language("en", "", "CA", 0, "en-US");
    language("en", "", "GB", 0, "en-GB");   /* #754: British English plays in Celsius */
    language("en", "", "AU", 0, "en-GB");
    language("en", "", "TW", 0, "en-GB");
    language("fr", "", "FR", 0, "fr");
    language("fr", "", "CA", 0, "fr-CA");
    language("es", "", "ES", 0, "es");
    language("es", "", "MX", 0, "es-419");
    language("es", "", "419", 0, "es-419");
    language("pt", "", "BR", 1, "pt-BR");
    language("pt", "", "BR", 0, "pt");
    language("pt", "", "PT", 1, "pt");
    language("zh", "Hant", "", 0, "zh-Hant");
    language("zh", "", "TW", 0, "zh-Hant");
    language("zh", "", "HK", 0, "zh-Hant");
    language("zh", "", "CN", 0, "zh-Hans");
    language("zh", "Hans", "TW", 0, "zh-Hans");
    language("ja", "", "JP", 0, "ja");
    language("ko", "", "KR", 0, "ko");
    language("de", "", "AT", 0, "de");
    language("sv", "", "SE", 0, "en-US");
    region("ja", 0); region("en-US", 1); region("fr-CA", 1); region("es-419", 1); region("pt-BR", 1);
    region("en-GB", 2); region("fr", 2); region("de", 2); region("ru", 2); region("pt", 2);
    region("zh-Hant", 4); region("ko", 4); region("zh-Hans", 4);
    if (failures) return 1;
    puts("all game language tests pass");
    return 0;
}

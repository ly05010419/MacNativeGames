/* The Switch language code and region for a Mac locale (runtime/services.c oe::GetDesiredLanguage, runtime/settings_service.c
   GetRegionCode). The locale comes as BCP 47 subtags: the language ("en"), the script ("Hant", may be empty) and the region
   (upper case "GB" or "419", may be empty). Regional variants follow the console: a British, Irish or Australian English
   locale plays British English, which shows temperatures in Celsius (#754: en-GB had played American English, in Fahrenheit);
   French in Canada is Canadian French; Spanish in the Americas is Latin American Spanish. brazil: the build's RomFS has the
   official Brazilian Portuguese text (TOTK 1.4.3, BotW 1.9.0). */
#pragma once
#include <string.h>

static inline int game_language_region_in(const char *region, const char *const *list) {
    for (; *list; list++) if (!strcmp(region, *list)) return 1;
    return 0;
}

static inline const char *game_language(const char *language, const char *script, const char *region, int brazil) {
    static const char *const american_english[] = {"", "US", "CA", "PR", "UM", "VI", "AS", "GU", "MP", NULL};
    static const char *const latin_american_spanish[] = {"419", "US", "MX", "AR", "BO", "CL", "CO", "CR", "CU", "DO", "EC",
                                                         "SV", "GT", "HN", "NI", "PA", "PY", "PE", "PR", "UY", "VE", NULL};
    static const char *const same[] = {"ja", "de", "it", "nl", "ru", "ko", NULL};
    for (const char *const *l = same; *l; l++) if (!strcmp(language, *l)) return *l;
    if (!strcmp(language, "fr")) return !strcmp(region, "CA") ? "fr-CA" : "fr";
    if (!strcmp(language, "es")) return game_language_region_in(region, latin_american_spanish) ? "es-419" : "es";
    if (!strcmp(language, "pt")) return brazil && !strcmp(region, "BR") ? "pt-BR" : "pt";
    if (!strcmp(language, "zh"))
        return !strcmp(script, "Hant") || (!script[0] && (!strcmp(region, "TW") || !strcmp(region, "HK") || !strcmp(region, "MO")))
               ? "zh-Hant" : "zh-Hans";
    if (!strcmp(language, "en") && !game_language_region_in(region, american_english)) return "en-GB";
    return "en-US";   /* American English, also for a language neither game has */
}

/* nn::settings::system::RegionCode the console of that language would have: 0 Japan, 1 the Americas, 2 Europe,
   4 Hong Kong / Taiwan / Korea. Chinese, Simplified too, gets 4 as on an Asian console; 5 (China) is the Tencent console,
   which runs separate releases of the games. */
static inline int game_language_region_code(const char *code) {
    static const char *const americas[] = {"en-US", "fr-CA", "es-419", "pt-BR", NULL};
    static const char *const asia[] = {"zh-Hant", "zh-TW", "zh-Hans", "zh-CN", "ko", NULL};
    if (!strcmp(code, "ja")) return 0;
    if (game_language_region_in(code, americas)) return 1;
    if (game_language_region_in(code, asia)) return 4;
    return 2;
}

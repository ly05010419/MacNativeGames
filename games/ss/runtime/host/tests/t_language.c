// Settings language comparisons through the host hooks. All codes are synthetic;
// no game module, control data, graphics device or font file is needed.
#include "../hle_sdk16.c"
#include <limits.h>

module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 20; }
int dm_main_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop stand-ins in hle_sdk16.c hold the game out of focus
void dm_applet_leave(void) {}
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};

int main(void) {
    // Check the full matrix: both Chinese spellings match their own script;
    // different scripts, region variants and every unrelated language stay distinct.
    for (int32_t lang = 0; lang < 18; lang++) for (int i = 0; i < 18; i++) {
        char code[8] = {0}; memcpy(code, codes[i], strlen(codes[i]));
        unsigned want = lang == i || (lang == 6 && i == 15) || (lang == 15 && i == 6)
                                 || (lang == 11 && i == 16) || (lang == 16 && i == 11);
        CHECK(h_set_LangEq(&lang, code) == want, "Language == LanguageCode matrix");
        CHECK(h_set_LangEq2(code, &lang) == want, "LanguageCode == Language matrix");
    }
    const int32_t invalid[] = {-1, INT_MIN, 18, INT_MAX};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        CHECK(!h_set_LangEq(&invalid[i], "zh-Hans"), "invalid Language is not equal");
        CHECK(!h_set_LangEq2("zh-Hans", &invalid[i]), "invalid Language is not equal (reversed)");
        CHECK(h_set_LangMake(invalid[i]) == 0, "invalid Make still returns an empty code");
    }
    // A LanguageCode is exactly eight bytes; a missing terminator must not
    // accept an alias prefix or read past the supplied object.
    const char malformed[][8] = {"", "zh", "zh-hans", "zh-HansX", "zh-HantX", "zh-CNxyz", "zh-TWxyz"};
    const int32_t chinese[] = {6, 11, 15, 16};
    for (unsigned i = 0; i < sizeof malformed / sizeof *malformed; i++)
        for (unsigned j = 0; j < sizeof chinese / sizeof *chinese; j++) {
            CHECK(!h_set_LangEq(&chinese[j], malformed[i]), "malformed code is not an alias");
            CHECK(!h_set_LangEq2(malformed[i], &chinese[j]), "malformed code is not an alias (reversed)");
        }
    // Matching aliases must not collapse the system table: newer games use its
    // indices, and callers of Make must continue to receive the original spelling.
    char available[19 * 8]; memset(available, 0xa5, sizeof available);
    CHECK(h_set_AvailLangN() == 18, "available language count stays 18");
    CHECK(h_set_AvailLang(available, 19) == 18, "available language list keeps all 18 entries");
    for (int i = 0; i < 18; i++) {
        char expected[8] = {0}; memcpy(expected, codes[i], strlen(codes[i]));
        uint64_t code = h_set_LangMake(i);
        CHECK(!memcmp(&code, expected, 8), "Make preserves each exact spelling");
        CHECK(!memcmp(available + i * 8, expected, 8), "available language list preserves order and spelling");
    }
    const unsigned char *tail = (const unsigned char *)available + 18 * 8;
    for (int i = 0; i < 8; i++) CHECK(tail[i] == 0xa5, "available language list leaves the unused entry alone");
    printf("language: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

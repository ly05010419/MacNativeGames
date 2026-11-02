/* SDK 7.3.2 entry points that TOTK's runtime does not cover, answered at the API level.

   Luigi's Mansion 3 runs offline: no internet, friends, local wireless, shop or web pages. These SDK 7.3.2 entry points
   answer the way a console without those services does, at the API level, instead of reaching IPC services the host does
   not provide. Each failure result is a real module code, so the game's own "not available" paths run. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
#define NIFM_UNAVAILABLE RESULT(110, 300)      /* 2110-0300: no network connection */
#define FRIENDS_UNAVAILABLE RESULT(121, 6)     /* 2121-0006: network service unavailable */
#define LDN_DISABLED RESULT(203, 22)           /* 2203-0022: wireless communication disabled */
#define APPLET_UNAVAILABLE RESULT(128, 3)      /* 2128-0003: the applet cannot start */
#define NSD_UNAVAILABLE RESULT(141, 5)
#define SSL_UNAVAILABLE RESULT(123, 2)
#define ACCOUNT_NSA_UNAVAILABLE RESULT(124, 280) /* 2124-0280: network service account unavailable */

static void once(const char *what) { fprintf(stderr, "[lm3] offline: %s\n", what); }

static void nifm_initialize(void) { once("nifm initialized, no network"); }
static unsigned char nifm_available(void) { return 0; }
static void nifm_submit_and_wait(void) {}
static uint32_t nifm_unavailable(void) { return NIFM_UNAVAILABLE; }

static void friends_initialize(void) {}
static uint32_t friends_unavailable(void) { return FRIENDS_UNAVAILABLE; }

static uint32_t ldn_disabled(void) { return LDN_DISABLED; }
static void ldn_finalize(void) {}
static int ldn_state(void) { return 0; }   /* State_None: never initialized */
static uint32_t ldn_ipv4(unsigned char a, unsigned char b, unsigned char c, unsigned char d) { return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d; }
static uint32_t ldn_broadcast(uint32_t address, uint32_t mask) { return address | ~mask; }

static uint32_t applet_unavailable(void) { once("an applet (shop, web page, user creator) is not available"); return APPLET_UNAVAILABLE; }
static void web_request_exit(void) {}
static void error_show(const void *variant) { (void)variant; once("the game asked to show a system error"); }
static uint32_t nsd_unavailable(void) { return NSD_UNAVAILABLE; }
static uint32_t ssl_unavailable(void) { return SSL_UNAVAILABLE; }
static uint32_t account_nsa_unavailable(void) { return ACCOUNT_NSA_UNAVAILABLE; }

/* nn::settings::LanguageCode is the eight-byte code ("en-US"); SDK 7 asks the settings service even for this table. */
static const char *const language_codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                             "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static uint64_t language_code(int language) {
    if (language < 0 || language >= (int)(sizeof language_codes / sizeof *language_codes)) runtime_fail("unknown settings language");
    uint64_t code = 0; memcpy(&code, language_codes[language], strlen(language_codes[language])); return code;
}
static unsigned char code_is(const uint64_t *code, const int *language) { return *code == language_code(*language); }
static unsigned char is_code(const int *language, const uint64_t *code) { return code_is(code, language); }

/* nn::oe: the exit-request section only matters when the system asks to close; no system asks here. */
static void oe_exit_section(void) {}
static uint32_t oe_performance_configuration(int mode) { return mode ? 0x92220008u : 0x00010000u; }   /* docked 768 MHz / handheld default */

void *runtime_lm3_hle_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn4nifm10InitializeEv", nifm_initialize}, {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_and_wait},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_unavailable},
        {"_ZN2nn7friends10InitializeEv", friends_initialize},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friends_unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_unavailable},
        {"_ZN2nn7friends14ShowFriendListERKNS_7account3UidE", applet_unavailable},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", friends_unavailable},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", friends_unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_unavailable},
        {"_ZN2nn3ldn10InitializeEv", ldn_disabled}, {"_ZN2nn3ldn8FinalizeEv", ldn_finalize}, {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn15MakeIpv4AddressEhhhh", ldn_ipv4},
        {"_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", ldn_broadcast},
        {"_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", applet_unavailable},
        {"_ZN2nn2ec24ShowShopAddOnContentListEv", applet_unavailable},
        {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", applet_unavailable},
        {"_ZN2nn3web26RequestExitOfflineHtmlPageEv", web_request_exit},
        {"_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", error_show},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_unavailable},
        {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_unavailable},
        {"_ZN2nn7account15ShowUserCreatorEv", applet_unavailable},
        {"_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", language_code},
        {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", code_is},
        {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", is_code},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", oe_exit_section}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", oe_exit_section},
        {"_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", oe_performance_configuration},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", account_nsa_unavailable},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", account_nsa_unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", account_nsa_unavailable},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", account_nsa_unavailable},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    /* Every other local-wireless entry point fails the same way (the game only reaches them after Initialize). */
    if (!strncmp(name, "_ZN2nn3ldn", 10)) return ldn_disabled;
    return NULL;
}

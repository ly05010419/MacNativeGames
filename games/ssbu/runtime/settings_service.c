/* nn::settings "set" service (ISettingsServer), for SDK versions whose settings functions ask it from inside the SDK:
   Breath of the Wild 1.6.0 (SDK 7.3.2) calls nn::settings::operator==(LanguageCode, Language), which asks MakeLanguageCode.
   Language codes are listed in nn::settings::Language order; the current language is the one oe::GetDesiredLanguage
   reports (runtime/services.c: the Mac's language, or TOTK_LANG). Linked into the BotW program only (botw.md). */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>

static const char *const CODES[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
enum { NCODES = sizeof CODES / sizeof CODES[0], NCODES_V1 = 15 };   /* commands 1 / 3 list the first 15 (pre-4.0 firmware) */

static uint64_t code(unsigned i) { uint64_t v = 0; memcpy(&v, CODES[i], strlen(CODES[i])); return v; }
static uint64_t current(void) {
    uint64_t (*desired)(void) = (uint64_t (*)(void))runtime_override("_ZN2nn2oe18GetDesiredLanguageEv");
    return desired ? desired() : code(1);
}

/* Fill an out buffer with up to `limit` codes; a C buffer ("pointer") is also reported back to the client. */
static int32_t put_codes(const IpcRequest *rq, IpcResponse *rs, unsigned limit) {
    int pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    IpcBuffer b = pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
    unsigned n = 0;
    if (b.size) {
        uint64_t *dst = ipc_guest(b.addr, b.size);
        for (; n < limit && (n + 1) * 8 <= b.size; n++) dst[n] = code(n);
        if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, n * 8, b.index};
    }
    return (int32_t)n;
}

static void settings_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t v = current(); IPC_OUT(rs, v); return; }                         /* GetLanguageCode */
    case 1: { int32_t n = put_codes(rq, rs, NCODES_V1); IPC_OUT(rs, n); return; }       /* GetAvailableLanguageCodes */
    case 2: {                                                                            /* MakeLanguageCode(Language) */
        int32_t language = IPC_ARG(rq, int32_t, 0);
        if (language < 0 || language >= NCODES) { rs->result = 0x3c69u; return; }     /* settings: invalid language */
        uint64_t v = code((unsigned)language); IPC_OUT(rs, v); return;
    }
    case 3: { int32_t n = NCODES_V1; IPC_OUT(rs, n); return; }                          /* GetAvailableLanguageCodeCount */
    case 4: { int32_t region = 1; IPC_OUT(rs, region); return; }                        /* GetRegionCode: USA */
    case 5: { int32_t n = put_codes(rq, rs, NCODES); IPC_OUT(rs, n); return; }          /* GetAvailableLanguageCodes2 */
    case 6: { int32_t n = NCODES; IPC_OUT(rs, n); return; }                             /* GetAvailableLanguageCodeCount2 */
    case 8: { uint8_t quest = 0; IPC_OUT(rs, quest); return; }                          /* GetQuestFlag: not a kiosk unit */
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_settings(void) { runtime_ipc_register("set", settings_handler, NULL, NULL); }

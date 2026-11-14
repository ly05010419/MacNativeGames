/* set (nn::settings::ISettingsServer): the system language, the languages a console offers and its region. The system
   language is the one the game is told it should use (services.c: TOTK_LANG, TOTK_LOCALE or the Mac's language).
   Newer SDKs ask it through IPC (Paper Mario: The Thousand-Year Door's nn::settings::GetAvailableLanguageCodes). */
#include "ipc.h"
#include <stdio.h>
#include <string.h>

uint64_t runtime_desired_language(void);

/* nn::settings::Language order; the first 15 are what a pre-4.0 system offered (commands 1 and 3). */
static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NCODES (int)(sizeof codes / sizeof *codes)
#define NLEGACY 15

static uint64_t code(int i) { uint64_t c = 0; memcpy(&c, codes[i], strlen(codes[i])); return c; }

/* The reply's out buffer: map-alias (B), or pointer (C) with auto-select, which the reply then describes. */
static void *out_buffer(const IpcRequest *rq, IpcResponse *rs, uint64_t *size) {
    if (rq->nb && rq->b[0].size) { *size = rq->b[0].size; return ipc_guest(rq->b[0].addr, rq->b[0].size); }
    if (rq->nc && rq->c[0].size) {
        *size = rq->c[0].size;
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, rq->c[0].size, 0};
        return ipc_guest(rq->c[0].addr, rq->c[0].size);
    }
    *size = 0;
    return NULL;
}

static void available(const IpcRequest *rq, IpcResponse *rs, int limit) {
    uint64_t size; uint8_t *out = out_buffer(rq, rs, &size);
    int32_t n = limit < (int)(size / 8) ? limit : (int)(size / 8);
    for (int i = 0; i < n; i++) { uint64_t c = code(i); memcpy(out + 8 * i, &c, 8); }
    IPC_OUT(rs, n);
}

/* RegionCode from the system language: Japan 0, USA 1 (the Americas), Europe 2, HongKongTaiwanKorea 4. */
static int32_t region(void) {
    static const int32_t by_language[NCODES] = {0, 1, 2, 2, 2, 2, 4, 4, 2, 2, 2, 4, 2, 1, 1, 4, 4, 1};
    uint64_t l = runtime_desired_language();
    for (int i = 0; i < NCODES; i++) if (l == code(i)) return by_language[i];
    return 2;
}

static void settings(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t l = runtime_desired_language(); IPC_OUT(rs, l); return; }            /* GetLanguageCode */
    case 1: available(rq, rs, NLEGACY); return;                                            /* GetAvailableLanguageCodes */
    case 2: {                                                                               /* MakeLanguageCode(Language) */
        uint32_t i = IPC_ARG(rq, uint32_t, 0);
        if (i >= (uint32_t)NCODES) ipc_unimplemented(s, rq);   /* a language this table does not know */
        uint64_t c = code((int)i); IPC_OUT(rs, c); return;
    }
    case 3: { int32_t n = NLEGACY; IPC_OUT(rs, n); return; }                               /* GetAvailableLanguageCodeCount */
    case 4: { int32_t r = region(); IPC_OUT(rs, r); return; }                              /* GetRegionCode */
    case 5: available(rq, rs, NCODES); return;                                             /* GetAvailableLanguageCodes2 */
    case 6: { int32_t n = NCODES; IPC_OUT(rs, n); return; }                                /* GetAvailableLanguageCodeCount2 */
    case 8: { uint8_t quest = 0; IPC_OUT(rs, quest); return; }                             /* GetQuestFlag: not a store kiosk */
    case 11: {                                                                              /* GetDeviceNickName */
        uint64_t size; char *out = out_buffer(rq, rs, &size);
        if (out && size) { memset(out, 0, size); strncpy(out, "Mac", size - 1); }
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_settings(void) { runtime_ipc_register("set", settings, NULL, NULL); }

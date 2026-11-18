// System service interfaces used by games on newer SDKs (16.x): oe / time / pl (shared font) / vi vsync / diag.
// On the real device all of these go through Horizon system services (IPC); the host gives values directly or makes them "unavailable".
#include "hle.h"
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------- oe ----------
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "CPU boost mode; the host does not scale clocks", void, h_oe_CpuBoost, int m) { (void)m; }
// DisplayVersion = char[16] (the application's display version): the value from that version's control data (NACP), chosen by main's build ID (the game profile's build rows); unknown builds get 1.0.0
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "display version (by the main build ID)", void, h_oe_DispVer, char *v) {
    static const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
    const char *ver = "1.0.0";
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (unsigned j = 0; j < sizeof k / sizeof *k; j++) if (!strcmp(g_mod[i].build_id, k[j].build)) ver = k[j].ver;
    memset(v, 0, 16); strlcpy(v, ver, 16);
}
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", "system / application volume balance; the host always reports 1:1", void, h_oe_GetVolBal, float *sys, float *app) { *sys = 1.0f; *app = 1.0f; }
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", "volume balance; the host ignores it", void, h_oe_SetVolBal, float sys, float app) { (void)sys; (void)app; }
// Program total active time (nn::TimeSpan, nanoseconds): the host's monotonic time since startup
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "total active time → time since the host started", int64_t, h_oe_ActiveTime, void) { return (int64_t)(now_ns() - g_t0_ns); }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "exit request critical section; the host exits through SIGTERM / the lifecycle", void, h_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "exit request critical section; the host ignores it", void, h_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "extended auto-sleep delay; the host ignores it", void, h_oe_Inactivity, unsigned on) { (void)on; }

// ---------- time ----------
// CalendarTime = { int16 year; int8 month, day, hour, minute, second }, returned by value (within 8 bytes, in x0)
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "absolute seconds → UTC calendar (host gmtime_r)", uint64_t, h_time_ToCalUtc, const int64_t *t) {
    time_t v = (time_t)*t; struct tm tm; gmtime_r(&v, &tm);
    return (uint64_t)(uint16_t)(tm.tm_year + 1900) | (uint64_t)(uint8_t)(tm.tm_mon + 1) << 16 | (uint64_t)(uint8_t)tm.tm_mday << 24 |
           (uint64_t)(uint8_t)tm.tm_hour << 32 | (uint64_t)(uint8_t)tm.tm_min << 40 | (uint64_t)(uint8_t)tm.tm_sec << 48;
}
// LocationName = char[0x24] (time zone name): follows the host's system time zone
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "device time zone name → host time zone", Result, h_time_LocName, char *out) {
    memset(out, 0, 0x24); tzset(); strlcpy(out, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24); return RES_OK;
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "network clock (offline, unavailable)", Result, h_time_NetNow, int64_t *out) { (void)out; return RES_FAIL; }
HLE_FORCE("_ZN2nn4time8FinalizeEv", "pairs with Initialize; the host has no session", Result, h_time_Finalize, void) { return RES_OK; }
// "Ensure network clock availability" async task: no request is sent; the finish event fires immediately and the result is failure. The object itself is untouched (constructor / destructor no-ops), the event lives in host static storage
static _Alignas(16) uint8_t g_ev_netclock[0x100];
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "network clock task constructor; the host opens no session", void, h_time_NcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "network clock task destructor; no session to close", void, h_time_NcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "network clock task init (offline)", Result, h_time_NcInit, void *self, int mode) { (void)self; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "network clock task start: finishes at once with failure", Result, h_time_NcStart, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "network clock task finish event: already signalled", void *, h_time_NcEvent, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return g_ev_netclock; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "network clock task result: offline, failure", Result, h_time_NcResult, void *self) { (void)self; return RES_FAIL; }

// The host has none of the original system shared fonts (0 standard / 1 Simplified Chinese / 2 Simplified Chinese extension / 3 Traditional Chinese / 4 Korean / 5 Nintendo extension) and uses stand-ins:
//   Nintendo extension = DM_SHARED_FONT_EXT or $DM_FONT_DIR/nintendo_ext.ttf (generated by dm/tools/make_ext_font.py: button icons at the original private-use code points, e.g. U+E0E0 = A button);
//   all other types share one = DM_SHARED_FONT or $DM_FONT_DIR/standard.ttf, then macOS's Arial Unicode (that path does not exist on iOS; the App ships dm/font/).
//   If the extension font cannot be opened it falls back to the standard font (button icons draw as "?", everything else works).
// The game's own text uses the fonts in its RomFS; shared fonts are only used in a few places such as system names and button icons.
// Format: each font is BFTTF - an 8-byte header + the TTF (obfuscated by per-word XOR in the system archive). Wonder calls nn::fontll::ScalableFontEngineHelper::Decode(address, size) on the address
//   (SMBW sdk +0x3093c0), which deobfuscates in place and returns address + 8. Decode first checks the first word: if it is already the "decoded" marker (little-endian 0x18029a7f) it returns address + 8 without touching the data.
//   So the host provides the "decoded" marker + the TTF as is; the second word holds the obfuscated length as in the original (key 0x49621806: stored as bswap(length ^ key)). A raw TTF would be XORed into garbage as if it were ciphertext,
//   and the font engine then crashes on a wild pointer (SMBW main +0x107dbc0). Games that read the address as a TTF get it past the header (font_skip below).
typedef struct { uint8_t *p; size_t n; } font_t;
static font_t font_map(const char *p) {
    font_t f = {0}; int fd = open(p, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return f; }
    size_t n = (size_t)st.st_size;
    uint8_t *m = mmap(NULL, n + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { LOG("[RED] shared font memory allocation failed (%zu bytes)", n + 8); close(fd); return f; }
    size_t got = 0; while (got < n) { ssize_t r = pread(fd, m + 8 + got, n - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) { LOG("[RED] shared font read short: %s (%zu / %zu bytes)", p, got, n); munmap(m, n + 8); return f; }
    uint32_t done = 0x18029a7fu, len = __builtin_bswap32((uint32_t)n ^ 0x49621806u);
    memcpy(m, &done, 4); memcpy(m + 4, &len, 4);
    f.p = m; f.n = n + 8; return f;
}
static font_t font_first(const char *env, const char *file, const char *fallback) {
    char buf[1024]; const char *dir = getenv("DM_FONT_DIR"), *c[3] = {getenv(env), NULL, fallback};
    if (dir && *dir) { snprintf(buf, sizeof buf, "%s/%s", dir, file); c[1] = buf; }
    for (int i = 0; i < 3; i++) {
        if (!c[i] || !*c[i]) continue;
        font_t f = font_map(c[i]); if (f.p) { LOG("shared font (%s): %s (%zu bytes, BFTTF 'decrypted' header + TTF as is)", file, c[i], f.n - 8); return f; }
    }
    return (font_t){0};
}
static pthread_mutex_t font_mu = PTHREAD_MUTEX_INITIALIZER;
static font_t std_f, ext_f;
static font_t font_get(int type) {
    static int std_done, ext_done;
    pthread_mutex_lock(&font_mu);
    if (!std_done) { std_done = 1; std_f = font_first("DM_SHARED_FONT", "standard.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
                     if (!std_f.p) LOG("[RED] cannot open the standard shared font (point DM_SHARED_FONT or DM_FONT_DIR/standard.ttf at a TTF)"); }
    if (type == 5 && !ext_done) { ext_done = 1; ext_f = font_first("DM_SHARED_FONT_EXT", "nintendo_ext.ttf", NULL);
                                  if (!ext_f.p) LOG("[YELLOW] Nintendo extended shared font missing (dm/tools/make_ext_font.py generates nintendo_ext.ttf): button icons will be drawn as '?'"); }
    font_t f = type == 5 && ext_f.p ? ext_f : std_f;
    pthread_mutex_unlock(&font_mu);
    return f;
}
// ScalableFontEngine::LoadFont takes the TTF itself, so a game that imports Decode (and gets the header, font_skip below) but hands the address straight to LoadFont gets it past the header:
//   The Legend of Zelda: Echoes of Wisdom (1.1.0, SDK 20.5.1) records shared fonts as type 0, calls no Decode for them, and with the header LoadFont failed with 403 and SetFont with 301 on every font,
//   every glyph lookup then returned null and main+0x8b37c crashed at address 0x38 (GitHub PR #55). Only the host's own BFTTF bases qualify; Decode's result (base + 8), game-owned font data and NULL pass as is.
static const void *font_payload(const void *p) {
    pthread_mutex_lock(&font_mu);
    const void *r = p && (p == std_f.p || p == ext_f.p) ? (const uint8_t *)p + 8 : p;
    pthread_mutex_unlock(&font_mu);
    return r;
}
static uint32_t (*font_load)(void *, char *, const void *, unsigned, unsigned);
static pthread_once_t font_load_once = PTHREAD_ONCE_INIT;
static void font_load_init(void) {
    font_load = guest_sym("_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj");
    if (!font_load) die("ScalableFontEngine::LoadFont not found in the guest");
}
HLE_FORCE("_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj", "shared font BFTTF allocation base → TTF payload; parsing and errors stay in the SDK", uint32_t, h_font_LoadFont, void *self, char *name, const void *data, unsigned index, unsigned capacity) {
    pthread_once(&font_load_once, font_load_init);
    return font_load(self, name, font_payload(data), index, capacity);
}
// The address points straight at the TTF (past the 8-byte header) and the size excludes the header, except for a game on SDK 11 or later whose main imports ScalableFontEngineHelper::Decode
//   (dm_sdk_major, dm_main_imports; the profile's shared_font overrides). Super Mario 3D World + Bowser's Fury (SDK 10) records shared fonts as type 0 (main+0x975658), calls no Decode and hands the address
//   as is to ScalableFontEngine::LoadFont; with the header LoadFont fails with error 403 and SetFont with 301, every lookup of the "?" glyph then misses, and main+0x6c73c0 crashes on a null pointer.
//   Odyssey (7), Link's Awakening (8), Age of Calamity (9) and Three Houses (6) take it the same way. Unity's player hands the address and size of every type straight to FreeType's FT_New_Memory_Face
//   (Patrick's Parabox, SDK 13, Unity 2020.3: main+0x2390b0 maps its OS font names Switch_Font_STD … Switch_Font_NIN_Extended to types 0-5, then main+0x23b2e0), as libnx homebrew does with the same pl offsets;
//   with the header every face failed, so text the game's own font lacks (the Japanese, Korean and Chinese names in its language menu) drew nothing. Its main imports no Decode, as none of the Unity games'
//   does (Engage, Ace Attorney Investigations Collection, Brain Memory Training Academy); Wonder, Breath of the Wild, Tears of the Kingdom and Pokémon Legends: Z-A import it and pass the header to it
static size_t font_skip(void) {
    static int skip = -1;
    if (skip < 0) {
#ifdef DM_GAME_SHARED_FONT_TTF
        skip = DM_GAME_SHARED_FONT_TTF ? 8 : 0; LOG("shared font: %s (game profile shared_font)", skip ? "the TTF itself" : "BFTTF header + TTF");
#else
        int m = dm_sdk_major(), dec = dm_main_imports("_ZN2nn6fontll24ScalableFontEngineHelper6Decode"); skip = DM_SDK_OLD(m) || !dec ? 8 : 0;
        LOG("shared font: %s (SDK %d, main %s ScalableFontEngineHelper::Decode)", skip ? "the TTF itself" : "BFTTF header + TTF", m, dec ? "imports" : "does not import");
#endif
    }
    return (size_t)skip;
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "shared font load state: loaded (1)", int, h_pl_LoadState, int type) { return font_get(type).p ? 1 : 0; }
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "shared font address → BFTTF header (archive shared_font ttf: the TTF itself)", const void *, h_pl_Addr, int type) { const uint8_t *p = font_get(type).p; return p ? p + font_skip() : NULL; }
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "shared font size → 8-byte header + stand-in font file size (archive shared_font ttf: without the header)", size_t, h_pl_Size, int type) { font_t f = font_get(type); return f.p ? f.n - font_skip() : 0; }
// Every shared font at once (Rhythm Heaven Groove, SDK 21): the SDK asks the pl service for GetSharedFontInOrderOfPriority and, when the fonts are loaded, fills min(count, max) SharedFontData {u32 type; const void *address; size_t size}
//   (0x18 bytes) with the shared memory base + each font's offset (sdk+0x40f270), then returns whether they are loaded; the game retries every 10 ms until they are (main+0x501f04). The service's offsets point past each font's 8-byte
//   BFTTF header, and the game hands each address straight to ScalableFontEngine::LoadFont (main+0x501ff0), so this always gives the TTF itself, whatever the profile's shared_font. Unhooked, the SDK opens the pl service and aborts
//   in nn::sm::Initialize. All six types in their numeric order, whatever the language, as the pl service reimplementations do; the stand-ins make types 0-4 one font
typedef struct { uint32_t type, pad; const void *address; size_t size; } pl_font_data_t;
HLE_FORCE("_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE", "every shared font: the stand-in TTFs (past the BFTTF header), types 0-5 in order", unsigned, h_pl_GetSharedFont, int *count, pl_font_data_t *out, int max, uint64_t lang) {
    (void)lang; int n = 0;
    for (int t = 0; t < 6; t++) {
        font_t f = font_get(t); if (!f.p) continue;
        if (n < max) out[n] = (pl_font_data_t){ (uint32_t)t, 0, f.p + 8, f.n - 8 };
        n++;
    }
    *count = n; return n > 0;
}
// Whether running on a Switch 2 (codename Ounce) (returns bool): the host reports false, as a Switch 1. Mario 1.2.1 uses it to decide whether to count add-on content (the upgrade pack) and suggest the Switch 2 Edition
HLE_FORCE("_ZN2nn2pl16IsRunningOnOunceEv", "running on Switch 2: no", unsigned, h_pl_IsOunce, void) { return 0; }
// The Legend of Zelda: Breath of the Wild (SDK 20): asks for the shared fonts to be loaded before querying their state; the host's stand-in fonts are there on demand. VR mode (the Labo VR goggles): the host never enters VR; the query is always false, entering / leaving is a no-op
HLE_FORCE("_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", "request shared font load; the stand-in font is always available, no-op", void, h_pl_ReqLoad, int type) { (void)type; }
HLE_FORCE("_ZN2nn2pl8IsVrModeEv", "VR mode: no", unsigned, h_pl_IsVr, void) { return 0; }
HLE_FORCE("_ZN2nn2pl11BeginVrModeEv", "enter VR mode (through am); the host does not", void, h_pl_BeginVr, void) { LOG("pl::BeginVrMode (the host does not enter VR)"); }
HLE_FORCE("_ZN2nn2pl9EndVrModeEv", "leave VR mode (through am)", void, h_pl_EndVr, void) {}

// ---------- ec: shop applet (the upgrade-to-Switch-2-Edition page) ----------
// The shop is out of scope: no-op. The game does not look at the return value (both overloads are handled as returning nothing)
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionE", "shop upgrade page, out of scope; no-op", void, h_ec_ShowUpg, int ed) { LOG("ec::ShowShopApplicationInformationForUpgrade(%d) (cut)", ed); }
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionERKNS_7account10UserHandleE", "shop upgrade page (given user), out of scope; no-op", void, h_ec_ShowUpgU, int ed, const void *user) { (void)user; LOG("ec::ShowShopApplicationInformationForUpgrade(%d, user) (cut)", ed); }

// ---------- nfp: amiibo ----------
// From Mario 1.2.1 the game initializes nfp at boot (v0 did not use it). The real SDK would connect to the sm: service, which the host lacks → abort, so the whole family is taken over.
// The host has no NFC reader: it reports one NFC device on the currently connected controller (same as hle_hid.c) whose detection never finds a tag -- the same as real hardware with no amiibo at hand:
// the game's "touch an amiibo" screen appears as usual and cancelling leaves it. Operations that need a tag (mount, application area read / write, amiibo settings applets) all return "tag not found".
// The game attaches no activate / deactivate events; it polls GetDeviceState to see whether a tag is there.
#define NFP_RES_TAG_NOT_FOUND (115u | 97u << 9)   // module 115 (nfp), description 97
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1 };   // 0 initialized, 1 searching for a tag (2 = tag found, never on the host)
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
static _Atomic int g_nfp_state;
uint32_t hle_hid_connected_npad(void);
static Result nfp_no_tag(const char *what) {
    static _Atomic int logged; if (!atomic_exchange(&logged, 1)) LOG("nfp: %s and other operations that need an amiibo tag → no tag found (the host has no NFC)", what);
    return NFP_RES_TAG_NOT_FOUND;
}
HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo library init; the host has one NFC device that never finds a tag", void, h_nfp_Init, void) { LOG("nfp::Initialize: the host has one NFC device and finds no amiibo tag"); }
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC availability change event; bound to a kernel event, never signalled", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC device list: one device", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "controller the NFC device is on: the one connected now", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) { (void)h; *out = hle_hid_connected_npad(); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "start tag detection (never finds one)", Result, h_nfp_Start, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_SEARCH); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "stop tag detection", Result, h_nfp_Stop, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_INIT); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "device state: initialized / searching for a tag", int, h_nfp_State, const NfpDeviceHandle *h) { (void)h; return atomic_load(&g_nfp_state); }
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "tag info: no tag", Result, h_nfp_TagInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetTagInfo"); }
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "model info: no tag", Result, h_nfp_ModelInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetModelInfo"); }
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "mount tag: no tag", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { (void)h; (void)model; (void)target; return nfp_no_tag("Mount"); }
// Another group The Legend of Zelda: Breath of the Wild (SDK 20) uses: tag activate / deactivate events (bound to a kernel event that never fires; without the take-over the SDK's code reads a null pointer at sdk+0x3f2a78), the two-argument Mount,
//   Unmount, reading common / register info (all answered as no tag), the library state (1 = initialized) and Finalize
HLE_FORCE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag placed event; bound to a kernel event, never signalled", Result, h_nfp_AttachAct, void *ev, const NfpDeviceHandle *h) { (void)h; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag removed event; bound to a kernel event, never signalled", Result, h_nfp_AttachDeact, void *ev, const NfpDeviceHandle *h) { (void)h; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE", "mount tag (two arguments): no tag", Result, h_nfp_Mount2, const NfpDeviceHandle *h, int model) { (void)h; (void)model; return nfp_no_tag("Mount"); }
HLE_FORCE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", "unmount tag: no tag", Result, h_nfp_Unmount, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Unmount"); }
HLE_FORCE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", "common info: no tag", Result, h_nfp_CommonInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetCommonInfo"); }
HLE_FORCE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", "register info: no tag", Result, h_nfp_RegInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetRegisterInfo"); }
HLE_FORCE("_ZN2nn3nfp8GetStateEv", "amiibo library state: initialized (1)", int, h_nfp_LibState, void) { return 1; }
HLE_FORCE("_ZN2nn3nfp8FinalizeEv", "amiibo library finalize; the host has no session", void, h_nfp_Fini, void) {}
HLE_FORCE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "flush tag: no tag", Result, h_nfp_Flush, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Flush"); }
HLE_FORCE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "open application area: no tag", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) { (void)h; (void)id; return nfp_no_tag("OpenApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "read application area: no tag", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) { (void)buf; (void)h; (void)sz; if (n) *n = 0; return nfp_no_tag("GetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "write application area: no tag", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) { (void)h; (void)buf; (void)sz; return nfp_no_tag("SetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "create application area: no tag", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("CreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "recreate application area: no tag", Result, h_nfp_RecreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("RecreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo restorer applet: no tag", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartRestorer"); }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo game data eraser applet: no tag", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartGameDataEraser"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo nickname and owner settings applet: no tag", Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag) { (void)h; (void)reg; (void)p; (void)tag; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", "amiibo nickname and owner settings applet (with the old register info): no tag", Result, h_nfp_Nick2, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag, const void *old) { (void)h; (void)reg; (void)p; (void)tag; (void)old; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }

// ---------- vi: vsync event ----------
// The game passes a SystemEventType; after binding, a host thread fires it at 60 Hz (DM_VSYNC_HZ can change it). While the display link presents, the event follows its ticks, as a
// Switch's follows its display: Super Mario Galaxy 2 runs the Wii's VI retrace (and with it each emulated frame) on this event, and on a clock of the host's own the frames drifted
// against the screen's vsyncs, two in some display-link ticks and none in others (iPhone: about one tick in seven, a stutter the frame-rate counter showed as 50 to 60)
void dm_host_helper_qos(qos_class_t q);   // hle_thread.c
__attribute__((weak)) int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) { (void)seen; (void)timeout_ns; return 0; }   // nvn_mtl.m
static void *vsync_thread(void *ev) {
    dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // the game paces frames on this event
    const char *e = getenv("DM_VSYNC_HZ"); double hz = e ? atof(e) : 60.0; if (hz <= 0) hz = 60.0;
    uint64_t period = (uint64_t)(1e9 / hz), next = now_ns() + period, seen = 0;
    for (;;) {
        if (!e && mtl_link_wait_vsync(&seen, 2 * period)) { hle_signal_system_event(ev); next = now_ns() + period; continue; }
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)}; nanosleep(&ts, NULL); }
        hle_signal_system_event(ev); next += period; if (now_ns() > next + 4 * period) next = now_ns() + period;   // do not try to catch up when far behind
    }
    return NULL;
}
// Image memory needed for an indirect layer (an applet's picture handed to the game to draw, such as the inline keyboard): on hardware it asks the vi service. Computed as linear RGBA8, 4 KB aligned (Super Mario 3D World + Bowser's Fury's inline keyboard asks for 1280x720)
HLE_FORCE("_ZN2nn2vi33GetIndirectImageMemoryRequirementEPmS1_ii", "indirect layer image memory: RGBA8 linear, 4 KB aligned", Result, h_vi_IndirectReq, size_t *size, size_t *align, int w, int h) {
    *size = ((size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4 + 0xfff) & ~(size_t)0xfff; *align = 0x1000; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "vsync event: a host thread signals it at 60 Hz", Result, h_vi_VsyncEvent, void *ev, void *disp) {
    (void)disp; hle_bind_system_event(ev, 1);
    pthread_t th; if (pthread_create(&th, NULL, vsync_thread, ev)) die("cannot start the vsync thread"); pthread_detach(th);
    LOG("vi::GetDisplayVsyncEvent: %p signalled by the host at 60 Hz", ev); return RES_OK;
}

// ---------- diag ----------
// The text after LogMetaData (NN_LOG) goes straight into the host log; an abort prints the call site and exits with a failure
HLE_FORCE("_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", "game log → host log", void, h_diag_Put, const void *meta, const char *s, size_t n) {
    (void)meta; while (n && (s[n - 1] == '\n' || s[n - 1] == '\0')) n--; if (n) LOG("[game] %.*s", (int)n, s);
}
// Abort: report the call site (module + offset) and the guest backtrace, then exit
static void diag_abort(uint32_t res, void *lr) {
    module_t *m = module_of(lr);
    LOG("game called nn::diag abort: Result %#x, call site %s+%#llx", res, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)lr - m->base) : (unsigned long long)(uintptr_t)lr);
    dm_backtrace((uint64_t)__builtin_frame_address(1));
    die("game aborted");
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEv", "game aborted", void, h_diag_Abort0, void) { diag_abort(0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", "game aborted (with Result)", void, h_diag_AbortR, const uint32_t *r) { diag_abort(r ? *r : 0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag12GetBacktraceEPmi", "backtrace; the host provides none (0 frames)", int, h_diag_Bt, uint64_t *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn4diag13GetSymbolNameEPcmm", "symbol name; the host provides none (empty string)", uint64_t, h_diag_SymName, char *buf, size_t n, uint64_t addr) { (void)addr; if (buf && n) buf[0] = 0; return 0; }

// ---------- settings: language enum <-> language code ----------
// On the real device MakeLanguageCode asks the settings service; the table is public and fixed (nn::settings::Language order). LanguageCode = char[8]
static const char *const k_lang_code[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
HLE_FORCE("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", "language enum vs. language code comparison (returns bool)", unsigned, h_set_LangEq, const int32_t *lang, const char *code) {
    if ((unsigned)*lang >= sizeof k_lang_code / sizeof *k_lang_code) return 0;
    return strncmp(k_lang_code[*lang], code, 8) == 0;
}
// The reversed comparison and making a code from the enum (Super Mario 3D World + Bowser's Fury, SDK 10: its first comparison in nnMain connected to the settings service)
HLE_FORCE("_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", "language code vs. language enum comparison (returns bool)", unsigned, h_set_LangEq2, const char *code, const int32_t *lang) {
    if ((unsigned)*lang >= sizeof k_lang_code / sizeof *k_lang_code) return 0;
    return strncmp(k_lang_code[*lang], code, 8) == 0;
}
HLE_FORCE("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", "language enum → language code (8 bytes returned by value)", uint64_t, h_set_LangMake, int lang) {
    char code[8] = {0};
    if ((unsigned)lang < sizeof k_lang_code / sizeof *k_lang_code) strncpy(code, k_lang_code[lang], sizeof code);
    uint64_t v; memcpy(&v, code, 8); return v;
}
// Every language code the system offers, in nn::settings::Language order, as the settings service lists them; returns how many were written (Paper Mario: The Thousand-Year Door, SDK 15:
// nnMain asks for 32 and takes the index of GetDesiredLanguage's code in the list as its language, main+0x327888; the SDK's own version aborted in sm::Initialize)
HLE_FORCE("_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi", "available language codes (the fixed system table)", int, h_set_AvailLang, char *out, int count) {
    int n = 0;
    for (; n < count && n < (int)(sizeof k_lang_code / sizeof *k_lang_code); n++) { memset(out + 8 * n, 0, 8); strncpy(out + 8 * n, k_lang_code[n], 8); }
    return n;
}
// How many codes that list holds (Catherine: Full Body, SDK 8.2.1: nnMain compares LanguageCode::Make(0 … count − 1) with GetDesiredLanguage's code and keeps the matching index,
// main+0x117590; the SDK's own version aborted in sm::Initialize)
HLE_FORCE("_ZN2nn8settings29GetAvailableLanguageCodeCountEv", "number of available language codes (the fixed system table)", int, h_set_AvailLangN, void) {
    return (int)(sizeof k_lang_code / sizeof *k_lang_code);
}

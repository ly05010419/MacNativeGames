// Miscellaneous service layer: add-on content / shop / error applet / profanity filter / server name resolution / parental controls / software keyboard.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "hle.h"
#include <dirent.h>

// ---------- aoc add-on content (the DLC NSPs in the player's folder) / ec shop (excluded) ----------
// Owned add-on content indices (ascending), fixed at the first query:
//   numeric subdirectories of DM_AOC_DIR: each is one DLC's RomFS, which the packing script extracts only from DLC NSPs really in the player's folder (title ID = base + 0x1000 + index).
//     Engage: F001 → 1 (E0), F002 → 2 (H0); the content itself is in the 2.0.0 update's RomFS and each DLC package holds one empty marker file; the game calls MountAddOnContent for each index ListAddOnContent returns (hle_fs.c).
//   No DM_AOC_DIR: none (on a Switch 2 Mario 1.2.1 uses it to tell whether the upgrade pack is owned, and the user has no Mario add-on content files).
//   DM_AOC_OWNED=0: always none (test switch, for runs without the DLC).
// Room for every index the scan accepts (1..2000): Super Smash Bros. Ultimate has 99 DLC, and a cap below the folder count dropped whichever readdir returned last (fighters among them).
static int g_aoc[2000], g_naoc = -1;
static int aoc_int_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void aoc_load(void) {
    if (g_naoc >= 0) return;
    g_naoc = 0;
    const char *e = getenv("DM_AOC_OWNED"), *dir = getenv("DM_AOC_DIR"); const char *why = "no DM_AOC_DIR";
    if (e && !strcmp(e, "0")) why = "DM_AOC_OWNED=0";
    else if (dir && *dir) {
        why = dir; DIR *d = opendir(dir); struct dirent *ent;
        while (d && (ent = readdir(d)) && g_naoc < (int)(sizeof g_aoc / sizeof *g_aoc)) {
            char *end; long i = strtol(ent->d_name, &end, 10);
            if (ent->d_name[0] >= '1' && ent->d_name[0] <= '9' && (!*end || !strcmp(end, ".img")) && i <= 2000) g_aoc[g_naoc++] = (int)i;
        }
        if (d) closedir(d);
        qsort(g_aoc, g_naoc, sizeof *g_aoc, aoc_int_cmp);
        int unique = 0; for (int i = 0; i < g_naoc; i++) if (!i || g_aoc[i] != g_aoc[i - 1]) g_aoc[unique++] = g_aoc[i];
        g_naoc = unique;   // a legacy directory and its replacement image report the same DLC once
    }
    static char list[2000 * 5 + 1]; list[0] = 0; for (int i = 0; i < g_naoc; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%d", i ? "," : "", g_aoc[i]);
    LOG("aoc: reporting %d add-on contents {%s} (%s)", g_naoc, list, why);
}
// On the big host stack: the first query lists DM_AOC_DIR and logs, a few KB, and Digimon Story Cyber Sleuth asks from a thread with little stack left (SIGBUS in __vfprintf
// under aoc_load, a store right below a page of the guest thread's stack)
HLE_FORCE_BIG("_ZN2nn3aoc17CountAddOnContentEv", "add-on content count: length of the owned index list (DM_AOC_DIR)", int, h_aoc_Count, void) { aoc_load(); return g_naoc; }
// ListAddOnContent(out, start index, max count): writes out from the owned index list starting at the start index, returns the number written
HLE_FORCE_BIG("_ZN2nn3aoc16ListAddOnContentEPiii", "add-on content list: the owned index list", int, h_aoc_List, int *out, int off, int n) {
    int w = 0; aoc_load();
    for (int i = off < 0 ? 0 : off; i < g_naoc && w < n; i++) out[w++] = g_aoc[i];
    return w;
}
// The event object is created on demand by the nn::os side table (not signaled), the game waiting on it only times out / gets not-signaled
HLE_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", "add-on content change event, out of scope; bound to a kernel event, never signalled (the game polls it with TryWait and aborts if it is uninitialized)", void, h_aoc_GetEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListEv", "shop applet, out of scope; cut: no-op", void, h_ec_ShowShop, void) { DM_APPLET("shop"); LOG("ec::ShowShopAddOnContentList (cut)"); }
// The other shop entry points (Engage's title menu item "DLC" calls ShowShopAddOnContentList(ApplicationId, UserHandle)): the original starts the shop applet through applet::CreateLibraryApplet,
// the host has no applet proxy (oe initialization is a no-op), and the SDK reads through the null proxy and crashes (sdk+0x38660). All return void: as if the player closed the shop at once, the game goes back to its menu.
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE", "shop applet, out of scope; cut: no-op", void, h_ec_AocList1, void) { DM_APPLET("shop"); LOG("ec::ShowShopAddOnContentList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_AocList2, void) { DM_APPLET("shop"); LOG("ec::ShowShopAddOnContentList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_AocList3, void) { DM_APPLET("shop"); LOG("ec::ShowShopAddOnContentList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", "shop applet, out of scope; cut: no-op", void, h_ec_AppInfo1, void) { DM_APPLET("shop"); LOG("ec::ShowShopApplicationInformation (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_AppInfo2, void) { DM_APPLET("shop"); LOG("ec::ShowShopApplicationInformation (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub1, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub2, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS0_8CourseIdE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub3, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS0_8CourseIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub4, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsList1, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsList2, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsDet1, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemDetail (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsDet2, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemDetail (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec22ShowShopEnterCodeSceneEv", "shop applet, out of scope; cut: no-op", void, h_ec_Code1, void) { DM_APPLET("shop"); LOG("ec::ShowShopEnterCodeScene (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec22ShowShopEnterCodeSceneERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Code2, void) { DM_APPLET("shop"); LOG("ec::ShowShopEnterCodeScene (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidE", "shop applet, out of scope; cut: no-op", void, h_ec_Prod1, void) { DM_APPLET("shop"); LOG("ec::ShowShopProductDetails (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Prod2, void) { DM_APPLET("shop"); LOG("ec::ShowShopProductDetails (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKc", "shop applet, out of scope; cut: no-op", void, h_ec_ProdList1, void) { DM_APPLET("shop"); LOG("ec::ShowShopProductList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_ProdList2, void) { DM_APPLET("shop"); LOG("ec::ShowShopProductList (cut): the host has no shop"); }
// The overloads without an ApplicationId (the calling application's own pages; The Great Ace Attorney Chronicles, SDK 10, imports these beside the ones above)
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationEv", "shop applet, out of scope; cut: no-op", void, h_ec_AppInfo3, void) { DM_APPLET("shop"); LOG("ec::ShowShopApplicationInformation (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_AppInfo4, void) { DM_APPLET("shop"); LOG("ec::ShowShopApplicationInformation (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListEv", "shop applet, out of scope; cut: no-op", void, h_ec_Sub5, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub6, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListERKNS0_8CourseIdE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub7, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec24ShowShopSubscriptionListERKNS0_8CourseIdERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_Sub8, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec26ShowShopConsumableItemListEv", "shop applet, out of scope; cut: no-op", void, h_ec_ConsList3, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec26ShowShopConsumableItemListERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsList4, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemList (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec28ShowShopConsumableItemDetailERKNS0_12ConsumableIdENS0_5NsUidE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsDet3, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemDetail (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec28ShowShopConsumableItemDetailERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_ConsDet4, void) { DM_APPLET("shop"); LOG("ec::ShowShopConsumableItemDetail (cut): the host has no shop"); }
HLE_FORCE("_ZN2nn2ec27ShowShopSubscriptionDetailsERKNS0_8CourseIdENS0_5NsUidERKNS_7account10UserHandleE", "shop applet, out of scope; cut: no-op", void, h_ec_SubDet, void) { DM_APPLET("shop"); LOG("ec::ShowShopSubscriptionDetails (cut): the host has no shop"); }
// The shop service accessor (Unreal's online subsystem, DRAGON QUEST XI S: FOnlineSubsystemSwitch::Init calls InitializeForShopServiceAccessors, whose SDK version connects to the ec service:
// an abort in sm::Initialize at the start of nnMain). The library init succeeds with nothing to set up; an accessor never initializes, so no request is ever sent. The accessor and its async
// response are stubbed whole: the SDK's destructor calls Finalize and aborts unless it succeeds. A response is complete at once and holds nothing
HLE_FORCE("_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", "shop service accessors' library init (ec service); offline, nothing to set up", Result, h_ec_InitSsa, void *mem, size_t n) { (void)mem; (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessorC1Ev", "shop service accessor: no session (cut)", void, h_ec_SsaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessorD1Ev", "shop service accessor: no session to close (cut)", void, h_ec_SsaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", "shop service accessor: the shop is unavailable offline (cut)", Result, h_ec_SsaInit, void *self, const void *svc) { (void)self; (void)svc; return RES_FAIL; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", "shop request: never sent (cut)",
          Result, h_ec_SsaRequest, void *self, void *resp, const void *uid, int method, const char *path, const void *post, int64_t timeout) {
    (void)self; (void)resp; (void)uid; (void)method; (void)path; (void)post; (void)timeout; return RES_FAIL;
}
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev", "shop async response: no request (cut)", void, h_ec_ArCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseD1Ev", "shop async response: nothing to detach (cut)", void, h_ec_ArDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", "shop async response: complete at once (cut)", unsigned, h_ec_ArTryWait, void *self) { (void)self; return 1; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", "shop async response: no data (cut)", Result, h_ec_ArGetSize, void *self, size_t *n) { (void)self; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", "shop async response: no data (cut)", Result, h_ec_ArGet, void *self, void *buf, size_t n) { (void)self; (void)buf; (void)n; return RES_FAIL; }
// DRAGON QUEST VII Reimagined (Unreal Engine 4.27) also imports these three: the library's finalize (its SDK version closes the ec session the init never opened) and two response calls
HLE_FORCE("_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", "shop service accessors' library finalize: nothing was set up", void, h_ec_FinSsa, void) {}
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse6CancelEv", "shop async response: nothing to cancel (cut)", void, h_ec_ArCancel, void *self) { (void)self; }
HLE_FORCE("_ZNK2nn2ec19ShopServiceAccessor13AsyncResponse12GetErrorCodeEv", "shop async response: no error code (cut)", uint64_t, h_ec_ArErr, const void *self) { (void)self; return 0; }
// The "add-on content lost" error screen is an applet too: logged only
HLE_FORCE("_ZN2nn3aoc25ShowAddOnContentLostErrorEPKii", "add-on content lost error applet; the host only logs it", void, h_aoc_LostErr, const int *idx, int n) { DM_APPLET("error"); (void)idx; LOG("aoc::ShowAddOnContentLostError (%d entries) (cut)", n); }

// ---------- err error applet: printed by the host ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "error display applet; the host prints the module / description number", void, h_err_ShowError, Result r) { DM_APPLET("error");
    LOG("err::ShowError Result=%#x (module %u, description %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
// The class is polymorphic, and games call it through its vtable too: Monster Hunter Rise checks the hunter's name with slot 8, MaskProfanityWordsInText(int *, char16_t *, uint32_t)
// (main+0x6bca160). The constructor used to do nothing, the object kept what the heap held there, and that call jumped into the heap. It now gets a host vtable: each slot of the SDK's
// (_ZTVN2nn3ngc15ProfanityFilterE) is matched by address with the class's methods and given the host's version (no word list: nothing is profanity, nothing is masked); others return 0
static Result ngc_v_zero(void) { return RES_OK; }
static void ngc_v_none(void *self) { (void)self; }
static Result ngc_v_check(void *self, uint32_t *masks, uint32_t pat, const void **words, uint64_t n) { (void)self; (void)pat; (void)words; for (uint64_t i = 0; masks && i < n; i++) masks[i] = 0; return RES_OK; }
static Result ngc_v_mask(void *self, int *masked, void *text, uint32_t region) { (void)self; (void)text; (void)region; if (masked) *masked = 0; return RES_OK; }
static void *ngc_vtable(void) {
    enum { NV = 40 }; static void *vt[NV + 2]; static int built;
    if (built) return built > 0 ? &vt[2] : NULL;
    void **sdk = guest_sym("_ZTVN2nn3ngc15ProfanityFilterE");
    if (!sdk) { built = -1; LOG("[RED] ProfanityFilter: the SDK has no vtable symbol; objects keep none"); return NULL; }
    static const struct { const char *name; void *fn; } M[] = {
        {"_ZN2nn3ngc15ProfanityFilterD1Ev", (void *)ngc_v_none}, {"_ZN2nn3ngc15ProfanityFilterD0Ev", (void *)ngc_v_none}, {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", (void *)ngc_v_none},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", (void *)ngc_v_check}, {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", (void *)ngc_v_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", (void *)ngc_v_check},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", (void *)ngc_v_mask}, {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", (void *)ngc_v_mask},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", (void *)ngc_v_mask},
    };
    void *addr[sizeof M / sizeof *M]; for (unsigned k = 0; k < sizeof M / sizeof *M; k++) addr[k] = guest_sym(M[k].name);
    module_t *mod = module_of(sdk); int n = 0, named = 0;
    vt[0] = sdk[0]; vt[1] = sdk[1];   // offset to top, type info
    // A slot holds SDK code, or the host's version where the loader bound the slot by name to a forced take-over (Initialize, GetContentVersion, CheckProfanityWords): those stay.
    // The table ends at the first value that is neither (Monster Hunter Rise's SDK: 9 slots, then a 1)
    for (; n < NV && (uintptr_t)sdk[2 + n] >= 0x10000; n++) {
        if (module_of(sdk[2 + n]) != mod) { if (module_of(sdk[2 + n])) break; vt[2 + n] = sdk[2 + n]; continue; }   // another guest module: not this class's table
        vt[2 + n] = (void *)ngc_v_zero;
        for (unsigned k = 0; k < sizeof M / sizeof *M; k++) if (addr[k] && sdk[2 + n] == addr[k]) { vt[2 + n] = M[k].fn; named++; break; }
    }
    built = 1; LOG("ngc: ProfanityFilter host vtable of %d slots (%d matched to the class's methods)", n, named);
    return &vt[2];
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterC1Ev", "profanity filter, the word list is in a system save; the host builds none and gives the object a host vtable", void, h_ngc_Ctor, void *self) {
    void *vt = ngc_vtable(); if (vt && self) memcpy(self, &vt, sizeof vt);
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "profanity filter", void, h_ngc_Dtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", "profanity filter init; the host has no word list", Result, h_ngc_Init, void *self, void *work, uint64_t sz) { (void)self; (void)work; (void)sz; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "word list version; the host returns 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "profanity check; never a hit", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}
// Called by name as well as through the vtable (The Great Ace Attorney Chronicles, SDK 10, imports these three): the SDK's versions read the word list the host never mounted
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", "profanity masking (UTF-8); nothing is masked", Result, h_ngc_Mask8, void *self, int *masked, char *text, uint32_t region) { return ngc_v_mask(self, masked, text, region); }
// The UTF-16 forms by name: Dark Souls Remastered (SDK 4) masks the character name with the unsigned short one right after the name entry; the SDK's version allocated from a heap the host never set up
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", "profanity masking (UTF-16); nothing is masked", Result, h_ngc_Mask16, void *self, int *masked, uint16_t *text, uint32_t region) { return ngc_v_mask(self, masked, text, region); }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", "profanity masking (char16_t); nothing is masked", Result, h_ngc_Mask16c, void *self, int *masked, uint16_t *text, uint32_t region) { return ngc_v_mask(self, masked, text, region); }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", "profanity mask mode; nothing is masked whatever the mode", void, h_ngc_MaskMode, void *self, int mode) { (void)self; (void)mode; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", "profanity filter shutdown; the host mounted no word list", void, h_ngc_Fini, void *self) { (void)self; }

// ---------- nsd server name resolution / pctl free communication: networking, out of scope ----------
HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "server name resolution, networking out of scope; cut", Result, h_nsd_Resolve, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "server name resolution, networking out of scope; cut", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool arguments only look at the low 8 bits; returns bool
// The permission query answers as TryBegin / IsFreeCommunicationAvailable do: the SDK's version calls the pctl service (pctl::detail::ipc::GetService), which the host never opens (DRAGON QUEST XI S imports it; from PR #38)
HLE_FORCE("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", "parental controls · permission to communicate with others, networking out of scope; never, no system UI", unsigned, h_pctl_CheckFree, unsigned show_ui) { (void)show_ui; return 0; }
HLE_FORCE("_ZN2nn4pctl25TryBeginFreeCommunicationEb", "parental controls · communication with others, networking out of scope; never allowed", unsigned, h_pctl_TryBegin, unsigned show_ui) { (void)show_ui; return 0; }
// Parental control of the 3D (VR goggles) mode, Super Mario Odyssey: no restriction, nothing to configure
HLE_FORCE("_ZN2nn4pctl26GetStereoVisionRestrictionEv", "parental controls · 3D view restriction; never restricted", unsigned, h_pctl_GetStereo, void) { return 0; }
HLE_FORCE("_ZN2nn4pctl26SetStereoVisionRestrictionEb", "parental controls · 3D view restriction; the host does not store it", void, h_pctl_SetStereo, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv", "parental controls · whether the 3D view restriction can be changed; yes", Result, h_pctl_ConfStereo, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv", "parental controls · permission to change the 3D restriction; granted at once", Result, h_pctl_ReqStereo, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4pctl20EndFreeCommunicationEv", "parental controls · communication with others", void, h_pctl_End, void) {}
// Engage asks on entering a battle map (no UI); without this the SDK connects to the pctl service through sm, which the host does not have, and aborts
HLE_FORCE("_ZN2nn4pctl28IsFreeCommunicationAvailableEv", "parental controls · whether communication with others is available, networking out of scope; never", unsigned, h_pctl_IsFree, void) { return 0; }
// The Legend of Zelda: Breath of the Wild: checks the parental-control permission for stereoscopic 3D before turning on the options' Labo VR mode (returns bool; the argument = whether to
// show UI, the game passes 0). The host has no VR display (pl::BeginVrMode refuses, hle_sdk16.c): not permitted, so the game clears its VR flag and stays 2D (as MacNativeGames found on 1.6.0 and 1.9.0)
HLE_FORCE("_ZN2nn4pctl27CheckStereoVisionPermissionEb", "parental controls · stereoscopic 3D permission; the host has no VR display, not permitted", unsigned, h_pctl_Stereo, unsigned show_ui) { (void)show_ui; return 0; }

// swkbd software keyboard: see hle_swkbd.c

// ---------- streamplay: GameShare / GameChat streaming (Switch 2 SDKs) ----------
// Donkey Kong Bananza initializes it at boot (main+0x5471c0) through the stpl:u service, which the host does not serve. The host streams to nobody: the library is ready, no session is open
// and no guest is connected, so every guest pad, sensor, vibration and mouse query has nothing to report. Opening a session (OpenSessionWithMatchmakeSupport, StartStreaming) is a player
// action and stays unhooked: it would reach the service, open
HLE_FORCE("_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi", "streamplay work memory (stpl:u): one page, the host keeps no state in it",
          uint32_t, h_stpl_MemSize, int type, int mode, int n) { (void)type; (void)mode; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", "streamplay (stpl:u): ready, no session", Result, h_stpl_Init, int type, int mode, void *work, size_t size) {
    (void)type; (void)mode; (void)work; (void)size; return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay8FinalizeEv", "streamplay (stpl:u): nothing to release", void, h_stpl_Fini, void) {}
// The connection state change event: a host-owned SystemEvent, bound on first use and never signaled (the state never changes)
static uint8_t g_stpl_ev[0x80] __attribute__((aligned(16)));
HLE_FORCE("_ZN2nn10streamplay30GetConnectionStateChangedEventEv", "streamplay connection state event: bound, never signaled", void *, h_stpl_ConnEv, void) {
    hle_bind_system_event(g_stpl_ev, 0); return g_stpl_ev; }
HLE_FORCE("_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", "streamplay connection state: not connected (0)", void, h_stpl_ConnState, uint32_t *s) { *s = 0; }
static _Atomic uint32_t g_stpl_styles, g_stpl_hold;
HLE_FORCE("_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", "guest pad styles: kept for the getter", void, h_stpl_SetStyles, uint32_t s) { g_stpl_styles = s; }
HLE_FORCE("_ZN2nn10streamplay29GetSupportedGuestNpadStyleSetEv", "guest pad styles: what the game set", uint32_t, h_stpl_GetStyles, void) { return g_stpl_styles; }
HLE_FORCE("_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", "guest Joy-Con hold type: kept for the getter", void, h_stpl_SetHold, uint32_t t) { g_stpl_hold = t; }
HLE_FORCE("_ZN2nn10streamplay23GetGuestNpadJoyHoldTypeEv", "guest Joy-Con hold type: what the game set", uint32_t, h_stpl_GetHold, void) { return g_stpl_hold; }
HLE_FORCE("_ZN2nn10streamplay25SetSupportedHidFeatureSetENS_4util10BitFlagSetILi32ENS0_13HidFeatureTagEEE", "guest hid features: no guest", void, h_stpl_HidFeat, uint32_t s) { (void)s; }
HLE_FORCE("_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", "guest pad style: none (no guest)", uint32_t, h_stpl_PadStyle, const void *id) { (void)id; return 0; }
#define STPL_PAD(mangled, cname) HLE_FORCE(mangled, "guest pad states: none (no guest)", int, cname, void *out, int n, const void *id) { (void)out; (void)n; (void)id; return 0; }
STPL_PAD("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadFull)
STPL_PAD("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadHandheld)
STPL_PAD("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadDual)
STPL_PAD("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadRight)
STPL_PAD("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadLeft)
HLE_FORCE("_ZN2nn10streamplay28GetGuestSixAxisSensorHandlesEPNS0_24GuestSixAxisSensorHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", "guest six-axis sensors: none (no guest)",
          int, h_stpl_SixHandles, void *h, int n, const void *id, uint32_t style) { (void)h; (void)n; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn10streamplay27GetGuestSixAxisSensorStatesEPNS0_23GuestSixAxisSensorStateEiRKNS0_24GuestSixAxisSensorHandleE", "guest six-axis states: none (no guest)",
          int, h_stpl_SixStates, void *out, int n, const void *h) { (void)out; (void)n; (void)h; return 0; }
HLE_FORCE("_ZN2nn10streamplay30GetGuestVibrationDeviceHandlesEPNS0_26GuestVibrationDeviceHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", "guest vibration devices: none (no guest)",
          int, h_stpl_VibHandles, void *h, int n, const void *id, uint32_t style) { (void)h; (void)n; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn10streamplay27GetGuestVibrationDeviceInfoEPNS_3hid19VibrationDeviceInfoERKNS0_26GuestVibrationDeviceHandleE", "guest vibration device info: none (no guest)",
          void, h_stpl_VibInfo, uint64_t *info, const void *h) { (void)h; *info = 0; }
HLE_FORCE("_ZN2nn10streamplay18GetGuestJcmsHandleERKNS0_10GuestPadIdERKNS_3hid8JcmsTypeE", "guest Joy-Con mouse handle: none (no guest)", uint32_t, h_stpl_JcmsHandle, const void *id, const void *type) { (void)id; (void)type; return 0; }
HLE_FORCE("_ZN2nn10streamplay18GetGuestJcmsStatesEPNS0_14GuestJcmsStateEiRKNS0_15GuestJcmsHandleE", "guest Joy-Con mouse states: none (no guest)",
          int, h_stpl_JcmsStates, void *out, int n, const void *h) { (void)out; (void)n; (void)h; return 0; }

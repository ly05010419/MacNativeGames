// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include <stdatomic.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
// The overload with the layer's size (SDL3's Switch video driver, Absolum: 1920x1080 docked); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0xe4).
// An NVN or Vulkan window's textures decide what is presented; an EGL window surface takes the layer's size (dm/gfx/gl/hle_gl.m: Digimon Story Cyber Sleuth sizes its layer
// itself and draws into the surface at that size)
static _Atomic uint32_t g_layer_wh;   // w | h << 16 of the last sized CreateLayer, 0 = none
void dm_vi_layer_size(int *w, int *h) { uint32_t v = g_layer_wh; *w = (int)(v & 0xffff); *h = (int)(v >> 16); }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer of a given size; the host returns a static object", Result, h_vi_CreateLayerWH, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_layer;
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000) g_layer_wh = (uint32_t)w | (uint32_t)h << 16;
    return RES_OK;
}
// SDK 22's overload with a LayerCreationSettings, which its constructor LayerCreationSettings(int w, int h) fills as {w, h, 2} (sdk+0x4526a0 in EA SPORTS FC 27; the SDK's own
// constructor runs). The SDK's CreateLayer goes to am::GetSelfController(), which the host never made (SIGSEGV at the load from it, sdk+0x44ea94)
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", "create layer from creation settings (SDK 22); the host returns a static object",
          Result, h_vi_CreateLayerSettings, void **out, void *disp, const int32_t *settings) {
    return h_vi_CreateLayerWH(out, disp, settings ? settings[0] : 0, settings ? settings[1] : 0);
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the present: the Vulkan present (hle_vk.m), where Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and
// crops the layer to that every frame; and the NVN present when the game sets no window crop of its own (nvnWindowSetCrop; present_crop in hle_nvn.c), where Link's Awakening
// draws 1280x720 into its 1920x1080 window textures in handheld mode and crops the layer to that. Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: none kept, 0 entries", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)out; (void)layer; static int once; if (!once++) LOG("vi::ListFrameHistory (%d entries asked): the host keeps no frame history, 0 returned", n);
    return 0;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; the host has one, \"Default\"", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, 1920, 1080}; memcpy(p + 0x48, lim, sizeof lim);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// ---------- Switch 2 SDKs: HDR, VRR and extended swap intervals ----------
// The SDK's versions check the display against its own default display object and forward the layer to vi2, whose layer object the host never made (Donkey Kong Bananza: a call
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). The host's output is SDR at a fixed refresh rate: no HDR output, no VRR, and the layer metadata has
// nothing to apply to. HdrOutputFormat 0 is SDR; paper white is the BT.2408 reference, 203 nits, read only when HDR is on
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: SDR output (the host presents SDR)", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp; if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: 203 nits (BT.2408); HDR is off", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; if (nits) *nits = 203; return RES_OK; }
// The largest buffer the display takes: BufferMaxResolution 0 is 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160 (Fire Emblem Fortune's Weave's tables, main+0x1abc2c0 and +0x1abc050). The SDK's version
// reads the service the host does not serve and left 0, so the game chose 720p output, its DLSS input 640x360, even docked. A Switch 2 drives a 4K TV docked and has a 1080p screen
HLE_FORCE("_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", "the display's largest buffer: 4K in console mode, the 1080p screen in handheld mode", Result, h_vi_MaxRes, uint32_t *res, const void *disp) {
    (void)disp; if (res) *res = dm_opmode() == 1 ? 3 : 1; return RES_OK; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "display VRR: not supported (fixed refresh rate)", Result, h_vi_VrrSupported, uint8_t *on, const void *disp) {
    (void)disp; if (on) *on = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "display VRR capability: the host has no VRR", Result, h_vi_SetVrr, void *disp, unsigned on) {
    (void)disp; (void)on; return RES_OK; }
// The extended swap interval asks for a presentation rate between whole vsync intervals (VRR); without VRR the swap interval NVN sets stays in effect
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "layer extended swap interval: needs VRR, which the host has not; NVN's swap interval stays", Result, h_vi_ExtSwap,
          void *layer, int a, int b) {
    (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerExtendedSwapInterval %d %d: no VRR on the host, the NVN swap interval stays", a, b); return RES_OK; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "layer extended swap interval cleared", Result, h_vi_ExtSwapClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrGamut, void *layer, int g) {
    (void)layer; (void)g; return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrToneMap, void *layer, int m) {
    (void)layer; (void)m; return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; (void)lo; (void)hi; return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; (void)lo; (void)hi; return RES_OK; }

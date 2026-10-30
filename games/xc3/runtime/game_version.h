#pragma once
#define GAME_XC3 1
#define GAME_TITLE_ID 0x010074F013262000ULL
/* Inherited from games/ot0 (a game that reserves 62 GiB): the alias region above 2^39. Xenoblade Chronicles 3 asks for a
   39-bit address space; it boots and plays with the region kept, its allocations sitting at 0x90_...-0x91_.... */
#define GUEST_ALIAS_HIGH 1
/* Names the host files copied from games/totk (frontend.m, settings_panel.h, contrib_submit.h) use. */
#define GAME_NAME "Xenoblade Chronicles 3"
#define GAME_FOLDER "XC3"   /* ~/Library/Application Support/MacNativeGames/XC3 */
#define GAME_LOG "xc3"      /* ~/Library/Logs/MacNativeGames/xc3.log */
#define GAME_CONTRIB_TEMPLATE ""   /* no shared shader list for this game: the panel's contribution button is hidden */
#define GAME_CONTRIB_TITLE "Shader list (Xenoblade Chronicles 3)"
/* Per-version addresses, on purpose none by default (as in games/dread): an address left over from another game must not
   compile. Each one is added here, per version, once it is found and checked in this game's own code. */
#if defined(XC3_GAME_V100)
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "v100"
#elif defined(XC3_GAME_V221)
#define TOTK_GAME_VERSION "2.2.1"
#define TOTK_GAME_DIR "v221"
/* Render scale (stage 13): the renderer starts with its scene scale at 0.5 in one mode (the renderer object's +0x20b4)
   and 0.53 in the other (+0x20b8), set by mov/movk immediates at main+0xf5362c..0xf53650; the scene is drawn at output size x scale
   (960x544 of 1920x1080, rounded up to 8). XC3_RENDER_SCALE_SITES sets both to one chosen scale, a float whose low 16 bits
   are zero: {main offset, original word, 1 when its 16-bit immediate becomes the scale's upper half / 0 when it becomes 0}. */
#define XC3_RENDER_SCALE_SITES {{0xf5362c, 0xd2a7e00bu, 1},   /* mov  x11, #0x3f000000 (0.5, mode 1) */ \
                                {0xf53640, 0xf2d5c28bu, 0},   /* movk x11, #0xae14, lsl #32 (low half of 0.53) */ \
                                {0xf53644, 0xf2e7e0ebu, 1}}   /* movk x11, #0x3f07, lsl #48 (0.53, the other mode) */
#else
#error "build with -DXC3_GAME_V100 or -DXC3_GAME_V221 (the Makefile's VERSION)"
#endif

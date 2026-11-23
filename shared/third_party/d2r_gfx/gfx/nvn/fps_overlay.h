#pragma once
#include <stddef.h>
#include <stdint.h>

/* TOTK: frames per second, the game's output resolution and, in brackets, its internal (3D scene) resolution in its
   top-right corner, one line ("FPS 60  1920x1080 (1600x900)"; the top-left holds the hearts). The internal resolution was
   left out at first (user, 2026-10-16) and asked for on 2026-10-23. The game image itself (frame screenshots, texture dumps) never contains it. TOTK_FPS_OVERLAY=0 hides it. */
enum { FPS_OVERLAY_TEX_W = 768, FPS_OVERLAY_TEX_H = 64, FPS_OVERLAY_MAX_SCALE = 5 };

/* fps / screen size < 0 / 0: not known yet ("--"). mfx: MetalFX upscaled the picture in the last frames. gen > 0: MetalFX
   frame interpolation shows that many generated frames a second besides the game's fps frames -> "FPS 50+50" (key 2).
   big: the game itself outputs more than 1920x1080 (true 2K) -> " 2K" at the end; 2: native 4K; 3: native 5K. */
typedef struct { int fps, screen_w, screen_h, mfx, gen, big, scene_w, scene_h; } fps_overlay_info;   // screen: the output; scene: 0 = not known

int fps_overlay_enabled(void);
/* Shared user toggle for every in-picture HUD layer: FPS, shader collection and load telemetry. */
/* The settings panel (key 1): HUD_MODE_ALL (FPS, shader collection, load), HUD_MODE_MINIMAL (FPS only), HUD_MODE_OFF, or
   HUD_MODE_FULL, "FPS 全信息" (user 2026-11-16: "fps pannel 现在这么多信息 在设置里 新建一个 叫 fps 全信息"): ALL's lines and every
   other line of the corner panel (the hitches, shader libraries, pipelines made in play, archive misses, the other kinds,
   the rest). hud_line_shown says which line each mode shows. graphics-overlay.txt keeps the number.
   HUD_MODE_BARE, "极简 FPS" (user 2026-11-18: "只显示 fps和分辨率，放大3倍（只有这个 放大3倍）"): the FPS and the output
   resolution alone (fps_overlay_text leaves the rest of the line out), HUD_BARE_SCALE times the size: the corner panel's
   font, and in the picture's corner (TOTK_HUD=corners, iOS) as far as the line's texture allows (fps_overlay_scale). */
enum { HUD_MODE_OFF = 0, HUD_MODE_ALL = 1, HUD_MODE_MINIMAL = 2, HUD_MODE_FULL = 3, HUD_MODE_BARE = 4 };
enum { HUD_BARE_SCALE = 3 };
int fps_overlay_hud_mode(void);
int fps_overlay_hints_enabled(void);   /* the FPS readout shows (all or minimal) */
int fps_overlay_full_hud(void);        /* everything shows (all) */
void fps_overlay_set_hud_mode(int mode);   /* HUD_MODE_*; other values are ignored */
/* Pixel scale (1..FPS_OVERLAY_MAX_SCALE) for a game picture view_h pixels tall: one text line is about 3% of its height. */
int fps_overlay_scale(double view_h);
/* The line in a 5x7 pixel font on a dark box, as BGRA8 pixels in out (FPS_OVERLAY_TEX_W x FPS_OVERLAY_TEX_H,
   row stride FPS_OVERLAY_TEX_W * 4). Returns the box size in pixels. */
void fps_overlay_raster(const fps_overlay_info *info, int scale, uint8_t *out, int *box_w, int *box_h);
void fps_overlay_text(const fps_overlay_info *info, char *out, size_t n);   /* "FPS 97  2560x1440  MetalFX on" */
/* Compact HUD (Mac default; TOTK_HUD=corners keeps the lines in the picture's corners): the FPS, shader, render pipeline
   (compact only) and load lines are
   not drawn into the picture but published here, and the window's corner panel (runtime/frontend.m) shows them with the
   shortcuts in one small box. */
/* HUD_LIB .. HUD_MISC (2026-11-16, user: "都要显示 不要有隐藏"): every kind the box used to leave out; a frontend draws the
   lines in HUD_ORDER. One that loops over the first four only still shows what it showed before. */
enum { HUD_FPS, HUD_SHADER, HUD_PSO, HUD_LOAD, HUD_LIB, HUD_PLAY, HUD_KINDS, HUD_MISC, HUD_MISS,
       HUD_HITCHES, HUD_HITCH0, HUD_HITCH1, HUD_HITCH2, HUD_LINES };
#define HUD_ORDER {HUD_FPS, HUD_HITCHES, HUD_HITCH0, HUD_HITCH1, HUD_HITCH2, HUD_SHADER, HUD_LIB, HUD_PSO, HUD_PLAY, HUD_MISS, \
                   HUD_KINDS, HUD_MISC, HUD_LOAD}
/* Every hitch on screen (user 2026-11-16: "我要对 每个卡顿 都有 可视化的 了解 不要隐藏"). hle_nvn.c's long-frame check (the
   frame > 20 ms and 1.4 x the recent mean, or the GPU > 16.7 ms and 1.3 x) notes each long frame here; the panel shows how
   many there were, how many of them took HITCH_SHOWN_MS or more (TOTK_HITCH_MS changes it), and the last HITCH_KEEP of
   those, each split into what the log's long-frame line measures: pipelines + shader libraries made, textures uploaded,
   the rest of the submit and present not split further, and the GPU time. */
enum { HITCH_KEEP = 3 };
#define HITCH_SHOWN_MS 50.0
typedef struct {
    double at_s, frame_ms, submit_ms, gpu_ms, make_ms, upload_ms, upload_mb;
    unsigned pipelines, libraries, uploads, reuploads;
    int hidden;   // behind the loading screen: the player saw the loading screen stay a little longer, no motion stopped
} hitch_info;
void hitch_overlay_note(const hitch_info *h, uint64_t now_ns);
void hitch_text(char *out, size_t n, const hitch_info *h);          /* one hitch's line */
void hitch_count_text(char *out, size_t n, uint64_t all, uint64_t shown, uint64_t hidden, double shown_ms);
void hitch_overlay_publish(uint64_t now_ns);   /* the panel's hitch lines, yellow for 3 s after a new hitch */
enum { HUD_TEXT_MAX = 192 };   // bytes of one panel line with its NUL (a Chinese character is 3; the hitch lines need ~150)
int hud_line_shown(int line);   /* the corner panel shows this HUD_* line in the current HUD_MODE_* */
int hud_compact(void);
void hud_publish(int line, const char *text, int highlight);
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* 0: the line was never published (hidden or not known yet) */
/* Any one line (at most 80 characters of the 5x7 font) the same way; highlight draws it yellow instead of green. The _tex
   form rasterises into a texture of tex_w x tex_h pixels (row stride tex_w * 4); the scale drops until the line fits. */
void fps_overlay_raster_line(const char *text, int highlight, int scale, uint8_t *out, int *box_w, int *box_h);
void fps_overlay_raster_line_tex(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h);
/* The bottom-right load line (totk_sysload.c) is wider than the other two: its texture. */
enum { LOAD_OVERLAY_TEX_W = 2048 };   // the widest line (every value 100%) at scale 4 (1440p)
int fps_overlay_has_glyph(char c);
int fps_overlay_line_pixel_font(const char *text);   // 0: the line has a character the pixel font lacks (drawn with CoreText)
/* TOTK automatic shader collection, bottom-left box (2026-10-16 night): "SHADER +<new>/<in the cache> PSO +<new pipelines>",
   yellow for 3 s after something new was collected. TOTK_SHADER_OVERLAY=0 hides it (default on on the Mac, off on iOS). */
int shader_overlay_enabled(void);
void shader_overlay_text(char *out, size_t n, uint64_t fresh, int64_t total, uint64_t pipelines);   /* total < 0: "--" (not counted yet) */
void pso_overlay_text(char *out, size_t n, uint64_t fresh, int64_t total);   /* "新渲染管线 N / 总计 M", the line under the shaders */
/* The lines that show the rest (2026-11-16). played < 0: "--" (not at the title yet). */
void lib_overlay_text(char *out, size_t n, uint64_t fresh, int64_t played, int64_t compiled);     /* "着色器库 本机新 N · 游玩中加载 M（现编 K）" */
void play_overlay_text(char *out, size_t n, int64_t built, int64_t archived, int64_t compiled);   /* "游玩中建管线 N（存档取 M · 现编 K）" */
void kinds_overlay_text(char *out, size_t n, uint64_t compute, uint64_t mesh, uint64_t tileread, uint64_t border);   /* "本机新 计算 … · 网格 … · 贴图读取 … · 边框 …" */
void misc_overlay_text(char *out, size_t n, uint64_t unlisted, int64_t fill_failed);   /* "不进清单的 HDR / 宿主管线 N · 存档填充失败 M" */
/* Pipeline archive misses (nvn_mtl.m par_lookup_note): at a draw, not archived / archived but refused by Metal; in the
   warm-up of the App's own lists (a packaging defect) and of this device's list (not prepared). */
void miss_overlay_text(char *out, size_t n, uint64_t draw_none, uint64_t draw_refused, uint64_t app_list, uint64_t device_list);
/* Record that the game's present number `ticket` reached the screen at now_s (seconds). The rate of game presents is
   re-measured every half second; fps_overlay_value() returns it, or -1 before the first measurement. */
void fps_overlay_note(uint64_t ticket, double now_s);
int fps_overlay_value(void);
/* The same for interpolated frames shown (n counts them); fps_overlay_gen_value() is that rate, or -1. */
void fps_overlay_note_gen(uint64_t n, double now_s);
int fps_overlay_gen_value(void);

/* What TotK's settings panel and window (frontend.m, settings_panel.h, copied from games/totk) call that has nothing behind
   it in Xenoblade Chronicles 3 yet. Each answer makes the panel show the row as one fixed value:
   - the output resolution: no mode can be picked until stage 13 finds XC3's own render size (the row shows "1080p");
   - the aspect ratio: TotK's ultrawide comes from its own patches (aspect_patch.h); XC3 stays 16:9;
   - totk_sr_*: TotK's StateReporting for Apple's performance tools (totk_state_report.m, off by default there). */
#include "runtime.h"

int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }

float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }

void totk_sr_start(const char *game, const char *version) { (void)game; (void)version; }
void totk_sr_picture(int key, int visible) { (void)key; (void)visible; }
void totk_sr_window(int key, int visible) { (void)key; (void)visible; }
void totk_sr_view(int w, int h, int display_hz, int fullscreen) { (void)w; (void)h; (void)display_hz; (void)fullscreen; }
void totk_sr_closing(void) {}
void totk_sr_settings(int open) { (void)open; }

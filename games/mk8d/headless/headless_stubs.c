#include "../runtime/runtime.h"
#include <stdio.h>

/* The loader probe never initializes graphics or the window. These definitions
   keep the probe independent of the Metal 4 SDK required by the full renderer. */
void *runtime_gfx_override(const char *name) { (void)name; return NULL; }
int runtime_gfx_fault(void *addr, int is_write, void *context) {
    (void)addr; (void)is_write; (void)context; return 0;
}
void runtime_gfx_probe(int negative) { (void)negative; runtime_fail("graphics unavailable in headless probe"); }
void runtime_gpu_path_probe(const char *which) { (void)which; runtime_fail("graphics unavailable in headless probe"); }
void runtime_vpool_probe(void) { runtime_fail("graphics unavailable in headless probe"); }
void *runtime_front_override(const char *name) { (void)name; return NULL; }
int runtime_front_focus(void) { return 1; }
void runtime_front_run(void (*callback)(void)) { callback(); }
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    (void)req; (void)out; (void)out_size; return 0;
}
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_front_request_close(void) { runtime_fail("window unavailable in headless probe"); }
void runtime_shortcut_help_toggle(void) {}
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    fprintf(stderr, "[odyssey] %s: %llu/%llu\n", stage,
            (unsigned long long)done, (unsigned long long)total);
}
void runtime_boot_status(const char *stage, const char *detail) {
    fprintf(stderr, "[odyssey] %s: %s\n", stage, detail);
}
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
void dm_gfx_anomdump(void) {}
void dm_gfx_gpucapture_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int v) { (void)name; (void)v; }
void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }

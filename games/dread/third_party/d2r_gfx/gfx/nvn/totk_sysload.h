// TOTK: system load for the bottom-right line (user, 2026-10-16 night: the load of every CPU core and of the GPU, on one
// line like the FPS counter). See totk_sysload.c.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SYSLOAD_MAX_CORES = 32 };
/* Percentages, -1 = not known. p / e: every performance / efficiency core in logical-CPU order. gpu_sys: the GPU's own
   utilisation as macOS reports it (the whole GPU: other apps and our keep-alive spin kernels included, so near 100 % while
   the game runs). gpu_game: the share of time the GPU spent on our command buffers (the game's rendering, MetalFX; not the
   keep-alive). */
/* thread_max: the busiest thread of this process, % of one core (-1 = not known yet); thread_name: its name (for the log). */
typedef struct { int np, ne, p[SYSLOAD_MAX_CORES], e[SYSLOAD_MAX_CORES], gpu_sys, gpu_game, thread_max; char thread_name[32]; } sysload_info;

int sysload_enabled(void);                       // TOTK_LOAD_OVERLAY=0 hides the line (Mac default on, iOS off)
void sysload_start(void);                        // starts the sampler thread once (every 0.5 s)
int sysload_get(sysload_info *out);              // 0 before the first sample
void sysload_note_gpu(double start_s, double end_s);   // one of our command buffers ran on the GPU (CACurrentMediaTime seconds)
/* "P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%  T 47%  GPU 99% (70%)" (T: the busiest thread) */
void sysload_text(char *out, size_t n, const sysload_info *in);
/* Busy time of command buffers that complete in order: the union of their [start, end) intervals. Returns what was added. */
typedef struct { double last_end, busy; } sysload_busy;
double sysload_busy_add(sysload_busy *b, double start_s, double end_s);

// TOTK runtime shader translation (totk_shader_rt.m).
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#include <stdint.h>
NSString *totk_rt_dir(void);
// A file of the translation cache by name: the cache folder, or on iOS the seed inside the app for the files it brings
// (read in place, never copied). totk_rt_list: every file name of both, sorted.
NSString *totk_rt_file(NSString *name);
NSArray<NSString *> *totk_rt_list(void);
// Current-game raw shader keys observed outside the bundled static inventory.
// Includes persisted earlier sessions and returns an immutable snapshot.
NSSet<NSString *> *totk_rt_observed_shader_keys(void);
// The warm-up scope: this game version's shader keys (the seed's shader-keys.json) plus the observed ones; nil = no valid
// scope, on iOS, or TOTK_WARM_ALL=1 (warm the whole cache).
NSSet<NSString *> *totk_rt_shader_scope(void);
// The variants of a shader already in the translation cache (or the seed), without translating anything; empty when none.
NSArray<NSString *> *totk_rt_known_variants(NSString *shader);
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
// nil on iOS when the variant is not translated yet (request recorded). may_defer (Mac, async shaders): instead of
// translating now, queue the translation in the background and return nil; once it is recorded, totk_rt_ready_gen()
// moves. Without may_defer a running background job of the same shader is waited for.
// may_defer 2: urgent, a draw further on cannot go without it: run now on the urgent services (a queued job of the shader too).
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local, int may_defer);
void totk_rt_submit_stats(uint64_t *wait_n, uint64_t *wait_ns, uint64_t *sync_n, uint64_t *sync_ns);   // submit-thread translation waits / on-the-spot translations (frame TSV)
void totk_rt_urgent_stats(uint64_t *n, uint64_t *ns);   // urgent jobs done and their time from queued to recorded
void totk_rt_service_prestart(void);   // start the submit thread's translator service in the background (Mac)
// Called on the background worker after a translation, before the variant is recorded: compile its Metal library there.
void totk_rt_set_ready_hook(void (*hook)(NSString *variant, int stage));
// The variant totk_rt_variant would return when the cache index already names one; nil otherwise (never translates).
NSString *totk_rt_variant_cached(NSString *shader, NSDictionary *answers);
NSString *totk_rt_portable_variant(NSString *name);   // "<variant>_b" as another player's build names it (pipeline contributions)
int totk_rt_contrib_on(void);                       // a contribution file is kept (TOTK_SHADER_CONTRIB set, seed on)
void totk_rt_contrib_line(NSString *line);           // one row into the contribution file (totk_shader_contrib.h)
void totk_rt_probe_index(NSString *shader, NSArray<NSDictionary *> *rows);   // probe-gfx only
#endif
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);
// Distinct shader variants this run needed and the cache did not have (the iPhone, which translates nothing).
uint64_t totk_rt_missed(void);
uint64_t totk_rt_ready_gen(void);   // moves each time a background translation has been recorded
int totk_rt_can_translate(void);   // 0 on iOS and with TOTK_SHADER_NO_TRANSLATE=1

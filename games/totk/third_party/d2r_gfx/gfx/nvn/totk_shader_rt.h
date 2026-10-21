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
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local);   // nil on iOS when the variant is not translated yet (request recorded)
#endif
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);
/* Runtime translations done through the service this run and their cumulative wall time on the calling thread
   (CLOCK_UPTIME_RAW ns). Only real translations count, not cache lookups. Either pointer may be NULL. */
void totk_rt_xlate_stats(uint64_t *count, uint64_t *ns);

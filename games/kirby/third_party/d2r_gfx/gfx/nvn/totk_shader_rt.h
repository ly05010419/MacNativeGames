// TOTK runtime shader translation (totk_shader_rt.m).
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#include <stdint.h>
NSString *totk_rt_dir(void);
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
// The variants of a shader already in the translation cache (or the seed), without translating anything; empty when none.
NSArray<NSString *> *totk_rt_known_variants(NSString *shader);
// may_defer (Mac, async shaders): queue the translation in the background and return nil; totk_rt_completed() moves when
// it is recorded. Without may_defer a queued job is taken back and translated now, and a running one is waited for.
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local, int may_defer);   // nil on iOS when the variant is not translated yet (request recorded)
#endif
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
/* TOTK_SHADER_ASYNC: background translations finished so far (0 otherwise); a missing variant is worth asking for
   again once this has moved. */
uint64_t totk_rt_completed(void);
int totk_rt_async_on(void);   // background translation is on (Mac; TOTK_SHADER_ASYNC, default on for Odyssey)
void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);

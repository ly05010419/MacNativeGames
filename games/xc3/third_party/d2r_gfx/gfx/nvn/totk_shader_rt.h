// TOTK runtime shader translation (totk_shader_rt.m).
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#include <stdint.h>
NSString *totk_rt_dir(void);
int totk_rt_fix_bindless(NSString *base);
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
NSString *totk_rt_vtg(NSString *gs_shader, NSString *part, const uint8_t *vs_ctl, const uint8_t *vs_code, const uint8_t *gs_ctl, const uint8_t *gs_code, NSString *topology, NSDictionary *answers);   // geometry as compute
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local);   // nil on iOS when the variant is not translated yet (request recorded)
void totk_rt_set_ready_hook(void (*hook)(NSString *variant, int stage));
void totk_rt_prefetch(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local);
void totk_rt_prefetch_answers(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local, NSDictionary *answers);
#endif
uint64_t totk_rt_ready_gen(void);
void totk_rt_set_async_allowed(int allowed);
int totk_rt_async_on(void);
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);

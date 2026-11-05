// the earlier port runtime shader translation (psv_shader_rt.m).
#pragma once
#include <string.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#include <stdint.h>
NSString *psv_rt_dir(void);
/* A substring in a file's bytes. macOS memmem compares at every position (2026-11-04: 9% of the shader warm-up); memchr
   for one rarer character of the needle (anchor, its index) is vectorised, and only its hits are compared. */
static inline int psv_has_bytes(const void *hay, size_t n, const char *needle, size_t anchor) {
    size_t m = strlen(needle); if (m == 0 || m > n) return m == 0;
    const char *p = (const char *)hay, *end = p + n - (m - anchor - 1); char a = needle[anchor];
    for (const char *q = p + anchor; q < end && (q = memchr(q, a, (size_t)(end - q))); q++)
        if (!memcmp(q - anchor, needle, m)) return 1;
    return 0;
}
int psv_rt_fix_bindless(NSString *base);
int psv_rt_fix_bindless_data(NSString *base, NSData *have);
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
NSString *psv_rt_vtg(NSString *gs_shader, NSString *part, const uint8_t *vs_ctl, const uint8_t *vs_code, const uint8_t *gs_ctl, const uint8_t *gs_code, NSString *topology, NSDictionary *answers);   // geometry as compute
NSString *psv_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local);   // nil on iOS when the variant is not translated yet (request recorded)
void psv_rt_set_ready_hook(void (*hook)(NSString *variant, int stage));
void psv_rt_prefetch(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local);
void psv_rt_prefetch_answers(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local, NSDictionary *answers);
#endif
uint64_t psv_rt_ready_gen(void);
void psv_rt_set_async_allowed(int allowed);
int psv_rt_async_on(void);
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
void psv_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);

#pragma once
#import <Foundation/Foundation.h>
#include "nvn_refl.h"
// Zero means unproved. Bits name reflection entries containing handle words.
NSDictionary *nvn_pool_sources(nvn_refl *rf, NSArray *bindings, NSString *source);
// The source has a pooled texture read, the only case nvn_pool_sources has anything to prove (and its whole cost).
BOOL nvn_pool_sources_pooled(const nvn_refl *rf, NSArray *bindings, NSString *source);
// The proof made once (2026-11-18): a row of build/pool_sources.tsv and back. decode sets the bindings' pool_sources as
// nvn_pool_sources does and returns its result, or nil for a row it cannot read (the caller proves it again).
NSString *nvn_pool_sources_encode(const nvn_refl *rf, NSDictionary *result);
NSDictionary *nvn_pool_sources_decode(nvn_refl *rf, NSString *text);
// Storage bindings the MSL stores through get rw (moved from nvn_mtl.m: the packaging tool marks them as the runtime does).
void nvn_refl_mark_stores(nvn_refl *rf, NSArray *bindings, NSString *source);
// Index programs contain only proved scalar integer operations and uniform
// loads. Branches are unions, never guessed. Failure means scan the full input.
typedef int (*nvn_pool_uniform_read)(void *context, int binding, uint32_t offset, uint32_t *value);
int nvn_pool_index_values(NSArray *programs, nvn_pool_uniform_read read, void *context, uint32_t *values, int capacity);

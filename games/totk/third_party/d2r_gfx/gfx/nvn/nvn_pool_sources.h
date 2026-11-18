#pragma once
#import <Foundation/Foundation.h>
#include "nvn_refl.h"
// Zero means unproved. Bits name reflection entries containing handle words.
NSDictionary *nvn_pool_sources(nvn_refl *rf, NSArray *bindings, NSString *source);
// Index programs contain only proved scalar integer operations and uniform
// loads. Branches are unions, never guessed. Failure means scan the full input.
typedef int (*nvn_pool_uniform_read)(void *context, int binding, uint32_t offset, uint32_t *value);
int nvn_pool_index_values(NSArray *programs, nvn_pool_uniform_read read, void *context, uint32_t *values, int capacity);

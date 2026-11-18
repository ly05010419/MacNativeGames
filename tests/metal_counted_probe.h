#pragma once
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_counted.h"

// Standalone GPU probes compile the real shaders and pipelines without the runtime's frame counters.
id<MTLLibrary> mtl_counted_library(id<MTLDevice> dev, NSString *source, MTLCompileOptions *options, NSError **error) {
    return [dev newLibraryWithSource:source options:options error:error];
}

id<MTLRenderPipelineState> mtl_counted_render(id<MTLDevice> dev, MTLRenderPipelineDescriptor *descriptor, NSError **error) {
    return [dev newRenderPipelineStateWithDescriptor:descriptor error:error];
}

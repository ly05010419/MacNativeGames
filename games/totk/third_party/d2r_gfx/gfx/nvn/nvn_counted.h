// The Metal libraries and pipelines the graphics layer makes outside the game's own shader paths (our passes, the
// geometry stage, format converters, diagnostics): each is counted with the frame's libraries / pipelines (nvn_mtl.m
// g_bp_lib_* / g_bp_pso_*, the long-frame line's "新建管线 / 着色器库" and the per-frame record's @lib_* / @pso_*), as the
// game's are where shader_get, pso_get, the dispatch and mtk_pso call them. User 2026-11-15: "确定 着色器和管线 统计 … 都全
// 没有出现 落下的 没有统计的" (mesh pipelines, the geometry stage and our passes had been left out).
// tests/test_pipeline_stats_complete.py fails on any other Metal library or pipeline call outside the game paths and probes.
// Defined in nvn_mtl.m.
#pragma once
#import <Metal/Metal.h>

id<MTLLibrary> mtl_counted_library(id<MTLDevice> dev, NSString *source, MTLCompileOptions *options, NSError **error);
id<MTLRenderPipelineState> mtl_counted_render(id<MTLDevice> dev, MTLRenderPipelineDescriptor *descriptor, NSError **error);
id<MTLComputePipelineState> mtl_counted_compute(id<MTLDevice> dev, id<MTLFunction> function, NSError **error);

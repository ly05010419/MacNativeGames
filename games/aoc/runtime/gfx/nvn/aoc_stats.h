#pragma once
#include <stdint.h>
enum { AOC_STATS_SAMPLES = 240 };
typedef struct {
    uint64_t inventory, libraries, render_pipelines, compute_pipelines;
    uint64_t library_loads, pipeline_builds, load_errors, pipeline_errors;
    uint64_t warm_libraries, warm_render, warm_compute;
    double library_ms, pipeline_ms;
    int warming;
} AocShaderStats;
typedef struct {
    double game_fps, display_fps, frame_ms, p50_ms, p95_ms, p99_ms, gpu_ms;
    double history[AOC_STATS_SAMPLES];
    unsigned samples;
    int active, display_active, target;
    AocShaderStats shaders;
} AocStats;
// Collection is off initially. Enabling/disabling clears old timing samples.
void aoc_stats_enable(int enabled);
int aoc_stats_enabled(void);
void aoc_stats_frame(uint64_t now_ns, double gpu_ms, int target);
void aoc_stats_display(double presented_seconds, uint64_t now_ns);
void aoc_stats_resources(AocShaderStats shaders);
AocStats aoc_stats_snapshot(uint64_t now_ns);

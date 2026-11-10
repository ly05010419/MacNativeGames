// AoC's measured KIDS-task clock, independent of presentation and shader work.
#pragma once
#include <stdint.h>
#define AOC_TIMING_ABI 3
enum { AOC_DYNAMIC = 0x38, AOC_DT = 0x3c, AOC_SECONDS = 0x40,
       AOC_UPDATES = 0x48, AOC_MAX_DT = 0x50, AOC_MIN_DT = 0x54,
       AOC_COUNTER_SCALE = 0x60, AOC_SCENE_TICKS = 0x68, AOC_SCENE_DT = 0x70,
       AOC_SCENE_ACTIVE = 0x74, AOC_SCENE_UPDATES = 0x78, AOC_SCENE_SECONDS = 0x80 };
int aoc_timing_valid(int target);
void aoc_timing_apply(int profile_target);
void aoc_timing_set(int target, int dynamic);
int aoc_timing_target(void); // 0: original; otherwise 30 or 60; legacy 120 falls back to 60
int aoc_timing_dynamic(void);
void aoc_timing_clock(double *seconds, uint64_t *updates);
void aoc_timing_scene_clock(double *seconds, uint64_t *updates);

#pragma once
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do {fprintf(stderr,"[totk-thread] " __VA_ARGS__);fputc('\n',stderr);}while(0)
#define die(...) do {LOG(__VA_ARGS__);runtime_fail("thread runtime failure");}while(0)
#define now_ns runtime_now_ns
#define guest_ns runtime_now_ns
#define guest_sym runtime_symbol
#define guest_thread_init runtime_thread_init
static inline int dm_detclock_enabled(void) {return 0;}
void dm_thread_enter(uint64_t entry,uint64_t arg,uint64_t sp,uint64_t *host_context);

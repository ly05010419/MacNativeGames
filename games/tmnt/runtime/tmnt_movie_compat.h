#pragma once
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#define LOG(...) do { fprintf(stderr,"[tmnt-movie] "); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
#define die(...) do { LOG(__VA_ARGS__); runtime_fail("experimental movie decoder"); } while(0)
typedef struct { const char *name; void *fn; } TmntMovieEntry;
#define HLE_FORCE(m, why, ret, fn, ...) static ret fn(__VA_ARGS__); __attribute__((used,section("__DATA,tmntmovie"))) static const TmntMovieEntry tmnt_ent_##fn = {m, (void*)fn}; static ret fn(__VA_ARGS__)
#define guest_sym runtime_symbol
static inline uint64_t now_ns(void) { return runtime_now_ns(); }
static inline int dm_host_mutex_lock(pthread_mutex_t *m) { int r=pthread_mutex_trylock(m); if (r != EBUSY) return r; int away=runtime_core_away(); r=pthread_mutex_lock(m); runtime_core_back(&away); return r; }

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define LOG(...) do {fprintf(stderr,"[totk-gfx] " __VA_ARGS__);fputc('\n',stderr);}while(0)
#define die(...) do {LOG(__VA_ARGS__);exit(2);}while(0)
typedef struct {const char *name;uint8_t *base;size_t size;} module_t;
module_t *module_of(const void *p);
uint64_t now_ns(void);uint64_t guest_ns(void);
int dm_detclock_enabled(void);
void dm_fault_hook_register(int (*fn)(void *,int,void *));
void dm_guest_write_prep_register(void (*fn)(void *,size_t));
void dm_res_report_register(void (*fn)(uint32_t,const char *));
void dm_guest_write_prepare(void *addr,size_t n);
void crash_set_wait_dump(void (*fn)(void));
void dm_guest_frame_tick(void);
void dm_shot_written(const char *path,int ok);

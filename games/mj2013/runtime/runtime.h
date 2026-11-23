/* 明星三缺一 2013 (mj2013) on the Zelda-line runtime: a libnx homebrew (no Nintendo SDK, no NVN). The shared files
   (threads.c, ipc.c, hle_stack.c, gate.S, thread_enter.S, input_file.h) include this header, so it declares what they
   use; the rest is this port's own host (loader.c, svc_memory.c, the hb_*.c services, frontend). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct {
 const char *name, *digest; size_t file_size, size;
 uint64_t text_end, ro_off, data_off, patch_off, key_off;
 const Gate *gates; unsigned ngate;
 uint8_t *base;
} Module;

extern pthread_key_t runtime_keys[8];
_Noreturn void runtime_fail(const char *message);
void *runtime_symbol(const char *name);              /* no symbols in a homebrew: always NULL */
uint64_t runtime_now_ns(void);
void runtime_thread_init(void);
void runtime_dispatch(uint64_t ret, uint64_t *regs);
void runtime_gate(void);
uint64_t runtime_svc(unsigned num, uint64_t *regs, const char *module, uint64_t offset);
int runtime_guest_code_pc(uint64_t pc);
void runtime_trace_guest(uint64_t fp, uint64_t lr);
void runtime_describe_address(uint64_t a, char *out, size_t n);
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
uint8_t *runtime_module_base(const char *name);
uint64_t runtime_image_bytes(void);
int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission, unsigned *attribute);
uint64_t runtime_module_permission(uint64_t address, uint64_t size, unsigned permission);
uint64_t runtime_module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute);

/* threads.c (shared) */
uint32_t svc_thread_main_init(void);
int svc_thread(int num, uint64_t *regs);
void runtime_hle_stack_init(uint64_t tsd_offset);
void runtime_hle_stack_set(uintptr_t top);
void *runtime_hle_wrap(void *fn);
void *runtime_hle_wrap_named(void *fn, const char *name);
void runtime_hle_trace_set(int mode);
void runtime_hle_describe(unsigned index, char *out, size_t cap);
void runtime_thread_note_hle(unsigned index);
uint32_t runtime_thread_current_handle(void);
void runtime_dump_threads(void);
int runtime_core_away(void);
void runtime_core_back(int *away);
void runtime_core_checkpoint(void);
void runtime_spin_wait(unsigned module, uint64_t head);
void hle_bind_system_event(void *event, int autoclear);
void runtime_frame_step_pin(double ms);

/* svc_memory.c: the guest address space (heap, alias, stack, ASLR regions) and the memory SVCs */
int svc_memory(unsigned num, uint64_t *regs);
void *runtime_guest_map_shared(uint64_t addr, uint64_t size, void *host, int writable);   /* MapSharedMemory */
uint64_t runtime_heap_base(void);

/* hb_*.c: services the game reaches through sm (ipc.c registry) */
void hb_services_init(void);                         /* register every service (called once before boot) */
const char *runtime_profile_dir(void);               /* hb_fs.c: TOTK_PROFILE_DIR or the per-game data folder */

/* frontend: the presented frame and the input the window collects */
typedef struct { const uint8_t *rgba; unsigned width, height, stride; } FrontFrame;
void runtime_front_present(const uint32_t *rgba, unsigned width, unsigned height);   /* hb_display.c → window */
void runtime_front_run(void (*boot)(void));
void runtime_front_request_close(void);
void runtime_boot_status(const char *stage, const char *detail);
void runtime_input_key(unsigned code, int down);           /* a Mac key code (input.c) */
void runtime_input_key_synthetic(unsigned code, int down);
void runtime_input_touch(int active, float x, float y);    /* game pixels (1280x720) */
void runtime_input_clear(void);

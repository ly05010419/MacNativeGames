/* Helpers for the services a libnx homebrew reaches over IPC (hb_*.c). The framing is shared/runtime/ipc.c's. */
#pragma once
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The firmware this host reports (set:sys GetFirmwareVersion). 5.1.0: libnx then takes the older, simpler paths --
   CMIF sm (not TIPC, 12.0.0+), the time services without their shared memory (6.0.0+) -- all of which Horizon still
   answers today. */
#define HB_FW_MAJOR 5
#define HB_FW_MINOR 1
#define HB_FW_MICRO 0
#define HB_ARUID 0x51ULL                                  /* the applet resource user id handed out (the main thread's id) */

static inline void hb_session(IpcResponse *rs, const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    rs->move[rs->nmove++] = runtime_ipc_new_session(name, handler, state, close);
}
static inline void hb_copy(IpcResponse *rs, uint32_t handle) { rs->copy[rs->ncopy++] = handle; }
/* A readable event whose writable end the service keeps (and may signal). */
static inline uint32_t hb_event(uint32_t *writable_out) { uint32_t w = runtime_event_new(); if (writable_out) *writable_out = w; return runtime_event_readable(w); }
#define HB_OUT_U8(rs, v)  do { uint8_t  _v = (uint8_t)(v);  IPC_OUT(rs, _v); } while (0)
#define HB_OUT_U32(rs, v) do { uint32_t _v = (uint32_t)(v); IPC_OUT(rs, _v); } while (0)
#define HB_OUT_U64(rs, v) do { uint64_t _v = (uint64_t)(v); IPC_OUT(rs, _v); } while (0)

/* A client buffer the request sends (A: map alias, X: pointer) and one it receives (B: map alias, C: receive list),
   whichever of each pair is used (libnx's HipcAutoSelect sends both, one of them empty). */
static inline const IpcBuffer *hb_in_buffer(const IpcRequest *rq, unsigned i) {
    if (rq->na > i && rq->a[i].size) return &rq->a[i];
    if (rq->nx > i && rq->x[i].size) return &rq->x[i];
    return NULL;
}
static inline const IpcBuffer *hb_out_buffer(const IpcRequest *rq, unsigned i) {
    if (rq->nb > i && rq->b[i].size) return &rq->b[i];
    if (rq->nc > i && rq->c[i].size) return &rq->c[i];
    return NULL;
}
/* Copy n bytes into the received buffer (truncated to its size); returns the bytes written. */
static inline uint64_t hb_write_out(const IpcRequest *rq, unsigned i, const void *p, uint64_t n) {
    const IpcBuffer *b = hb_out_buffer(rq, i); if (!b) return 0;
    if (n > b->size) n = b->size;
    memcpy(ipc_guest(b->addr, n), p, n); return n;
}
/* A NUL-terminated string the client sent in a buffer (paths, device names), bounded by the buffer and by cap. */
static inline void hb_in_string(const IpcRequest *rq, unsigned i, char *out, size_t cap) {
    const IpcBuffer *b = hb_in_buffer(rq, i); out[0] = 0; if (!b || !cap) return;
    size_t n = b->size < cap - 1 ? (size_t)b->size : cap - 1;
    memcpy(out, ipc_guest(b->addr, n), n); out[n] = 0; out[strnlen(out, n)] = 0;
}
/* An unknown command: logged with its interface and stopped (a fake success would hide what the game needs). */
#define HB_UNKNOWN(s, rq) ipc_unimplemented((s), (rq))
#define HB_LOG(...) do { fprintf(stderr, "[hb] " __VA_ARGS__); fputc('\n', stderr); } while (0)

void hb_applet_register(void);
void hb_system_register(void);   /* set, set:sys, time:u, apm, psm, ... */
void hb_hid_register(void);
void hb_fs_register(void);
void hb_display_register(void);  /* vi, binder relay, nvdrv */

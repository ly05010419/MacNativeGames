#ifndef NVN_ENCODE_FIFO_H
#define NVN_ENCODE_FIFO_H
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* One producer, one consumer. Neither side reuses a slot until the other
   publishes ownership. Reset only with both threads outside the queue. */
enum { NVN_ENCODE_FIFO_CAP = 4096 };   /* 2026-10-16: 512 filled about 210 times per submission in the world (A then waits for B); 4096 absorbs the bursts */
typedef struct { uint64_t op, u[8]; } nvn_encode_packet;
typedef struct {
    _Alignas(64) _Atomic uint64_t published;
    _Alignas(64) _Atomic uint64_t consumed;
    _Alignas(64) uint64_t produced, cached_consumed;
    _Alignas(64) uint64_t read, cached_published;
    _Alignas(64) nvn_encode_packet packets[NVN_ENCODE_FIFO_CAP];
} nvn_encode_fifo;
static inline void nvn_encode_fifo_reset(nvn_encode_fifo *q) {
    atomic_store_explicit(&q->published, 0, memory_order_relaxed);
    atomic_store_explicit(&q->consumed, 0, memory_order_relaxed);
    q->produced = q->cached_consumed = q->read = q->cached_published = 0;
}
static inline int nvn_encode_fifo_push(nvn_encode_fifo *q, const nvn_encode_packet *p) {
    if (q->produced - q->cached_consumed == NVN_ENCODE_FIFO_CAP) {
        q->cached_consumed = atomic_load_explicit(&q->consumed, memory_order_acquire);
        if (q->produced - q->cached_consumed == NVN_ENCODE_FIFO_CAP) return 0;
    }
    q->packets[q->produced++ & (NVN_ENCODE_FIFO_CAP - 1)] = *p;
    return 1;
}
static inline void nvn_encode_fifo_publish(nvn_encode_fifo *q) {
    atomic_store_explicit(&q->published, q->produced, memory_order_release);
}
static inline const nvn_encode_packet *nvn_encode_fifo_peek(nvn_encode_fifo *q) {
    if (q->read == q->cached_published) {
        q->cached_published = atomic_load_explicit(&q->published, memory_order_acquire);
        if (q->read == q->cached_published) return NULL;
    }
    return &q->packets[q->read & (NVN_ENCODE_FIFO_CAP - 1)];
}
static inline void nvn_encode_fifo_consume(nvn_encode_fifo *q) {
    atomic_store_explicit(&q->consumed, ++q->read, memory_order_release);
}
#endif

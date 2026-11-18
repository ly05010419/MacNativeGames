#ifndef TMNT_SPLIT_TABLE_H
#define TMNT_SPLIT_TABLE_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { const void *base; void *token; } tmnt_split_slot;
typedef struct { tmnt_split_slot *slots; size_t count, cap; } tmnt_split_table;
typedef void *(*tmnt_split_resize_fn)(void *, size_t);
typedef void *(*tmnt_split_alloc_fn)(size_t, size_t);
/* A stable token names the most recent recording at a buffer/thread pair. Tokens
   must outlive any outstanding command handles, so normal teardown cannot free them. */
static inline int tmnt_split_table_get(tmnt_split_table *t, const void *base,
        tmnt_split_resize_fn resize, tmnt_split_alloc_fn alloc, void **out) {
    for (size_t i=0;i<t->count;i++) if (t->slots[i].base==base) { *out=t->slots[i].token; return 0; }
    if (t->count==t->cap) {
        size_t next=t->cap ? t->cap*2 : 16;
        if (next<t->cap || next>SIZE_MAX/sizeof *t->slots) return 1;
        tmnt_split_slot *slots=resize(t->slots,next*sizeof *slots);
        if (!slots) return 2;
        t->slots=slots; t->cap=next;
    }
    void *token=alloc(1,64);
    if (!token) return 3;
    t->slots[t->count++]=(tmnt_split_slot){base,token};
    *out=token;
    return 0;
}
/* Only call after every handle associated with this table has been retired. */
static inline void tmnt_split_table_destroy(tmnt_split_table *t, void (*release)(void *)) {
    for (size_t i=0;i<t->count;i++) release(t->slots[i].token);
    release(t->slots);
    *t=(tmnt_split_table){0};
}
static inline uintptr_t tmnt_split_control_position(const void *ctl, size_t used, size_t size) {
    return ctl && used<=size && size-used>=64 ? (uintptr_t)ctl+used : 0;
}
static inline uintptr_t tmnt_split_handle_or_copy(uintptr_t position, const void *copy) {
    return position ? position : (uintptr_t)copy;
}
#endif

/* Synthetic owned recordings. implementation.inc contains the real recorder,
   allocator, flatten, Call/Copy and typed command functions extracted by Python. */
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <malloc/malloc.h>
typedef struct NVNdevice NVNdevice;
typedef void *PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC;
typedef uint32_t NVNboolean;
typedef uint64_t NVNcommandHandle, NVNbufferAddress;
typedef int32_t NVNshaderStage, NVNdrawPrimitive, NVNindexType;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;
typedef struct {
    int32_t prim, itype, count, base_vertex, base_instance, instances;
    uint64_t ibuf, ind;
} p_draw_idx;
#define MAGIC(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b)<<8 | (uint32_t)(c)<<16 | (uint32_t)(d)<<24)
#define NVN(name,ret,...) static ret name(__VA_ARGS__)
#define LOG(...) ((void)0)
#define OP_RESERVE 24
#include "implementation.inc"

static NVNcommandHandle finish(NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, OP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    o->ctl_used += 64;
    return (uintptr_t)flatten_recording(o->seg);
}
static void init(cmd_t *c) {
    nvnCommandBufferInitialize(c, NULL);
    c->game_size = 4096;
    c->ctl_size = 512;
    c->ctl = (void *)(uintptr_t)0x100000;
    c->cmd_base = 0x200000;
}
static void release(NVNcommandHandle h) {
    copy_prefix *p = copy_of(h);
    assert(p->magic == COPY_MAGIC);
    g_copy_bytes -= malloc_size(p);
    p->magic = 0;
    free(p);
}
static void own(cmd_t *c) { nvnCommandBufferBindUniformBuffer(c, 0, 1, 0x3000, 64); }
static void draw(cmd_t *c, int count) { nvnCommandBufferDrawElements(c, 3, 2, count, 0x12340000); }
static uint64_t hash(const void *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *mode = argv[1];
    cmd_t child, parent, middle;
    init(&child); init(&parent); init(&middle);
    nvnCommandBufferBeginRecording(&child);
    if (strcmp(mode, "empty")) {
        int count = !strcmp(mode, "growth") ? 3000 : 1;
        for (int i = 0; i < count; i++) draw(&child, 6);
    }
    NVNcommandHandle source = finish(&child), mid = 0;
    int retired = !strcmp(mode, "retired_source");
    if (!strcmp(mode, "nested_call") || !strcmp(mode, "nested_copy") || retired) {
        nvnCommandBufferBeginRecording(&middle);
        nvnCommandBufferCallCommands(&middle, 1, &source);
        mid = finish(&middle);
        if (retired) {
            release(source);
            nvnCommandBufferBeginRecording(&child);
            draw(&child, 99);
            source = finish(&child);
        }
    }
    nvnCommandBufferBeginRecording(&parent);
    int threshold = !strcmp(mode, "threshold");
    for (int i = 0; i < (threshold ? 86 : 1); i++) own(&parent);
    uint64_t before = parent.hw_used, before_last = parent.last_rec;
    NVNcommandHandle handles[2] = {source, source};
    if (!strcmp(mode, "copy")) nvnCommandBufferCopyCommands(&parent, 1, &source);
    else if (!strcmp(mode, "multiple")) nvnCommandBufferCallCommands(&parent, 2, handles);
    else if (!strcmp(mode, "reuse") || threshold) {
        for (int i = 0; i < (threshold ? 8 : 2); i++) nvnCommandBufferCallCommands(&parent, 1, &source);
    } else if (!strcmp(mode, "mixed")) {
        nvnCommandBufferCallCommands(&parent, 1, &source);
        nvnCommandBufferCopyCommands(&parent, 1, &source);
    } else if (!strcmp(mode, "zero")) nvnCommandBufferCallCommands(&parent, 0, NULL);
    else if (!strcmp(mode, "nested_copy")) nvnCommandBufferCopyCommands(&parent, 1, &mid);
    else if (mid) nvnCommandBufferCallCommands(&parent, 1, &mid);
    else nvnCommandBufferCallCommands(&parent, 1, &source);
    uint64_t after = parent.hw_used, after_last = parent.last_rec, ctl = parent.ctl_used;
    if (!strcmp(mode, "next_op")) own(&parent);
    if (!strcmp(mode, "bare_fix")) hw_fix(&parent, 12);
    NVNcommandHandle out = finish(&parent);
    unsigned draws = 0;
    int last_count = 0;
    for (const uint8_t *p = (const void *)(uintptr_t)out;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op == OP_END) break;
        if (h.op == OP_DRAW_IDX) {
            p_draw_idx d;
            memcpy(&d, p + 8, sizeof d);
            draws++; last_count = d.count;
        }
        p += h.size;
    }
    if (retired) assert(draws == 1 && last_count == 6);
    printf("{\"before\":%llu,\"before_last\":%llu,\"after\":%llu,\"after_last\":%llu,"
           "\"hw\":%llu,\"logical_used\":%llu,\"host_used\":%llu,\"host_hash\":%llu,"
           "\"ctl_before_end\":%llu,\"ctl_after_end\":%llu,\"draws\":%u,\"last_draw_count\":%d}\n",
           (unsigned long long)before, (unsigned long long)before_last,
           (unsigned long long)after, (unsigned long long)after_last,
           (unsigned long long)parent.hw_used,
           (unsigned long long)nvnCommandBufferGetCommandMemoryUsed(&parent),
           (unsigned long long)parent.cmd_used,
           (unsigned long long)hash((const void *)(uintptr_t)out, copy_of(out)->body_bytes + 16),
           (unsigned long long)ctl, (unsigned long long)parent.ctl_used, draws, last_count);
    release(source);
    if (mid) release(mid);
    release(out);
    nvnCommandBufferFinalize(&parent);
    nvnCommandBufferFinalize(&middle);
    nvnCommandBufferFinalize(&child);
    assert(g_copy_bytes == 0);
    return 0;
}

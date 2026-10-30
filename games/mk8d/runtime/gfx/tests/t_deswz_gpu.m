#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../nvn/include/nvn.h"
#include "../nvn/nvn_tex.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"

typedef struct { const char *name; nvn_texdesc d; } test_case;
typedef struct { size_t off, pitch, bytes; unsigned level, layer, slices, rows; } record;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void fill_bytes(unsigned char *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 37u + (i >> 7)); }

static int run_case(id<MTLDevice> dev, id<MTLCommandQueue> queue, const test_case *tc, int special) {
    nvn_texlayout layout; nvn_tex_layout(&tc->d, &layout);
    size_t source_off = special ? 4 : 0;
    unsigned levels = tc->d.levels ? tc->d.levels : 1;
    unsigned count = 0; size_t dst_size = 32;
    for (unsigned l = 0; l < levels; l++) {
        uint32_t rb, rows, slices, bpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&tc->d, l, 0, &rb, &rows, &slices, &base, &th, &td, &bpb)) return 1;
        unsigned layers = layout.layers;
        for (unsigned layer = 0; layer < layers; layer++) {
            size_t pitch = rb + ((special && !count) ? 3 : 0);
            size_t bytes = (size_t)slices * rows * pitch;
            count++; dst_size += bytes + 7;
        }
    }
    record *records = calloc(count, sizeof(*records));
    unsigned char *expected = calloc(1, dst_size + 32), *source = calloc(1, source_off + layout.size + 32);
    if (!records || !expected || !source) return 1;
    size_t cursor = 4;
    unsigned ri = 0;
    for (unsigned l = 0; l < levels; l++) {
        uint32_t rb, rows, slices, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&tc->d, l, 0, &rb, &rows, &slices, &base, &th, &td, &bpb);
        for (unsigned layer = 0; layer < layout.layers; layer++) {
            size_t pitch = rb + ((special && ri == 0) ? 3 : 0), bytes = (size_t)slices * rows * pitch;
            cursor = (cursor + 3) & ~3ull;
            records[ri] = (record){cursor, pitch, (size_t)slices * rows * rb, l, layer, slices, rows};
            fill_bytes(expected + cursor, bytes, 17u + ri * 29u);
            if (nvn_swizzle(&tc->d, l, layer, expected + cursor, pitch, source + source_off)) return 1;
            cursor += bytes + 7; ri++;
        }
    }
    id<MTLBuffer> src = [dev newBufferWithBytes:source length:source_off + layout.size + 32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [dev newBufferWithLength:dst_size + 32 options:MTLResourceStorageModeShared];
    memset(dst.contents, 0xCD, dst.length);
    id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    int fail = 0;
    for (unsigned i = 0; i < count; i++)
        fail |= nvn_deswz_gpu_encode(enc, src, source_off, &tc->d, records[i].level, records[i].layer, dst, records[i].off, records[i].pitch) != 0;
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    for (unsigned i = 0; i < count; i++) {
        size_t row_bytes = records[i].bytes / (records[i].slices * records[i].rows);
        int ok = 1;
        for (unsigned z = 0; z < records[i].slices; z++)
            for (unsigned y = 0; y < records[i].rows; y++)
                ok &= !memcmp((unsigned char *)dst.contents + records[i].off + ((size_t)z * records[i].rows + y) * records[i].pitch,
                              expected + records[i].off + ((size_t)z * records[i].rows + y) * records[i].pitch, row_bytes);
        printf("%s L%u layer%u: %s %zu bytes\n", tc->name, records[i].level, records[i].layer, ok ? "OK" : "BAD", records[i].bytes);
        fail |= !ok;
    }
    free(records); free(expected); free(source);
    return fail;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("BAD: no Metal device\n"); return 1; }
        if (nvn_deswz_gpu_init(dev)) { printf("BAD: GPU deswizzle pipeline init failed\n"); return 1; }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        test_case cases[] = {
            {"RGBA8 1x1", {1, NVN_FORMAT_RGBA8, 0, 0, 1, 1, 1, 1, 0}},
            {"RGBA8 3x5", {1, NVN_FORMAT_RGBA8, 0, 0, 3, 5, 1, 1, 0}},
            {"RGBA8 64x64", {1, NVN_FORMAT_RGBA8, 0, 0, 64, 64, 1, 1, 0}},
            {"RGBA8 1000x700", {1, NVN_FORMAT_RGBA8, 0, 0, 1000, 700, 1, 1, 0}},
            {"RGBA8 2048x2048 mip", {1, NVN_FORMAT_RGBA8, 0, 0, 2048, 2048, 1, 12, 0}},
            {"R8 3", {1, NVN_FORMAT_R8, 0, 0, 3, 1, 1, 1, 0}},
            {"R8 17", {1, NVN_FORMAT_R8, 0, 0, 17, 1, 1, 1, 0}},
            {"RGBA16F", {1, NVN_FORMAT_RGBA16F, 0, 0, 17, 9, 1, 2, 0}},
            {"RGBA32F", {1, NVN_FORMAT_RGBA32F, 0, 0, 17, 9, 1, 2, 0}},
            {"BC1 30x18 mip", {1, NVN_FORMAT_RGBA_DXT1, 0, 0, 30, 18, 1, 4, 0}},
            {"BC3 30x18 mip", {1, NVN_FORMAT_RGBA_DXT3, 0, 0, 30, 18, 1, 4, 0}},
            {"BC7 30x18 mip", {1, NVN_FORMAT_BPTC_UNORM, 0, 0, 30, 18, 1, 4, 0}},
            {"2D array 5", {4, NVN_FORMAT_RGBA8, 0, 0, 17, 9, 5, 2, 0}},
            {"cube", {8, NVN_FORMAT_RGBA8, 0, 0, 17, 9, 1, 2, 0}},
            {"3D 64x64x16 mip", {2, NVN_FORMAT_RGBA8, 0, 0, 64, 64, 16, 7, 0}},
            {"packaged 3D byte 0x00 mip", {2, NVN_FORMAT_RGBA8, 0, 0, 32, 32, 32, 6, 0, 1}},
            {"packaged 3D byte 0x12 mip", {2, NVN_FORMAT_RGBA8, 0, 0, 32, 32, 32, 6, 0, 0x13}},
        };
        int fail = 0; unsigned n = 0;
        for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) { fail |= run_case(dev, queue, &cases[i], i == 1); n++; }

        test_case neg = {"negative control", {1, NVN_FORMAT_RGBA8, 0, 0, 64, 64, 1, 2, 0}};
        nvn_texlayout nl; nvn_tex_layout(&neg.d, &nl);
        size_t rb, expected_bytes = 64 * 64 * 4; rb = 64 * 4;
        unsigned char *lin = malloc(expected_bytes), *srcmem = calloc(1, nl.size + 32);
        fill_bytes(lin, expected_bytes, 91); nvn_swizzle(&neg.d, 0, 0, lin, rb, srcmem);
        id<MTLBuffer> sb = [dev newBufferWithBytes:srcmem length:nl.size + 32 options:MTLResourceStorageModeShared];
        id<MTLBuffer> db = [dev newBufferWithLength:expected_bytes options:MTLResourceStorageModeShared]; memset(db.contents, 0xCD, db.length);
        id<MTLCommandBuffer> ncb = [queue commandBuffer]; id<MTLComputeCommandEncoder> ne = [ncb computeCommandEncoder];
        int enc_bad = nvn_deswz_gpu_encode(ne, sb, 0, &neg.d, 1, 0, db, 0, rb); [ne endEncoding]; [ncb commit]; [ncb waitUntilCompleted];
        int mismatch = enc_bad || memcmp(db.contents, lin, expected_bytes) != 0; printf("negative control (wrong level): %s %zu bytes\n", mismatch ? "OK (flagged)" : "BAD (not flagged)", expected_bytes); fail |= !mismatch;
        free(lin); free(srcmem);

        test_case perf = {"performance reference", {1, NVN_FORMAT_RGBA8, 0, 0, 2048, 2048, 1, 12, 0}};
        nvn_texlayout pl; nvn_tex_layout(&perf.d, &pl); size_t perf_size = 0;
        for (unsigned l = 0; l < perf.d.levels; l++) { uint32_t pr, py, pz, pb; uint64_t pbase; unsigned pth, ptd; nvn_deswizzle_geom(&perf.d, l, 0, &pr, &py, &pz, &pbase, &pth, &ptd, &pb); perf_size += (size_t)pr * py; }
        unsigned char *ps = calloc(1, pl.size), *pd = calloc(1, perf_size); size_t perf_cursor = 0;
        for (unsigned l = 0; l < perf.d.levels; l++) { uint32_t pr, py, pz, pb; uint64_t pbase; unsigned pth, ptd; nvn_deswizzle_geom(&perf.d, l, 0, &pr, &py, &pz, &pbase, &pth, &ptd, &pb); fill_bytes(pd + perf_cursor, (size_t)pr * py, 7 + l); nvn_swizzle(&perf.d, l, 0, pd + perf_cursor, pr, ps); perf_cursor += (size_t)pr * py; }
        uint64_t c0 = now_ns(); perf_cursor = 0; for (unsigned l = 0; l < perf.d.levels; l++) { uint32_t pr, py, pz, pb; uint64_t pbase; unsigned pth, ptd; nvn_deswizzle_geom(&perf.d, l, 0, &pr, &py, &pz, &pbase, &pth, &ptd, &pb); nvn_deswizzle(&perf.d, l, 0, ps, pd + perf_cursor, pr); perf_cursor += (size_t)pr * py; } uint64_t cpu_ns = now_ns() - c0;
        id<MTLBuffer> psrc = [dev newBufferWithBytes:ps length:pl.size options:MTLResourceStorageModeShared];
        id<MTLBuffer> pdst = [dev newBufferWithLength:perf_size options:MTLResourceStorageModeShared];
        c0 = now_ns(); id<MTLCommandBuffer> pcb = [queue commandBuffer]; id<MTLComputeCommandEncoder> pe = [pcb computeCommandEncoder]; perf_cursor = 0; for (unsigned l = 0; l < perf.d.levels; l++) { uint32_t pr, py, pz, pb; uint64_t pbase; unsigned pth, ptd; nvn_deswizzle_geom(&perf.d, l, 0, &pr, &py, &pz, &pbase, &pth, &ptd, &pb); nvn_deswz_gpu_encode(pe, psrc, 0, &perf.d, l, 0, pdst, perf_cursor, pr); perf_cursor += (size_t)pr * py; } [pe endEncoding]; uint64_t encode_ns = now_ns() - c0; [pcb commit]; [pcb waitUntilCompleted]; uint64_t gpu_ns = now_ns() - c0;
        printf("performance reference: 2048x2048 RGBA8 with mips (about 22 MB), CPU nvn_deswizzle %.3f ms; GPU encode %.3f ms + waitUntilCompleted, total %.3f ms\n", cpu_ns / 1e6, encode_ns / 1e6, gpu_ns / 1e6); free(ps); free(pd);
        printf("== all %u cases pass / negative control flagged ==\n", n);
        return fail != 0;
    }
}

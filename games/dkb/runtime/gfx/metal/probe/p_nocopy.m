// Probe: create zero-copy MTLBuffers over game memory (boundaries expanded outward to 16 KiB pages, overlap allowed), with GPU writes / CPU reads visible to each other.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    size_t pg = getpagesize();
    uint8_t *mem; posix_memalign((void **)&mem, pg, 8 * pg);
    memset(mem, 0, 8 * pg);
    // Two "pools": A = [pg+0x1000, 3pg+0x800), B = [2pg+0x2000, 5pg), neither page-aligned, and after expanding outward they overlap on pages 2–3
    uint8_t *a0 = mem + pg + 0x1000, *a1 = mem + 3 * pg + 0x800, *b0 = mem + 2 * pg + 0x2000, *b1 = mem + 5 * pg;
    #define DOWN(p) ((uint8_t *)((uintptr_t)(p) & ~(uintptr_t)(pg - 1)))
    #define UP(p) ((uint8_t *)(((uintptr_t)(p) + pg - 1) & ~(uintptr_t)(pg - 1)))
    id<MTLBuffer> A = [dev newBufferWithBytesNoCopy:DOWN(a0) length:UP(a1) - DOWN(a0) options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> B = [dev newBufferWithBytesNoCopy:DOWN(b0) length:UP(b1) - DOWN(b0) options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> bad = [dev newBufferWithBytesNoCopy:a0 length:pg options:MTLResourceStorageModeShared deallocator:nil];
    printf("page size %zu; A %s B %s; buffer at an unaligned start %s\n", pg, A ? "ok" : "failed", B ? "ok" : "failed", bad ? "ok (not as expected)" : "failed (as expected)");
    // GPU: copy 4096 bytes from a0 in A to b0 in B (of the two blocks' landing points, one is outside and one is inside the overlapping pages)
    for (int i = 0; i < 4096; i++) a0[i] = (uint8_t)(i * 7 + 3);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromBuffer:A sourceOffset:a0 - DOWN(a0) toBuffer:B destinationOffset:b0 - DOWN(b0) size:4096];
    // Then within the overlap region: write through B, read through A - fill 256 bytes of B starting at b0+0x1000 with 0xAB
    [bl fillBuffer:B range:NSMakeRange(b0 - DOWN(b0) + 0x1000, 256) value:0xAB];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int ok1 = !memcmp(a0, b0, 4096), ok2 = 1;
    for (int i = 0; i < 256; i++) ok2 &= b0[0x1000 + i] == 0xAB;
    uint8_t *viaA = (uint8_t *)A.contents + (b0 + 0x1000 - DOWN(a0));
    int ok3 = viaA[0] == 0xAB && viaA[255] == 0xAB;
    printf("GPU copy A→B 4096 B identical: expected 1 → got %d\nGPU fill seen through the original CPU pointer: expected 1 → got %d\nsame bytes seen through A's contents: expected 1 → got %d\n", ok1, ok2, ok3);
    return !(A && B && !bad && ok1 && ok2 && ok3);
}

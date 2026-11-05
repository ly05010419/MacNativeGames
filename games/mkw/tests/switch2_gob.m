// Independent, fixed one-GOB fixture: linear 16-byte segments in Switch 2
// storage order. Covers CPU conversion, GPU upload, and GPU Read/WriteTexels.
// Build with -DGAME_MKW=1 and nvn_tex.c, nvn_deswizzle.c, nvn_deswizzle_gpu.m.
#import <Foundation/Foundation.h>
#import "../third_party/d2r_gfx/gfx/tex/nvn_deswizzle_gpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void completed(id<MTLCommandBuffer> cb) {
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "GPU conversion failed: %s\n", cb.error.description.UTF8String);
        abort();
    }
}

int main(void) { @autoreleasepool {
    static const uint8_t segments[32] = {
        0,4,16,20, 1,5,17,21, 2,6,18,22, 3,7,19,23,
        8,12,24,28, 9,13,25,29, 10,14,26,30, 11,15,27,31
    };
    uint8_t storage[512], expected[512], got[512];
    for (int i=0; i<512; i++) { storage[i]=(uint8_t)(i/16); expected[i]=segments[i/16]; }
    nvn_texdesc d={.target=1,.format=0x25,.width=16,.height=8,.depth=1,.levels=1};
    assert(!nvn_deswizzle(&d,0,0,storage,got,64));
    assert(!memcmp(got,expected,512));
    memset(got,0xff,512);
    assert(!nvn_swizzle(&d,0,0,expected,64,got));
    assert(!memcmp(got,storage,512));

    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    assert(dev && !nvn_deswz_gpu_init(dev));
    id<MTLCommandQueue> q=[dev newCommandQueue];
    id<MTLBuffer> pool=[dev newBufferWithBytes:storage length:512 options:MTLResourceStorageModeShared];
    id<MTLBuffer> linear=[dev newBufferWithLength:512 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[q commandBuffer];
    id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    assert(!nvn_deswz_gpu_encode(enc,pool,0,&d,0,0,linear,0,64));
    [enc endEncoding]; completed(cb);
    assert(!memcmp(linear.contents,expected,512));

    memset(linear.contents,0xff,512);
    cb=[q commandBuffer]; enc=[cb computeCommandEncoder];
    assert(!nvn_texels_gpu_encode(enc,pool,0,&d,0,0,0,0,0,64,8,1,linear,0,64,512,1));
    [enc endEncoding]; completed(cb);
    assert(!memcmp(linear.contents,expected,512));

    memset(pool.contents,0xff,512);
    cb=[q commandBuffer]; enc=[cb computeCommandEncoder];
    assert(!nvn_texels_gpu_encode(enc,pool,0,&d,0,0,0,0,0,64,8,1,linear,0,64,512,0));
    [enc endEncoding]; completed(cb);
    assert(!memcmp(pool.contents,storage,512));
    puts("Switch 2 GOB: CPU read/write, GPU upload/read/write passed (2,560 bytes checked)");
} return 0; }

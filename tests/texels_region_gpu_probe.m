/* Native counterpart of test_texels_region_layout.py. Link the production
 * nvn_deswizzle_gpu.m, nvn_deswizzle.c and nvn_tex.c; run only while holding the
 * repository's exclusive game slot. No proprietary texture data is needed. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_deswizzle_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long checks;
static void require(int ok,const char *message) {
    ++checks;
    if(!ok) { fprintf(stderr,"FAIL %s after %llu checks\n",message,checks);exit(1); }
}
static uint64_t address(uint32_t x,uint32_t y,uint32_t z,uint32_t row,uint32_t rows,unsigned th,unsigned td) {
    uint32_t bh=1u<<th,bd=1u<<td;
    uint64_t gx=(row+63u)/64u,by=(rows+8u*bh-1u)/(8u*bh);
    uint64_t tile=((z/bd)*by+y/(8u*bh))*gx+x/64u,gob=(z%bd)*bh+(y/8u)%bh;
    uint32_t bits[5];
#ifdef GAME_NS2
    bits[0]=y&1;bits[1]=(y>>1)&1;bits[2]=(x>>4)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1;
#else
    bits[0]=y&1;bits[1]=(x>>4)&1;bits[2]=(y>>1)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1;
#endif
    uint64_t within=x&15u;
    for(unsigned i=0;i<5;i++)within|=(uint64_t)bits[i]<<(i+4);
    return tile*(512ull*bh*bd)+gob*512ull+within;
}
static uint8_t pixel(unsigned x,unsigned y,unsigned z,unsigned layer,unsigned level) {
    return (uint8_t)((x*73u)^(y*151u)^(z*199u)^(x>>3)^(y>>2)^(layer*23u)^(level*41u));
}
static void complete(id<MTLCommandBuffer> cb) {
    [cb commit];[cb waitUntilCompleted];
    require(cb.status==MTLCommandBufferStatusCompleted,"GPU completion");
}
static void run(id<MTLDevice> dev,id<MTLCommandQueue> queue,nvn_texdesc desc,unsigned level,unsigned layer,int partial) {
    nvn_texlayout layout;nvn_tex_layout(&desc,&layout);
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
    require(nvn_deswizzle_geom(&desc,level,layer,&row,&rows,&slices,&base,&th,&td,&bpb)==0,"production geometry");
    unsigned x=partial&&row>80?61:0,y=partial&&rows>10?5:0,z=slices>1?1:0;
    unsigned w=partial&&row>80?19:row,h=partial&&rows>10?6:rows,d=slices-z;
    size_t pitch=partial?w+13:(w+31)&~15u,img=pitch*h+32,linear_off=partial?7:16,pool_off=32;
    size_t pool_size=pool_off+layout.size+32,linear_size=linear_off+img*d+32;
    id<MTLBuffer> pool=[dev newBufferWithLength:pool_size options:MTLResourceStorageModeShared];
    id<MTLBuffer> linear=[dev newBufferWithLength:linear_size options:MTLResourceStorageModeShared];
    require(pool&&linear,"buffers");
    uint8_t *expected_pool=malloc(pool_size),*expected_linear=malloc(linear_size);
    require(expected_pool&&expected_linear,"expected buffers");
    memset(pool.contents,0xcd,pool_size);memset(linear.contents,0xa5,linear_size);
    for(unsigned k=0;k<slices;k++)for(unsigned j=0;j<rows;j++)for(unsigned i=0;i<row;i++) {
        uint64_t a=pool_off+base+address(i,j,k,row,rows,th,td);
        require(a<pool_size,"oracle storage bounds");
        ((uint8_t *)pool.contents)[a]=pixel(i,j,k,layer,level);
    }
    memcpy(expected_pool,pool.contents,pool_size);memcpy(expected_linear,linear.contents,linear_size);
    for(unsigned k=0;k<d;k++)for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++)
        expected_linear[linear_off+k*img+j*pitch+i]=pixel(x+i,y+j,z+k,layer,level);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    require(nvn_texels_gpu_encode(enc,pool,pool_off,&desc,level,layer,x,y,z,w,h,d,linear,linear_off,pitch,img,1)==0,"regional read accepted");
    [enc endEncoding];complete(cb);
    require(memcmp(linear.contents,expected_linear,linear_size)==0,"regional read pixels/tails/padding");
    require(memcmp(pool.contents,expected_pool,pool_size)==0,"regional read pool untouched");
    for(unsigned k=0;k<d;k++)for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++) {
        uint8_t v=pixel(x+i,y+j,z+k,layer,level)^0x5b;
        ((uint8_t *)linear.contents)[linear_off+k*img+j*pitch+i]=v;
        expected_pool[pool_off+base+address(x+i,y+j,z+k,row,rows,th,td)]=v;
    }
    memcpy(expected_linear,linear.contents,linear_size);
    cb=[queue commandBuffer];enc=[cb computeCommandEncoder];
    require(nvn_texels_gpu_encode(enc,pool,pool_off,&desc,level,layer,x,y,z,w,h,d,linear,linear_off,pitch,img,0)==0,"regional write accepted");
    [enc endEncoding];complete(cb);
    require(memcmp(pool.contents,expected_pool,pool_size)==0,"regional write pixels and all pool canaries");
    require(memcmp(linear.contents,expected_linear,linear_size)==0,"regional write linear untouched");
    free(expected_pool);free(expected_linear);
}
int main(void) { @autoreleasepool {
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();if(!dev)return 77;
    require(nvn_deswz_gpu_init(dev)==0,"production pipelines compile");
    id<MTLCommandQueue> queue=[dev newCommandQueue];require(queue!=nil,"queue");
    nvn_texdesc astc={.target=4,.format=0x80,.width=1365,.height=1365,.depth=3,.levels=11};
    nvn_texdesc rgba={.target=4,.format=0x25,.width=260,.height=260,.depth=2,.levels=1};
    nvn_texdesc large_astc={.target=1,.format=0x7b,.width=1536,.height=1536,.depth=1,.levels=11};
    nvn_texdesc volume={.target=2,.format=0x25,.width=19,.height=19,.depth=5,.levels=1};
    for(unsigned partial=0;partial<2;partial++) {
        const unsigned levels[]={0,1,10};
        for(unsigned i=0;i<3;i++) { @autoreleasepool { run(dev,queue,astc,levels[i],2,partial); } }
        @autoreleasepool { run(dev,queue,rgba,0,1,partial); }
        @autoreleasepool { run(dev,queue,large_astc,0,0,partial); }
        @autoreleasepool { run(dev,queue,volume,0,0,partial); }
    }
    printf("PASS 12 production regional GPU cases, %llu checks\n",checks);return 0;
} }

/* Synthetic whole-texture upload and typed-sampling test. Link the production
 * nvn_deswizzle_gpu.m, nvn_deswizzle.c and nvn_tex.c. Run only in the exclusive
 * game slot. ASTC UNORM16 void-extent block encoding follows Arm astc-encoder's
 * symbolic_to_physical: https://github.com/ARM-software/astc-encoder/blob/main/Source/astcenc_symbolic_physical.cpp
 * No proprietary texture or shader is used. ASTC_ORACLE_X1 is a test-only
 * negative: source bytes use X1 while the GAME_NS2 production kernel stays NS2. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn_deswizzle_gpu.h"

typedef struct { uint32_t x,y,layer,mip; } Sample;
typedef struct { unsigned level,layer,w,h,row,rows; size_t offset; } Job;
static unsigned long long checks;
static uint64_t gpu_deadline;
static void require(int ok,const char *message) {
    ++checks;if(!ok){fprintf(stderr,"FAIL %s after %llu checks\n",message,checks);exit(1);}
}
static unsigned mip(unsigned size,unsigned level) { unsigned x=size>>level;return x?x:1; }
/* Independent one-GOB layout and bit-permutation oracle. Captured material
 * spans match this MINIMAL_LAYOUT case; packaged tile choices are not guessed. */
static uint64_t gob_address(unsigned x,unsigned y,unsigned row) {
    uint64_t gob=(uint64_t)(y/8)*((row+63)/64)+x/64,within=x&15;
    unsigned bits[5];
#if defined(GAME_NS2) && !defined(ASTC_ORACLE_X1)
    bits[0]=y&1;bits[1]=(y>>1)&1;bits[2]=(x>>4)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1;
#else
    bits[0]=y&1;bits[1]=(x>>4)&1;bits[2]=(y>>1)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1;
#endif
    for(unsigned i=0;i<5;i++)within|=(uint64_t)bits[i]<<(i+4);
    return gob*512+within;
}
static unsigned color(unsigned layer,unsigned level,unsigned bx,unsigned by,unsigned channel) {
    return 32+(layer*29+level*31+bx*(13+channel*4)+by*(7+channel*6)+channel*43)%192;
}
static void block(uint8_t bytes[16],unsigned layer,unsigned level,unsigned bx,unsigned by) {
    bytes[0]=0xfc;bytes[1]=0xfd;memset(bytes+2,0xff,6);
    for(unsigned c=0;c<4;c++){unsigned v=color(layer,level,bx,by,c)*257;bytes[8+c*2]=v&255;bytes[9+c*2]=v>>8;}
}
static double linear_srgb(double v) {return v<=0.04045?v/12.92:pow((v+0.055)/1.055,2.4);}
static const char *sample_msl=
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct S{uint x,y,layer,mip;};\n"
"kernel void inspect_array(texture2d_array<float,access::sample> t [[texture(0)]],constant S* s [[buffer(0)]],device float4* out [[buffer(1)]],uint i [[thread_position_in_grid]]) {\n"
" constexpr sampler p(coord::normalized,address::clamp_to_edge,filter::nearest,mip_filter::nearest);S a=s[i];float2 uv=(float2(a.x,a.y)+0.5f)/float2(t.get_width(a.mip),t.get_height(a.mip));out[i]=t.sample(p,uv,a.layer,level(float(a.mip)));}\n"
"kernel void inspect_2d(texture2d<float,access::sample> t [[texture(0)]],constant S* s [[buffer(0)]],device float4* out [[buffer(1)]],uint i [[thread_position_in_grid]]) {\n"
" constexpr sampler p(coord::normalized,address::clamp_to_edge,filter::nearest,mip_filter::nearest);S a=s[i];float2 uv=(float2(a.x,a.y)+0.5f)/float2(t.get_width(a.mip),t.get_height(a.mip));out[i]=t.sample(p,uv,level(float(a.mip)));}\n";

static void run_case(id<MTLDevice> dev,id<MTL4CommandQueue> queue,id<MTLResidencySet> residency,
                     id<MTLLibrary> library,unsigned width,unsigned block_size,int srgb) { @autoreleasepool {
    const unsigned levels=11,layers=width==1365?90:1;
    const unsigned selected[]={0,1,88,89},nselected=layers==90?4:1;
    nvn_texdesc desc={.target=layers==90?4:1,.format=(block_size==8?0x80:0x7b)+(srgb?14:0),.flags=0x200,
        .width=width,.height=width,.depth=layers,.levels=levels};
    uint64_t level_base[11],layer_size=0;
    for(unsigned l=0;l<levels;l++) {
        unsigned w=mip(width,l),blocks=(w+block_size-1)/block_size,row=blocks*16;
        level_base[l]=layer_size;layer_size+=(uint64_t)((row+63)/64)*((blocks+7)/8)*512;
    }
    require(layer_size==(width==1365?657920:2078720),"independent NPOT mip-chain span");
    nvn_texlayout layout;nvn_tex_layout(&desc,&layout);
    require(layout.layer_size==layer_size&&layout.size==layer_size*layers,"production minimal layout matches independent offsets");
    Job jobs[44];Sample samples[264];unsigned nj=0,ns=0;size_t staging_size=0;
    for(unsigned i=0;i<nselected;i++)for(unsigned l=0;l<levels;l++) {
        unsigned layer=selected[i],w=mip(width,l),blocks=(w+block_size-1)/block_size;
        staging_size=(staging_size+255)&~(size_t)255;
        jobs[nj++]=(Job){l,layer,w,w,blocks*16,blocks,staging_size};staging_size+=(size_t)blocks*blocks*16;
        unsigned positions[6][2]={{0,0},{w-1,w-1},{w/2,w/2},{MIN(block_size,w-1),MIN(block_size,w-1)},
            {MIN(2*block_size-1,w-1),MIN(3*block_size+1,w-1)},{w>1?w-2:0,w>1?1:0}};
        for(unsigned j=0;j<6;j++)samples[ns++]=(Sample){positions[j][0],positions[j][1],layer,l};
    }
    id<MTLBuffer> pool=[dev newBufferWithLength:layout.size options:MTLResourceStorageModeShared];
    id<MTLBuffer> staging=[dev newBufferWithLength:staging_size+32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> params=[dev newBufferWithLength:nj*256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> requests=[dev newBufferWithBytes:samples length:ns*sizeof(Sample) options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[dev newBufferWithLength:ns*16 options:MTLResourceStorageModeShared];
    require(pool&&staging&&params&&requests&&output,"test buffers");
    uint8_t *expected=malloc(staging.length);require(expected!=NULL,"expected staging");
    memset(pool.contents,0xcd,pool.length);memset(staging.contents,0xa5,staging.length);memset(expected,0xa5,staging.length);
    memset(output.contents,0xa5,output.length);
    for(unsigned i=0;i<nj;i++){Job j=jobs[i];
        uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
        require(nvn_deswizzle_geom(&desc,j.level,j.layer,&row,&rows,&slices,&base,&th,&td,&bpb)==0,"production geometry");
        uint64_t wanted_base=j.layer*layer_size+level_base[j.level];
        require(row==j.row&&rows==j.rows&&slices==1&&bpb==16&&th==0&&td==0&&base==wanted_base,"independent NPOT array/mip geometry");
        for(unsigned by=0;by<j.rows;by++)for(unsigned bx=0;bx<j.row/16;bx++) {
            uint8_t bytes[16];block(bytes,j.layer,j.level,bx,by);
            uint64_t a=wanted_base+gob_address(bx*16,by,j.row);
            require(a+16<=pool.length,"ASTC source bounds");
            memcpy((uint8_t *)pool.contents+a,bytes,16);memcpy(expected+j.offset+(size_t)by*j.row+bx*16,bytes,16);
        }
    }
    MTLPixelFormat format=block_size==8?(srgb?MTLPixelFormatASTC_8x8_sRGB:MTLPixelFormatASTC_8x8_LDR):
        (srgb?MTLPixelFormatASTC_5x5_sRGB:MTLPixelFormatASTC_5x5_LDR);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:width mipmapped:YES];
    td.textureType=layers==90?MTLTextureType2DArray:MTLTextureType2D;td.arrayLength=layers;td.mipmapLevelCount=levels;
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[dev newTextureWithDescriptor:td];require(texture!=nil,"actual ASTC texture allocation");
    NSArray *allocations=@[pool,staging,params,requests,output,texture];for(id<MTLAllocation> a in allocations)[residency addAllocation:a];[residency commit];
    NSError *error=nil;MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new];ad.maxBufferBindCount=3;ad.maxTextureBindCount=1;
    id<MTL4ArgumentTable> args=[dev newArgumentTableWithDescriptor:ad error:&error];require(args!=nil,"argument table");
    id<MTLComputePipelineState> sampler=[dev newComputePipelineStateWithFunction:[library newFunctionWithName:layers==90?@"inspect_array":@"inspect_2d"] error:&error];
    require(sampler!=nil,"typed ASTC sampling pipeline");
    id<MTL4CommandAllocator> allocator=[dev newCommandAllocator];require(allocator!=nil,"allocator");
    id<MTL4CommandBuffer> cb=[dev newCommandBuffer];[cb beginCommandBufferWithAllocator:allocator];
    id<MTL4ComputeCommandEncoder> enc=[cb computeCommandEncoder];
    [enc barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for(unsigned i=0;i<nj;i++){Job j=jobs[i];require(nvn_deswz_gpu_encode_m4(enc,args,params,i*256,pool,0,&desc,j.level,j.layer,staging,j.offset,j.row)==0,"production whole GPU untiling");}
    [enc endEncoding];enc=[cb computeCommandEncoder];
    [enc barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for(unsigned i=0;i<nj;i++){Job j=jobs[i];
        [enc copyFromBuffer:staging sourceOffset:j.offset sourceBytesPerRow:j.row sourceBytesPerImage:(size_t)j.row*j.rows sourceSize:MTLSizeMake(j.w,j.h,1)
                 toTexture:texture destinationSlice:j.layer destinationLevel:j.level destinationOrigin:MTLOriginMake(0,0,0)];}
    [enc barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    [args setAddress:requests.gpuAddress atIndex:0];[args setAddress:output.gpuAddress atIndex:1];[args setTexture:texture.gpuResourceID atIndex:0];
    [enc setArgumentTable:args];[enc setComputePipelineState:sampler];[enc dispatchThreads:MTLSizeMake(ns,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [enc endEncoding];[cb endCommandBuffer];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block BOOL failed=NO;MTL4CommitOptions *options=[MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback){if(feedback.error){failed=YES;fprintf(stderr,"GPU error: %s\n",feedback.error.localizedDescription.UTF8String);}dispatch_semaphore_signal(done);}];
    id<MTL4CommandBuffer> buffers[]={cb};[queue commit:buffers count:1 options:options];
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    require(now<gpu_deadline,"total test deadline");
    require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,(int64_t)(gpu_deadline-now)))==0,"bounded GPU completion");
    require(!failed,"GPU feedback success");
    require(memcmp(staging.contents,expected,staging.length)==0,"whole untiling bytes and staging padding");
    for(unsigned i=0;i<ns;i++){Sample s=samples[i];for(unsigned c=0;c<4;c++) {
        double want=color(s.layer,s.mip,s.x/block_size,s.y/block_size,c)/255.;if(srgb&&c<3)want=linear_srgb(want);
        double got=((float *)output.contents)[i*4+c];
        if(!isfinite(got)||fabs(got-want)>0.003){fprintf(stderr,"FAIL ASTC%ux%u srgb%d layer%u mip%u xy%u,%u c%u got%.8f expected%.8f\n",block_size,block_size,srgb,s.layer,s.mip,s.x,s.y,c,got,want);exit(1);}checks++;
    }}
    printf("PASS ASTC%ux%u %s %ux%u layers%u: %u mip/layer uploads, %u typed samples\n",block_size,block_size,srgb?"sRGB":"LDR",width,width,layers,nj,ns);
    free(expected);for(id<MTLAllocation> a in allocations)[residency removeAllocation:a];[residency commit];
} }
int main(void) { @autoreleasepool {
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();if(!dev||![dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    gpu_deadline=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+90ull*NSEC_PER_SEC;
    require(nvn_deswz_gpu_init(dev)==0,"production untiling pipeline");NSError *error=nil;
    id<MTLLibrary> library=[dev newLibraryWithSource:@(sample_msl) options:nil error:&error];
    if(!library)fprintf(stderr,"sampling compile: %s\n",error.localizedDescription.UTF8String);require(library!=nil,"sampling shader compile");
    id<MTL4CommandQueue> queue=[dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];require(queue!=nil,"queue");
    id<MTLResidencySet> residency=[dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];require(residency!=nil,"residency");[queue addResidencySet:residency];
    for(unsigned srgb=0;srgb<2;srgb++){run_case(dev,queue,residency,library,1365,8,srgb);run_case(dev,queue,residency,library,1536,5,srgb);}
    printf("PASS ASTC whole upload + typed sampling, %llu checks\n",checks);return 0;
} }

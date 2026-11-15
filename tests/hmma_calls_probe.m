#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
static void require(int ok,const char *why){if(!ok){fprintf(stderr,"FAIL %s\n",why);exit(1);}}
static float h2f(uint16_t h){__fp16 v;memcpy(&v,&h,2);return (float)v;}
static uint16_t f2h(float f){__fp16 v=(__fp16)f;uint16_t h;memcpy(&h,&v,2);return h;}
int main(int argc,char **argv){@autoreleasepool{
    require(argc==3,"two helper variants");id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    if(!dev||![dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *error=nil;id<MTLComputePipelineState> states[2];
    for(int i=0;i<2;i++){
        NSData *data=[NSData dataWithContentsOfFile:@(argv[i+1])];
        id<MTLLibrary> lib=[dev newLibraryWithData:dispatch_data_create(data.bytes,data.length,NULL,DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&error];
        states[i]=[dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&error];
        require(states[i]!=nil,"matrix pipeline compilation");
    }
    const int groups=64,tg=128,n=groups*tg;
    id<MTLBuffer> input=[dev newBufferWithLength:n*32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out[2]={[dev newBufferWithLength:n*8 options:MTLResourceStorageModeShared],[dev newBufferWithLength:n*8 options:MTLResourceStorageModeShared]};
    id<MTLBuffer> repetitions=[dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    id<MTLResidencySet> res=[dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    [res addAllocation:input];[res addAllocation:out[0]];[res addAllocation:out[1]];[res addAllocation:repetitions];[res commit];
    id<MTL4CommandQueue> queue=[dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    [queue addResidencySet:res];
    uint32_t *w=input.contents;srand(11);
    for(int i=0;i<n*8;i++)w[i]=f2h((rand()%2001-1000)/1000.0f)|((uint32_t)f2h((rand()%2001-1000)/1000.0f)<<16);
    unsigned checked=0;
    for(unsigned mode=0;mode<2;mode++){
        *(uint32_t*)repetitions.contents=mode?32:1;
        id<MTL4CommandAllocator> allocator=[dev newCommandAllocator];id<MTL4CommandBuffer> cb=[dev newCommandBuffer];[cb beginCommandBufferWithAllocator:allocator];
        NSMutableArray *tables=[NSMutableArray new];
        for(int variant=0;variant<2;variant++){
            MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=3;
            id<MTL4ArgumentTable> table=[dev newArgumentTableWithDescriptor:d error:&error];
            [tables addObject:table];
            [table setAddress:input.gpuAddress atIndex:0];[table setAddress:out[variant].gpuAddress atIndex:1];[table setAddress:repetitions.gpuAddress atIndex:2];
            id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];[e setComputePipelineState:states[variant]];[e setArgumentTable:table];
            [e dispatchThreadgroups:MTLSizeMake(groups,1,1) threadsPerThreadgroup:MTLSizeMake(tg,1,1)];[e endEncoding];
        }
        [cb endCommandBuffer];id<MTL4CommandBuffer> commands[]={cb};[queue commit:commands count:1];
        id<MTLSharedEvent> done=[dev newSharedEvent];[queue signalEvent:done value:1];require([done waitUntilSignaledValue:1 timeoutMS:10000],"bounded GPU completion");
        require(tables.count==2,"argument tables retained through GPU completion");
        require(memcmp(out[0].contents,out[1].contents,n*8)==0,"function-call rewrite changed GPU matrix output");checked+=n*4;
        if(!mode){
            const uint32_t *o=out[1].contents;unsigned bad=0;
            for(int warp=0;warp<n/32;warp++){
                float A[16][16],B[16][8],C[16][8];
                for(int lane=0;lane<32;lane++){
                    const uint32_t *f=w+(warp*32+lane)*8;int g=lane/4,t=lane%4;
                    for(int r=0;r<4;r++){int row=g+8*(r&1),k=2*t+8*(r>>1);A[row][k]=h2f(f[r]);A[row][k+1]=h2f(f[r]>>16);}
                    for(int r=0;r<2;r++){int k=2*t+8*r;B[k][g]=h2f(f[4+r]);B[k+1][g]=h2f(f[4+r]>>16);}
                    for(int r=0;r<2;r++){int row=g+8*r;C[row][2*t]=h2f(f[6+r]);C[row][2*t+1]=h2f(f[6+r]>>16);}
                }
                for(int lane=0;lane<32;lane++){int g=lane/4,t=lane%4;const uint32_t *p=o+(warp*32+lane)*2;
                    for(int q=0;q<2;q++)for(int j=0;j<2;j++){int row=g+8*q,col=2*t+j;float ref=C[row][col];for(int k=0;k<16;k++)ref+=A[row][k]*B[k][col];
                        double delta=fabs(h2f(p[q]>>(16*j))-ref);bad+=delta>0.01+fabs(ref)*0.004;}
                }
            }
            require(!bad,"matrix results disagree with independent CPU reference");
        }
    }
    printf("PASS %u half outputs bit-identical; 256 SIMD groups, 1/32 chained calls; independent CPU matrix reference passed\n",checked);return 0;
}}

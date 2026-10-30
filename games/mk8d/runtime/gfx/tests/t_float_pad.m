// Float32 attributes with fewer than four components must pad w with float 1
// even when translation declares an integer input and bitcasts it back to float.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int main(void) { @autoreleasepool {
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); if (!dev) return 2;
    NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\nstruct U { uint4 a [[attribute(0)]]; float4 f [[attribute(16)]]; };\nstruct I { int4 a [[attribute(0)]]; float4 f [[attribute(16)]]; };\nvertex void vu(U in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o=in.a|as_type<uint4>(in.f); }\nvertex void vi(I in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o=as_type<uint4>(in.a)|as_type<uint4>(in.f); }\n";
    NSError *err=nil; id<MTLLibrary> lib=[dev newLibraryWithSource:src options:nil error:&err]; if (!lib) { puts(err.localizedDescription.UTF8String); return 2; }
    id<MTLCommandQueue> q=[dev newCommandQueue]; float data[4]={-1,3,0.5,1};
    id<MTLBuffer> vb=[dev newBufferWithBytes:data length:sizeof data options:MTLResourceStorageModeShared];
    id<MTLBuffer> zero=[dev newBufferWithLength:16 options:MTLResourceStorageModeShared], out=[dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(zero.contents,0,16);
    int failed=0, checks=0; int formats[]={0x0a,0x16,0x22,0x2e};
    for (int sign=0;sign<2;sign++) for (int n=1;n<=4;n++) {
        int ct=sign?NVN_CT_INT:NVN_CT_UINT; nvn_refl rf={0}; rf.nv=1; rf.v[0].ct=ct; rf.v[0].nvn=0; memset(rf.v[0].sh,0xff,sizeof rf.v[0].sh); rf.v[0].sh[NVN_CT_FLOAT]=16;
        int routed=vin_ct(&rf,0,formats[n-1],1), loc=routed==NVN_CT_FLOAT?16:0;
        MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format=ct==NVN_CT_UINT?MTLVertexFormatUInt4:MTLVertexFormatInt4; vd.attributes[0].bufferIndex=30;
        vd.attributes[16].format=MTLVertexFormatFloat4; vd.attributes[16].bufferIndex=30;
        vd.attributes[loc].format=routed==NVN_CT_FLOAT?vfmt(formats[n-1]):vfmt_for_ct(vfmt(formats[n-1]),ct,formats[n-1]); vd.attributes[loc].bufferIndex=2;
        vd.layouts[2].stride=n*4; vd.layouts[30].stride=16; vd.layouts[30].stepFunction=MTLVertexStepFunctionConstant; vd.layouts[30].stepRate=0;
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:sign?@"vi":@"vu"]; pd.vertexDescriptor=vd; pd.rasterizationEnabled=NO;
        id<MTLRenderPipelineState> ps=[dev newRenderPipelineStateWithDescriptor:pd error:&err]; if (!ps) { puts(err.localizedDescription.UTF8String); return 2; }
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor]; rp.renderTargetWidth=rp.renderTargetHeight=1; rp.defaultRasterSampleCount=1;
        memset(out.contents,0xEE,16);
        id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp]; [en setRenderPipelineState:ps]; [en setVertexBuffer:out offset:0 atIndex:1]; [en setVertexBuffer:vb offset:0 atIndex:2]; [en setVertexBuffer:zero offset:0 atIndex:30]; [en drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status!=MTLCommandBufferStatusCompleted || cb.error) { printf("GPU command failed: %s\n",cb.error.localizedDescription.UTF8String); return 2; }
        uint32_t expected[4]={0,0,0,0x3f800000}; memcpy(expected,data,n*4);
        checks++; if (memcmp(out.contents,expected,16)) { failed++; printf("FAIL Float%d -> %s4 w=%#x expected float1=%#x\n",n,sign?"int":"uint",((uint32_t*)out.contents)[3],expected[3]); }
        pso_key k={0}; k.attr[0].fmt=formats[n-1]; float expanded[4]={0};
        checks++; if (n==4) { if (vconv_wanted(&rf,0,&k,0) || routed!=ct) { failed++; puts("FAIL Float4 lost direct bit-preserving fetch"); } }
        else if (!vconv_wanted(&rf,0,&k,0)) { failed++; printf("FAIL Float%d CPU fallback not admitted\n",n); }
        else { vconv_fill(formats[n-1],(uint64_t)(uintptr_t)data,n*4,1,expanded); if (memcmp(expanded,expected,16) || vconv_size(formats[n-1])!=n*4) { failed++; printf("FAIL Float%d CPU decode/extent\n",n); } }
    }
    printf("float padding: %d checks, %d failures\n",checks,failed); return failed?1:0;
} }

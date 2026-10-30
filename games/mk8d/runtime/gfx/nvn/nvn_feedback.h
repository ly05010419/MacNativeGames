// The first 64 bytes are shared with offline main0_tf (dmsh_build.py).
// Control buffers are opaque to the game: four byte offsets followed by Metal
// array-draw arguments. All updates and saved/restored counts follow GPU order.
typedef struct {
    uint32_t offset[4], args[4]; uint64_t order;
    uint32_t first, count, base_instance, pad; uint64_t reserved;
    uint64_t state, source_count; uint32_t stride[4], capacity[4];
    uint32_t requested, primitive; uint64_t reserved2;
} nvn_tf_job;
_Static_assert(offsetof(nvn_tf_job, state) == 64 && sizeof(nvn_tf_job) == 128, "feedback capture ABI");
static const char k_nvn_tf_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct capture {uint4 offset,args; device const uint* order; uint first,count,base_instance,pad; ulong reserved;};\n"
"struct job {capture c; device uint* state; device const uint* source_count; uint4 stride,capacity; uint requested,primitive; ulong reserved;};\n"
"kernel void tf_prepare(device job& j [[buffer(0)]]) {\n"
" uint n=j.source_count?j.source_count[0]:j.requested;\n"
" for(uint b=0;b<4;b++){j.c.offset[b]=j.state[b];if(j.stride[b]) n=min(n,(j.capacity[b]-min(j.capacity[b],j.state[b]))/j.stride[b]);}\n"
" n-=n%j.primitive; j.c.args=uint4(n,1,0,j.c.base_instance);\n"
" for(uint b=0;b<4;b++)if(j.stride[b])j.state[b]+=n*j.stride[b];\n"
" j.state[4]+=n;j.state[5]=1;j.state[6]=j.state[7]=0;\n"
"}\n";

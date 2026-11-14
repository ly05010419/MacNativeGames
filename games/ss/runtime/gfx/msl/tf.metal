// Transform feedback capture: tf_prepare (nvn_feedback.h, nvn_feedback.inc)
#include <metal_stdlib>
using namespace metal;
struct capture {uint4 offset,args; device const uint* order; uint first,count,base_instance,pad; ulong reserved;};
struct job {capture c; device uint* state; device const uint* source_count; uint4 stride,capacity; uint requested,primitive; ulong reserved;};
kernel void tf_prepare(device job& j [[buffer(0)]]) {
 uint n=j.source_count?j.source_count[0]:j.requested;
 for(uint b=0;b<4;b++){j.c.offset[b]=j.state[b];if(j.stride[b]) n=min(n,(j.capacity[b]-min(j.capacity[b],j.state[b]))/j.stride[b]);}
 n-=n%j.primitive; j.c.args=uint4(n,1,0,j.c.base_instance);
 for(uint b=0;b<4;b++)if(j.stride[b])j.state[b]+=n*j.stride[b];
 j.state[4]+=n;j.state[5]=1;j.state[6]=j.state[7]=0;
}

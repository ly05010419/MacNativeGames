// Geometry shader output compaction (nvn_geometry.h, nvn_geometry.inc)
#include <metal_stdlib>
using namespace metal;
struct geo_pack { device const uint *src; device uint *dst; device uint *args; uint groups, stride, topology, capacity; device uint *cnt; };
static bool geo_room(constant geo_pack&j,uint o,uint k,uint n){return o<=j.capacity&&k<=j.capacity-o&&n<=j.capacity-o-k;}
static uint geo_group(constant geo_pack& j, uint g, bool w, uint o) {
 uint a=0,b=0,n=0,k=0;
 for(uint i=0;i<j.stride;i++) { uint v=j.src[g*j.stride+i];
  if(v==0xffffffffu) {n=0;continue;}
  if(j.topology==1) {if(w && geo_room(j,o,k,1)) j.dst[o+k]=v; k+=1;}
  else if(j.topology==6) {if(n) {if(w && geo_room(j,o,k,2)) {j.dst[o+k]=b;j.dst[o+k+1]=v;} k+=2;}}
  else if(n>=2) {if(w && geo_room(j,o,k,3)) {j.dst[o+k]=(n&1)?b:a;j.dst[o+k+1]=(n&1)?a:b;j.dst[o+k+2]=v;} k+=3;}
  a=b;b=v;n++;
 }
 return k;
}
kernel void geo_pack_count(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]], uint block [[threadgroup_position_in_grid]],
 uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]], uint sw [[threads_per_simdgroup]]) {
 threadgroup uint sums[64]; uint c=g<j.groups?geo_group(j,g,false,0):0u;
 uint local=simd_prefix_exclusive_sum(c), total=simd_sum(c); if(sl==0) sums[si]=total;
 threadgroup_barrier(mem_flags::mem_threadgroup); uint before=0; for(uint s=0;s<si;s++) before+=sums[s];
 if(g<j.groups) j.cnt[g]=before+local;
 if(li==0) { uint n=0; for(uint s=0;s<(64u+sw-1u)/sw;s++) n+=sums[s]; j.cnt[j.groups+block]=n; }
}
kernel void geo_pack_scan_legacy(constant geo_pack& j [[buffer(0)]]) {
 uint blocks=j.groups/64u+uint(j.groups%64u!=0u);ulong s=0;
 for(uint b=0;b<blocks;b++){uint c=j.cnt[j.groups+b];j.cnt[j.groups+b]=uint(min(s,ulong(0xffffffffu)));s+=c;}
 uint w=j.topology==1?1u:j.topology==6?2u:3u,n=s<=j.capacity?uint(s):j.capacity-j.capacity%w;
 j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;
}
kernel void geo_pack_scan(constant geo_pack& j [[buffer(0)]], uint li [[thread_index_in_threadgroup]], uint3 nt [[threads_per_threadgroup]]) {
 threadgroup ulong sums[256];uint blocks=j.groups/64u+uint(j.groups%64u!=0u),chunk=(blocks+nt.x-1u)/nt.x,lo=li*chunk,hi=min(lo+chunk,blocks);ulong s=0;
 for(uint b=lo;b<hi;b++)s+=j.cnt[j.groups+b];sums[li]=s;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint step=1;step<nt.x;step<<=1){ulong add=li>=step?sums[li-step]:0;threadgroup_barrier(mem_flags::mem_threadgroup);sums[li]+=add;threadgroup_barrier(mem_flags::mem_threadgroup);}
 s=li?sums[li-1]:0;for(uint b=lo;b<hi;b++){uint c=j.cnt[j.groups+b];j.cnt[j.groups+b]=uint(min(s,ulong(0xffffffffu)));s+=c;}
 if(li==0){ulong total=sums[nt.x-1];uint w=j.topology==1?1u:j.topology==6?2u:3u,n=total<=j.capacity?uint(total):j.capacity-j.capacity%w;
 j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;}
}
kernel void geo_pack_small(constant geo_pack& j [[buffer(0)]], uint li [[thread_index_in_threadgroup]],
 uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]], uint sw [[threads_per_simdgroup]], uint3 nt [[threads_per_threadgroup]]) {
 threadgroup uint sums[128];uint chunk=(j.groups+nt.x-1u)/nt.x,lo=li*chunk,hi=min(lo+chunk,j.groups),s=0;
 for(uint g=lo;g<hi;g++)s+=geo_group(j,g,false,0);uint before=simd_prefix_exclusive_sum(s),total=simd_sum(s);if(sl==0)sums[si]=total;
 threadgroup_barrier(mem_flags::mem_threadgroup);for(uint b=0;b<si;b++)before+=sums[b];
 for(uint g=lo;g<hi;g++)before+=geo_group(j,g,true,before);
 if(li==0){uint n=0;for(uint b=0;b<(nt.x+sw-1u)/sw;b++)n+=sums[b];uint w=j.topology==1?1u:j.topology==6?2u:3u;n=n<=j.capacity?n:j.capacity-j.capacity%w;
 j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;}
}
kernel void geo_pack_write(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]]) { if(g<j.groups){ulong o=ulong(j.cnt[g])+j.cnt[j.groups+g/64u];if(o<=0xffffffffu)geo_group(j,g,true,uint(o));} }
struct geo_lean { device uint *raw; device const uint *mask; device const uint *map; device uint *dst; device uint *args; uint groups, stride, first, first_instance, capacity, collapse; };
kernel void geo_lean_cull(constant geo_lean& j [[buffer(0)]], uint g [[thread_position_in_grid]]) {
 if(g>=j.groups) return;
 for(uint i=0;i<j.stride;i++){uint v=j.raw[g*j.stride+i]; if(v!=0xffffffffu && !(j.mask[v]&1u)) j.raw[g*j.stride+i]=0xffffffffu;}
}
kernel void geo_lean_tri(constant geo_lean& j [[buffer(0)]], uint g [[thread_position_in_grid]]) {
 if(g>=j.groups) return;
 uint v[3]={0,0,0},n=0; bool keep=true;
 for(uint i=0;i<j.stride;i++){uint x=j.raw[g*j.stride+i]; if(x==0xffffffffu){if(n) break; continue;} if(n<3) v[n]=x; n++;}
 keep=n==3; for(uint k=0;k<3&&keep;k++) keep=(j.mask[v[k]]&1u)!=0u;
 for(uint k=0;k<3;k++) j.dst[3*g+k]=keep?j.map[v[k]]+j.first:j.collapse;
}
kernel void geo_lean_index(constant geo_lean& j [[buffer(0)]], uint i [[thread_position_in_grid]]) {
 if(i==0) j.args[4]=j.first_instance;
 if(i<j.args[0] && i<j.capacity) j.dst[i]=j.map[j.dst[i]]+j.first;
}

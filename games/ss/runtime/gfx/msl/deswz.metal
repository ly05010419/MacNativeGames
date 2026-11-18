// Block-linear textures deswizzled on the GPU (nvn_deswizzle_gpu.m)
#include <metal_stdlib>
using namespace metal;
struct P { ulong src_off, dst_off, base, pitch; uint row_bytes, rows, slices, th, td, gobs_x, gob2; };   // gob2: the Switch 2's order inside a GOB (nvn_gob_switch2)
kernel void deswz(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant P& p [[buffer(2)]], uint3 id [[thread_position_in_grid]]) {
 uint bh = 1u << p.th, bd = 1u << p.td, gobs_x = p.gobs_x, blocks_y = (p.rows + (8u * bh) - 1u) >> (3u + p.th);
 uint tid = id.x & 31u, gob_x = id.x >> 5, gob_y = id.y, z = id.z; uint xseg = p.gob2 ? ((tid >> 4) << 1) | ((tid >> 2) & 1u) : ((tid >> 4) << 1) | ((tid >> 1) & 1u), y = (gob_y << 3) | (p.gob2 ? (((tid >> 3) & 1u) << 2) | (tid & 3u) : (((tid >> 2) & 3u) << 1) | (tid & 1u)), x = (gob_x << 6) | (xseg << 4);
 if (x >= p.row_bytes || y >= p.rows || z >= p.slices) return;
 uint n = min(16u, p.row_bytes - x); ulong block = 512ul * bh * bd; ulong gob_in_block = ((ulong)(z & (bd - 1u)) << p.th) + (gob_y & (bh - 1u)); ulong so = p.src_off + p.base + ((ulong)(z >> p.td) * gobs_x * blocks_y + (gob_y >> p.th) * gobs_x + gob_x) * block + gob_in_block * 512ul + (ulong)tid * 16ul; ulong doff = p.dst_off + ((ulong)z * p.rows + y) * p.pitch + x;
 if (n == 16u && ((so | doff) & 15ul) == 0ul) *(device uint4 *)(dst + doff) = *(device const uint4 *)(src + so);
 else for (uint i = 0; i < n; i++) dst[doff + i] = src[so + i];
}
struct D { ulong src_off, base, layer_stride; uint width,height,layers,level,th,gobs_x,gob2; };
kernel void deswz_direct(device const uchar *src [[buffer(0)]], constant D& p [[buffer(2)]], texture2d_array<uint, access::write> dst [[texture(0)]], uint3 id [[thread_position_in_grid]]) {
 if(id.x>=p.width||id.y>=p.height||id.z>=p.layers)return;
 uint x=id.x*8u,y=id.y,bh=1u<<p.th,gy=y>>3;
 ulong off=p.src_off+p.base+ulong(id.z)*p.layer_stride+((ulong)(gy>>p.th)*p.gobs_x+(x>>6))*(512ul*bh)+(ulong)(gy&(bh-1u))*512ul;
 off+=p.gob2?((x&63u)>>5)*256u+((y&7u)>>2)*128u+((x&31u)>>4)*64u+((y&3u)>>1)*32u+(y&1u)*16u+(x&15u):((x&63u)>>5)*256u+((y&7u)>>1)*64u+((x&31u)>>4)*32u+(y&1u)*16u+(x&15u);
 dst.write(uint4(*(device const ushort4*)(src+off)),id.xy,id.z,p.level);
}

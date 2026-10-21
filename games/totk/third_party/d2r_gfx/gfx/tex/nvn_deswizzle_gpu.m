#import "nvn_deswizzle_gpu.h"

static id<MTLComputePipelineState> g_nvn_deswz_pipeline, g_nvn_region_pipeline;

static const char *const k_nvn_deswz_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct P { ulong src_off, dst_off, base, pitch; uint row_bytes, rows, slices, th, td; };\n"
    "kernel void deswz(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant P& p [[buffer(2)]], uint3 id [[thread_position_in_grid]]) {\n"
    " uint bh = 1u << p.th, bd = 1u << p.td, gobs_x = (p.row_bytes + 63u) >> 6, blocks_y = (p.rows + (8u * bh) - 1u) >> (3u + p.th);\n"
    " uint tid = id.x & 31u, gob_x = id.x >> 5, gob_y = id.y, z = id.z; uint xseg = ((tid >> 4) << 1) | ((tid >> 1) & 1u), y = (gob_y << 3) | (((tid >> 2) & 3u) << 1) | (tid & 1u), x = (gob_x << 6) | (xseg << 4);\n"
    " if (x >= p.row_bytes || y >= p.rows || z >= p.slices) return;\n"
    " uint n = min(16u, p.row_bytes - x); ulong block = 512ul * bh * bd; ulong gob_in_block = ((ulong)(z & (bd - 1u)) << p.th) + (gob_y & (bh - 1u)); ulong so = p.src_off + p.base + ((ulong)(z >> p.td) * gobs_x * blocks_y + (gob_y >> p.th) * gobs_x + gob_x) * block + gob_in_block * 512ul + (ulong)tid * 16ul; ulong doff = p.dst_off + ((ulong)z * p.rows + y) * p.pitch + x;\n"
    " if (n == 16u && ((so | doff) & 15ul) == 0ul) *(device uint4 *)(dst + doff) = *(device const uint4 *)(src + so);\n"
    " else for (uint i = 0; i < n; i++) dst[doff + i] = src[so + i];\n"
    "}\n"
    "struct R { ulong pool_off, linear_off, base, pitch, img; uint row_bytes, rows, slices, th, td, x, y, z, w, h, d, to_linear; };\n"
    "inline ulong region_bl(constant R& p, uint x, uint y, uint z) {\n"
    " ulong bh=1ul<<p.th, bd=1ul<<p.td, gx=((ulong)p.row_bytes+63ul)/64ul, by=((ulong)p.rows+8ul*bh-1ul)/(8ul*bh);\n"
    " return ((ulong)(z>>p.td)*gx*by+(ulong)(y>>(3u+p.th))*gx+(x>>6))*512ul*bh*bd\n"
    "      +(((ulong)z&(bd-1ul))*bh+(((ulong)y>>3)&(bh-1ul)))*512ul\n"
    "      +(x%64u/32u)*256ul+(y%8u/2u)*64ul+(x%32u/16u)*32ul+(y%2u)*16ul+x%16u;\n"
    "}\n"
    "kernel void texels_region(device uchar *pool [[buffer(0)]], device uchar *linear [[buffer(1)]], constant R& p [[buffer(2)]], uint3 id [[thread_position_in_grid]]) {\n"
    " uint seg=id.x*16u; if(seg>=p.w||id.y>=p.h||id.z>=p.d)return;\n"
    " uint x=p.x+seg,y=p.y+id.y,z=p.z+id.z,n=min(16u,p.w-seg);\n"
    " ulong a=p.pool_off+p.base+region_bl(p,x,y,z),b=p.linear_off+(ulong)id.z*p.img+(ulong)id.y*p.pitch+seg;\n"
    " if(n==16u && (x&15u)==0u && ((a|b)&15ul)==0ul) {\n"
    "   if(p.to_linear) *(device uint4 *)(linear+b)=*(device const uint4 *)(pool+a);\n"
    "   else *(device uint4 *)(pool+a)=*(device const uint4 *)(linear+b);\n"
    " } else for(uint i=0;i<n;i++) {\n"
    "   ulong q=p.pool_off+p.base+region_bl(p,x+i,y,z);\n"
    "   if(p.to_linear)linear[b+i]=pool[q];else pool[q]=linear[b+i];\n"
    " }\n"
    "}\n";

int nvn_deswz_gpu_init(id<MTLDevice> dev) {
    if (g_nvn_deswz_pipeline) return 0;
    if (!dev) return -1;
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(k_nvn_deswz_msl) options:nil error:&error];
    if (!library) return -1;
    id<MTLFunction> function = [library newFunctionWithName:@"deswz"];
    if (!function) return -1;
    g_nvn_deswz_pipeline = [dev newComputePipelineStateWithFunction:function error:&error];
    id<MTLFunction> region = [library newFunctionWithName:@"texels_region"];
    if (region) g_nvn_region_pipeline = [dev newComputePipelineStateWithFunction:region error:&error];
    return g_nvn_deswz_pipeline ? 0 : -1;
}

int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td
    };
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [enc setBuffer:src offset:0 atIndex:0];
    [enc setBuffer:dst offset:0 atIndex:1];
    [enc setBytes:&p length:sizeof p atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}

int nvn_deswz_gpu_encode_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !table || !params || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td
    };
    memcpy((uint8_t *)params.contents + params_off, &p, sizeof p);
    [enc setArgumentTable:table];
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [table setAddress:src.gpuAddress atIndex:0];
    [table setAddress:dst.gpuAddress atIndex:1];
    [table setAddress:params.gpuAddress + params_off atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}


int nvn_texels_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> pool, uint64_t pool_off,
                          const nvn_texdesc *d, unsigned level, unsigned layer,
                          uint32_t x,uint32_t y,uint32_t z,uint32_t w,uint32_t h,uint32_t depth,
                          id<MTLBuffer> linear,uint64_t linear_off,size_t pitch,size_t img,int to_linear) {
    if(!g_nvn_region_pipeline||!enc||!pool||!linear||!d||!w||!h||!depth||level>=32||level>=(d->levels?d->levels:1))return -1;
    nvn_texlayout layout;nvn_tex_layout(d,&layout);
    if(!layout.size||layer>=layout.layers||pool_off>pool.length||layout.size>pool.length-pool_off)return -1;
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
    if(nvn_deswizzle_geom(d,level,layer,&row,&rows,&slices,&base,&th,&td,&bpb)||
       x>row||w>row-x||y>rows||h>rows-y||z>slices||depth>slices-z||pitch<w||h>SIZE_MAX/pitch||img<pitch*h)return -1;
    /* Bound only the referenced bytes, without rejecting valid pitch padding beyond the last row. */
    uint64_t span=(uint64_t)(h-1)*pitch+w;
    if((uint64_t)(depth-1)>((UINT64_MAX-span)/img))return -1;
    span+=(uint64_t)(depth-1)*img;
    if(linear_off>linear.length||span>linear.length-linear_off)return -1;
    if(pool==linear)return -1; /* overlapping source/destination is not a conversion contract */
    struct { uint64_t pool_off,linear_off,base,pitch,img;uint32_t row_bytes,rows,slices,th,td,x,y,z,w,h,d,to_linear; } p={
      pool_off,linear_off,base,pitch,img,row,rows,slices,th,td,x,y,z,w,h,depth,to_linear!=0};
    [enc setComputePipelineState:g_nvn_region_pipeline];
    [enc setBuffer:pool offset:0 atIndex:0];[enc setBuffer:linear offset:0 atIndex:1];
    [enc setBytes:&p length:sizeof p atIndex:2];
    [enc dispatchThreads:MTLSizeMake((w+15ull)/16,h,depth) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
    return 0;
}

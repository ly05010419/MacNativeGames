/* Guard real TOTK-sized opaque objects through production NVN thunks. */
#include "runtime.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_protos.h"
#include <stdio.h>
#include <string.h>
#include <stdalign.h>
#include <stdlib.h>
#include "../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_texture_plan.h"
static void probe_texture_plan(void) {
 nvn_refl rf = {0};
 struct { uint64_t before; uint16_t slots[NVN_REFL_MAXB]; uint64_t after; } output;
 memset(&output, 0xa5, sizeof output);
 if(nvn_texture_plan(&rf,output.slots))runtime_fail("texture plan empty shader");
 rf.nb=8;
 rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=127};
 rf.b[1]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0};
 rf.b[2]=(nvn_rbind){.kind=NVN_RB_TBUF,.reg=9};
 rf.b[3]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=31};
 rf.b[4]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=128};
 rf.b[5]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=32};
 rf.b[6]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=127};
 rf.b[7]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=0};
 const uint16_t expected[]={127,9,159,127,128};
 unsigned count=nvn_texture_plan(&rf,output.slots);
 if(getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE"))==12)output.slots[2]=31;
 if(count!=5 || memcmp(output.slots,expected,sizeof expected))runtime_fail("texture plan ordered handles / image namespace");
 for(unsigned i=count;i<NVN_REFL_MAXB;i++)if(output.slots[i]!=0xa5a5)runtime_fail("texture plan wrote outside selected slots");
 rf.nb=NVN_REFL_MAXB;
 for(int i=0;i<NVN_REFL_MAXB;i++)rf.b[i]=(nvn_rbind){.kind=i%2?NVN_RB_IMG:NVN_RB_TEX,.reg=i%32};
 if(nvn_texture_plan(&rf,output.slots)!=NVN_REFL_MAXB)runtime_fail("texture plan maximum binding count");
 for(int i=0;i<NVN_REFL_MAXB;i++)if(output.slots[i]!=(i%2?128:0)+i%32)runtime_fail("texture plan maximum binding order");
 if(output.before!=0xa5a5a5a5a5a5a5a5ull || output.after!=0xa5a5a5a5a5a5a5a5ull)runtime_fail("texture plan output canary");
 fprintf(stderr,"texture plan probe: empty shader, ordered mixed bindings, duplicates, register boundaries, 64-slot capacity and canaries pass\n");
}
static void *(*lookup)(const char *);
static void *fn(const char *name){void *f=lookup(name);if(!f)runtime_fail("NVN probe lookup");return f;}
#define CALL(name,...) ((NVNT_##name)fn(#name))(__VA_ARGS__)
static void probe_linear_layout(void) {
 for(int mode=0;mode<4;mode++) {
  unsigned w=mode&1?68:260,h=w,bpp=mode&1?8:2,flags=mode<2?0x40:0x50;
  NVNtextureBuilder b;CALL(nvnTextureBuilderSetDefaults,&b);CALL(nvnTextureBuilderSetSize2D,&b,w,h);CALL(nvnTextureBuilderSetFormat,&b,mode&1?NVN_FORMAT_RGBA16:NVN_FORMAT_R16);CALL(nvnTextureBuilderSetFlags,&b,flags);
  if(CALL(nvnTextureBuilderGetStorageSize,&b)!=(size_t)640*h || CALL(nvnTextureBuilderGetStorageAlignment,&b)!=128)runtime_fail("linear render target implicit pitch/storage");
  CALL(nvnTextureBuilderSetStride,&b,768);if(CALL(nvnTextureBuilderGetStorageSize,&b)!=(size_t)768*h)runtime_fail("linear render target explicit pitch");
  nvn_texdesc d={.width=w,.height=h,.depth=1,.levels=1,.target=1,.format=mode&1?0x2a:6,.flags=flags,.stride=768};size_t row=w*bpp,n=row*h;unsigned char *src=malloc(n),*got=malloc(n),*pool=malloc((size_t)768*h);
  memset(pool,0xa5,(size_t)768*h);for(size_t i=0;i<n;i++)src[i]=(unsigned char)(i*29+i/13+17);
  if(nvn_swizzle(&d,0,0,src,row,pool))runtime_fail("linear render target CPU upload");
  for(unsigned y=0;y<h;y++){if(memcmp(pool+y*768,src+y*row,row))runtime_fail("linear render target row address");for(size_t x=row;x<768;x++)if(pool[y*768+x]!=0xa5)runtime_fail("linear render target padding changed");}
  if(nvn_deswizzle(&d,0,0,pool,got,row)||memcmp(src,got,n))runtime_fail("linear render target CPU read");free(src);free(got);free(pool);
 }
}
/* Expected footprints/tile modes obtained by executing v0 SDK 0x4429c8 /
   0x442de4 in Unicorn (analysis/terrain-layout-oracle.json). This is an
   independent oracle, including the equal-size case that must keep H=4. */
static void probe_minimal_layout(void) {
 struct layout_case { unsigned w,h,z,target,levels; uint64_t normal,minimal; unsigned th,td; } cases[] = {
  {320,320,1,1,1,245760,204800,4,0}, {256,256,1,1,1,131072,131072,4,0},
  {260,260,1,1,1,221184,152064,4,0}, {260,260,1,1,9,332800,214528,4,0},
  {320,320,4,4,1,245760,204800,4,0}, {320,320,5,2,1,1638400,1024000,0,2},
  {37,69,1,1,1,16384,9216,3,0}, {37,69,1,1,7,24576,15360,3,0}
 };
 /* Reproduce terrain corruption: a 320x320 RGB565 texture followed immediately
    by packed geometry. A wrong H=4 writes into every other 4 KiB of the latter. */
 const size_t bytes=320u*320u*2u, capacity=245760+4096;
 unsigned char *src=malloc(bytes),*pool=malloc(capacity),*readback=malloc(bytes);
 if(!src||!pool||!readback)runtime_fail("minimal layout allocation");
 for(size_t i=0;i<bytes;i++)src[i]=(unsigned char)(i*29+i/13+17);
 memset(pool,0xcd,capacity);
 nvn_texdesc d={.width=320,.height=320,.depth=1,.levels=1,.target=1,.format=0x3c,.flags=0x200};
 if(nvn_swizzle(&d,0,0,src,640,pool))runtime_fail("minimal texture upload");
 for(size_t i=bytes;i<capacity;i++)if(pool[i]!=0xcd)runtime_fail("minimal texture overwrote adjacent geometry");
 for(unsigned y=0;y<320;y++)for(unsigned x=0;x<640;x++) {
  size_t address=(y/8)*10*512+(x/64)*512+(x%64/32)*256+(y%8/2)*64+(x%32/16)*32+(y%2)*16+x%16;
  if(pool[address]!=src[y*640+x])runtime_fail("minimal one-GOB address differs from reference");
 }
 if(nvn_deswizzle(&d,0,0,pool,readback,640)||memcmp(src,readback,bytes))runtime_fail("minimal texture readback");
 free(src);free(pool);free(readback);
 for(unsigned i=0;i<sizeof cases/sizeof *cases;i++)for(unsigned mode=0;mode<2;mode++) {
  struct layout_case c=cases[i];nvn_texlayout l;
  d=(nvn_texdesc){.width=c.w,.height=c.h,.depth=c.z,.levels=c.levels,.target=c.target,.format=0x3c,.flags=mode?0x200:0};
  nvn_tex_layout(&d,&l);int reduced=mode&&c.normal!=c.minimal;
  if(l.layer_size!=(mode?c.minimal:c.normal)||l.tileH!=(reduced?0:c.th)||l.tileD!=(reduced?0:c.td))runtime_fail("minimal layout differs from original SDK");
  if(l.size!=l.layer_size*(c.target==4?c.z:1))runtime_fail("minimal array size");
 }
 d=(nvn_texdesc){.width=320,.height=320,.depth=1,.levels=1,.target=1,.format=0x3c,.flags=0x200,.pkg_block_h=4};
 nvn_texlayout l;nvn_tex_layout(&d,&l);
 if(l.tileH!=3||l.size!=204800)runtime_fail("packaged layout must override minimal policy");
}
void runtime_gfx_probe(int negative){
 probe_texture_plan();
 lookup=runtime_gfx_override("nvnBootstrapLoader");if(!lookup)runtime_fail("bootstrap override");
 if(negative){((uint64_t(*)(const void *))fn("nvnCommandBufferDrawTexture"))(NULL);runtime_fail("unimplemented NVN returned");}
 struct{alignas(16) NVNtexture value;unsigned char guard[64];} texture;
 NVNtextureBuilder builder;memset(&builder,0,sizeof(builder));memset(&texture,0xa5,sizeof(texture));
 CALL(nvnTextureBuilderSetDefaults,&builder);CALL(nvnTextureBuilderSetSize2D,&builder,1920,1080);CALL(nvnTextureBuilderSetFormat,&builder,NVN_FORMAT_RGBA8);
 if(CALL(nvnTextureBuilderGetStorageClass,&builder)!=1)runtime_fail("encoded texture storage class");
 if(!CALL(nvnTextureInitialize,&texture.value,&builder))runtime_fail("texture initialize");
 if(CALL(nvnTextureGetWidth,&texture.value)!=1920||CALL(nvnTextureGetHeight,&texture.value)!=1080||CALL(nvnTextureGetDepth,&texture.value)!=1)runtime_fail("texture dimensions");
 for(unsigned i=0;i<64;i++)if(texture.guard[i]!=0xa5)runtime_fail("NVN texture wrote past 128-byte object");
 if(CALL(nvnTextureGetStorageClass,&texture.value)!=CALL(nvnTextureBuilderGetStorageClass,&builder))runtime_fail("texture storage class identity");
 CALL(nvnTextureFinalize,&texture.value);
 for(unsigned i=0;i<64;i++)if(texture.guard[i]!=0xa5)runtime_fail("NVN texture finalize overflow");
 int major=0,minor=0;CALL(nvnDeviceGetInteger,NULL,0,&major);CALL(nvnDeviceGetInteger,NULL,1,&minor);if(major!=55||minor<15)runtime_fail("TOTK NVN ABI target");
 if(lookup("TOTK_not_an_nvn_entry"))runtime_fail("unknown NVN lookup should be null");
 probe_linear_layout();
 probe_minimal_layout();
 extern void dm_nvn_probe_present_interval(void); dm_nvn_probe_present_interval();
 extern void dm_nvn_probe_commands(void); dm_nvn_probe_commands();
 extern void dm_nvn_probe_texture_pins(void); dm_nvn_probe_texture_pins();
 extern void dm_nvn_probe_copy_marks(void); dm_nvn_probe_copy_marks();
 extern void dm_nvn_probe_inline_bulk(void); dm_nvn_probe_inline_bulk();
 extern void dm_nvn_probe_deferred_binds(void); dm_nvn_probe_deferred_binds();
 extern void dm_nvn_probe_prediction(void); dm_nvn_probe_prediction();
 printf("{\"status\":\"pass\",\"texture_object_bytes\":%zu,\"guard_bytes\":64,\"command_lifetime\":\"shared command memory, independent control memory, finalized recorder, copied payloads, reclamation\",\"texture_copies\":\"freed after two windows in which no recording or replay refers to them\",\"recording_copies\":\"freed when their control memory is added again, reused for other data or unmapped\",\"command_inline_flags\":\"pass\",\"deferred_binds\":\"handles read at submit, direct and called\",\"minimal_layout_sdk_cases\":16,\"minimal_layout_adjacent_geometry_guard\":true,\"linear_render_target_pitch\":true,\"prediction_preserves_scissor\":true,\"prediction_leaves_conditional_counters_untouched\":true,\"prediction_skips_unresolved_programs\":true,\"prediction_preserves_binding_variants\":true,\"prediction_reclaims_only_completed_stale_slots\":true,\"prediction_skips_untranslated_stages\":true,\"prediction_no_depthless_guess_for_depth_export\":true,\"scope\":\"NVN objects and CPU command recording; no drawing\"}\n",sizeof(NVNtexture));
}

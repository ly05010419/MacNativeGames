/* nn::pl shared fonts: the verified system BFTTF files laid out back to back as in the pl shared memory.
   GetSharedFontAddress points 8 bytes past each file start (after the BFTTF header), as the SDK adds the
   service-provided offset to the shared memory base; the size is the decoded font size. */
#include "runtime.h"
#include "shared_fonts_config.h"
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
static const FontRow rows[SHARED_FONT_COUNT]={SHARED_FONT_ROWS};
static uint8_t *region;static uint64_t offsets[SHARED_FONT_COUNT];
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void need(int c,const char *m){if(!c)runtime_fail(m);}
static void load(void){
 /* Astral Chain: the NSPs do not carry the system fonts; scripts/prepare_runtime.py leaves the rows empty without them. */
 if(!rows[0].path[0])runtime_fail("the game asked for the system shared fonts: extract them from your own Switch firmware to AC_build/extracted/firmware/shared-fonts (scripts/extract_shared_fonts.py) and run scripts/prepare_runtime.py again");
 uint64_t total=0;for(int i=0;i<SHARED_FONT_COUNT;i++)total+=rows[i].file_bytes;
 /* The GetSharedFont override hands out 64-bit pointers (a u32-offset pl:u model cannot reach anything:
    macOS arm64 refuses every user mapping below 4 GB), so the region lives wherever the kernel puts it. */
 region=malloc(total);need(region!=NULL,"shared font memory");
 uint64_t at=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){
  /* TOTK_SHARED_FONTS=<dir>: the same verified files in another directory (iOS: copied into the app's Documents). */
  char path[4096];const char *dir=getenv("TOTK_SHARED_FONTS"),*base=strrchr(rows[i].path,'/');
  if(dir)need(snprintf(path,sizeof path,"%s/%s",dir,base?base+1:rows[i].path)<(int)sizeof path,"shared font path");
  FILE *f=fopen(dir?path:rows[i].path,"rb");need(f!=NULL,"open shared font");
  need(fread(region+at,1,rows[i].file_bytes,f)==rows[i].file_bytes&&fgetc(f)==EOF,"shared font size");fclose(f);
  uint8_t d[32];char hex[65];CC_SHA256(region+at,(CC_LONG)rows[i].file_bytes,d);for(int k=0;k<32;k++)snprintf(hex+k*2,3,"%02x",d[k]);
  need(!strcmp(hex,rows[i].sha256),"shared font integrity mismatch");
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
/* For the pl:u IPC service: the region base (32-bit reachable) and the per-font offsets/sizes. */
uint32_t runtime_shared_font_base(void){valid(0);return (uint32_t)(uintptr_t)region;}
uint32_t runtime_shared_font_offset(int type){valid(type);return (uint32_t)offsets[type];}
const void *runtime_shared_font_address(int type){return address(type);}
size_t runtime_shared_font_size(int type){return size(type);}
unsigned runtime_shared_font_count(void){return SHARED_FONT_COUNT;}
/* GetSharedFont(int* count, SharedFontData* out, int max, LanguageCode): the sdk's own implementation
   does the whole pl:u IPC dance internally (RequestLoad, the priority list with 32-bit offsets into a
   shared memory macOS will not let us map below 4 GB, then command 6), so this override replaces it
   wholesale and hands out the region's real 64-bit pointers. SharedFontData is 16 bytes:
   {void* memory_address; uint32_t size; SharedFontType type;}. */
struct SharedFontData{void *address;uint32_t size;uint32_t type;};
static uint32_t get_shared_font(int32_t *count_out,struct SharedFontData *out,int32_t max,uint64_t lang){
 (void)lang;if(!count_out||!out)runtime_fail("pl:u null shared font out");valid(0);
 unsigned n=runtime_shared_font_count();if(n>(unsigned)max)n=(unsigned)max;
 for(unsigned i=0;i<n;i++){out[i].address=(void*)address((int)i);out[i].size=(uint32_t)size((int)i);out[i].type=i;}
 *count_out=(int32_t)n;return 0;}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",get_shared_font},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

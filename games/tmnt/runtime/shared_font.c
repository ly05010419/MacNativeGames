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
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
static const FontRow rows[SHARED_FONT_COUNT]={SHARED_FONT_ROWS};
static uint8_t *region;static uint64_t offsets[SHARED_FONT_COUNT];
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void need(int c,const char *m){if(!c)runtime_fail(m);}
/* Super Mario Odyssey 1.4.1 (SDK 7.3.2) hands GetSharedFontAddress straight to nn::fontll::ScalableFontEngine::LoadFont and
   never calls its Decode: with the XOR-encoded BFTTF payload every LoadFont returned 403 and the font texture cache stayed
   empty (SMO_TRACE_FONT=1). After the file is verified, its payload is decoded in place and the header rewritten with key
   0 (magic 0x7f9a0218, the plain size), so the data is plain TrueType and a Decode of it would change nothing. */
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put_be32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void decode_bfttf(uint8_t *p,uint64_t n){
 uint32_t key=be32(p)^0x7f9a0218u;need(n>=8&&(n-8)%4==0&&(be32(p+4)^key)==n-8,"shared font BFTTF header");
 for(uint64_t i=8;i<n;i+=4)put_be32(p+i,be32(p+i)^key);
 put_be32(p,0x7f9a0218u);put_be32(p+4,(uint32_t)(n-8));
 need(be32(p+8)==0x00010000u||be32(p+8)==0x4f54544fu,"shared font is not TrueType/OpenType after decoding");
}
static void load(void){
 uint64_t total=0;for(int i=0;i<SHARED_FONT_COUNT;i++)total+=rows[i].file_bytes;
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
  decode_bfttf(region+at,rows[i].file_bytes);
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): it asks for each font to be loaded before reading it; the fonts are loaded at
   startup. 1.6.0 also asks whether Labo VR mode is on: never on a Mac. */
static void request_load(int type){(void)type;}
static unsigned char vr_mode(void){return 0;}
/* BeginVrMode returns an nn::Result: it first asks CheckStereoVisionPermission(false) and answers 0xd08e (pctl
   module 142) when that says no (Smash 13.0.2 sdk+0x341e90). The host said nothing, so the game read whatever w0 held
   (Q71, 2026-10-24: main+0x1873650 tests it with cbz). It now answers that same "not permitted", matching the host's
   CheckStereoVisionPermission (system_services.c). EndVrMode returns only on success, so it stays void. */
static uint32_t vr_mode_begin(void){fprintf(stderr,"[botw] Labo VR mode is not available\n");return 0xd08e;}
static void vr_mode_end(void){}
/* Super Mario Odyssey 1.4.1 asks for all fonts at once, in the order of priority for a language (switchbrew pl:u
   GetSharedFontInOrderOfPriority): the language's own font first (Chinese, Korean), then Standard and the Nintendo
   extension, then the rest. SharedFontData is {int32 type, const void *address, size_t size}. */
typedef struct{int32_t type;const void *address;size_t size;} SharedFontData;
static int font_order(uint64_t language,int32_t out[SHARED_FONT_COUNT]){
 char code[9]={0};memcpy(code,&language,8);
 int32_t first=!strncmp(code,"zh-CN",5)||!strncmp(code,"zh-Hans",7)?1:!strncmp(code,"zh-TW",5)||!strncmp(code,"zh-Hant",7)?3:!strcmp(code,"ko")?4:0;
 int32_t pick[]={first,0,5,1,2,3,4};int n=0;
 for(unsigned i=0;i<sizeof pick/sizeof *pick;i++){int seen=0;for(int k=0;k<n;k++)seen|=out[k]==pick[i];if(!seen&&pick[i]<SHARED_FONT_COUNT)out[n++]=pick[i];}
 return n;
}
static uint32_t fonts_in_priority(int32_t *count,int32_t *types,const void **addresses,size_t *sizes,int max,uint64_t language){
 if(!count||(max>0&&(!types||!addresses||!sizes)))runtime_fail("pl font priority output pointers");
 int32_t order[SHARED_FONT_COUNT];int n=font_order(language,order);if(n>max)n=max<0?0:max;
 for(int i=0;i<n;i++){types[i]=order[i];addresses[i]=address(order[i]);sizes[i]=size(order[i]);}
 static int once;if(!once++)fprintf(stderr,"[totk] shared fonts in priority order: %d of max %d, first type %d\n",n,max,n?order[0]:-1);
 *count=n;return 0;
}
static uint32_t shared_fonts(int32_t *count,SharedFontData *out,int max,uint64_t language){
 if(!count||(max>0&&!out))runtime_fail("pl shared font output pointers");
 int32_t order[SHARED_FONT_COUNT];int n=font_order(language,order);if(n>max)n=max<0?0:max;
 for(int i=0;i<n;i++)out[i]=(SharedFontData){order[i],address(order[i]),size(order[i])};
 static int once;if(!once++)fprintf(stderr,"[totk] shared font data: %d of max %d, first type %d\n",n,max,n?order[0]:-1);
 *count=n;return 0;
}
/* 1.9.0 asks whether it runs on a Switch 2 ("Ounce"): no, it gets the original Switch's paths. */
static unsigned char running_on_ounce(void){return 0;}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}
  ,{"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},{"_ZN2nn2pl8IsVrModeEv",vr_mode},
  {"_ZN2nn2pl11BeginVrModeEv",vr_mode_begin},{"_ZN2nn2pl9EndVrModeEv",vr_mode_end},
  {"_ZN2nn2pl30GetSharedFontInOrderOfPriorityEPiPNS0_14SharedFontTypeEPPKvPmiNS_8settings12LanguageCodeE",fonts_in_priority},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",shared_fonts},
  {"_ZN2nn2pl16IsRunningOnOunceEv",running_on_ounce}
  };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

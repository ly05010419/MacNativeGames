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
static void load(void){
 if(!rows[0].path[0])runtime_fail("the game asked for the system shared fonts: extract them from your own Switch firmware to SMO_build/extracted/firmware/shared-fonts and run scripts/prepare_runtime.py again");
 uint64_t total=0;for(int i=0;i<SHARED_FONT_COUNT;i++)total+=rows[i].file_bytes;
 region=malloc(total);need(region!=NULL,"shared font memory");
 uint64_t at=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){
  char path[4096];const char *dir=getenv("TOTK_SHARED_FONTS"),*base=strrchr(rows[i].path,'/');
  if(dir)need(snprintf(path,sizeof path,"%s/%s",dir,base?base+1:rows[i].path)<(int)sizeof path,"shared font path");
  FILE *f=fopen(dir?path:rows[i].path,"rb");need(f!=NULL,"open shared font");
  need(fread(region+at,1,rows[i].file_bytes,f)==rows[i].file_bytes&&fgetc(f)==EOF,"shared font size");fclose(f);
  uint8_t d[32];char hex[65];CC_SHA256(region+at,(CC_LONG)rows[i].file_bytes,d);for(int k=0;k<32;k++)snprintf(hex+k*2,3,"%02x",d[k]);
  need(!strcmp(hex,rows[i].sha256),"shared font integrity mismatch");
  /* Decrypt BFTTF payload in place (words XORed with key = magic ^ 0x7f9a0218) */
  uint32_t magic=((uint32_t)region[at]<<24)|((uint32_t)region[at+1]<<16)|((uint32_t)region[at+2]<<8)|region[at+3];
  uint32_t key=magic^0x7f9a0218U;
  uint8_t *font=region+at+8;
  size_t words=(rows[i].file_bytes-8)/4;
  for(size_t w=0;w<words;w++){
   uint32_t val=((uint32_t)font[w*4]<<24)|((uint32_t)font[w*4+1]<<16)|((uint32_t)font[w*4+2]<<8)|font[w*4+3];
   val^=key;
   font[w*4]=(uint8_t)(val>>24);
   font[w*4+1]=(uint8_t)(val>>16);
   font[w*4+2]=(uint8_t)(val>>8);
   font[w*4+3]=(uint8_t)val;
  }
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes (decrypted TrueType in place)\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
static void request_load(int type){(void)type;valid(type);}
static void fonts_priority(int *count,int *types,const void **addrs,size_t *sizes,int max,uint64_t lang){
 (void)lang;
 int n=SHARED_FONT_COUNT<max?SHARED_FONT_COUNT:max;
 if(count)*count=n;
 for(int i=0;i<n;i++){
  if(types)types[i]=i;
  if(addrs)addrs[i]=address(i);
  if(sizes)sizes[i]=size(i);
 }
}
typedef struct{int type;const void *address;size_t size;} SharedFontData;
static void fonts_data(int *count,SharedFontData *data,int max,uint64_t lang){
 (void)lang;
 int n=SHARED_FONT_COUNT<max?SHARED_FONT_COUNT:max;
 if(count)*count=n;
 for(int i=0;i<n;i++){
  if(data){
   data[i].type=i;
   data[i].address=address(i);
   data[i].size=size(i);
  }
 }
}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size},
  {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},
  {"_ZN2nn2pl30GetSharedFontInOrderOfPriorityEPiPNS0_14SharedFontTypeEPPKvPmiNS_8settings12LanguageCodeE",fonts_priority},
  {"_ZN2nn2pl39GetSharedFontInOrderOfPriorityForSystemEPiPNS0_23SharedFontTypeForSystemEPPKvPmiNS_8settings12LanguageCodeE",fonts_priority},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",fonts_data}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

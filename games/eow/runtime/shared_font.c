/* nn::pl shared fonts: the verified system BFTTF files laid out back to back as in the pl shared memory.
   GetSharedFontAddress points 8 bytes past each file start (after the BFTTF header), as the SDK adds the
   service-provided offset to the shared memory base; the size is the decoded font size.
   In the pl shared memory only the 8-byte header stays obfuscated; the font after it is plain TrueType (homebrew hands
   that address straight to FreeType). The files keep the system archive's form, every big-endian word XORed with
   key = word0 ^ 0x7f9a0218, so the payload is decoded here after the hash check. Echoes of Wisdom renders text with
   these fonts (nn::fontll::ScalableFontEngine); with the obfuscated payload every CheckGlyphExist failed. */
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
 /* Bayonetta 3: the NSPs do not carry the system fonts; scripts/prepare_runtime.py leaves the rows empty without them. */
 if(!rows[0].path[0])runtime_fail("the game asked for the system shared fonts: extract them from your own Switch firmware to EoW_build/extracted/firmware/shared-fonts (scripts/extract_shared_fonts.py) and run scripts/prepare_runtime.py again");
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
  uint8_t *file=region+at;uint32_t key=((uint32_t)file[0]<<24|(uint32_t)file[1]<<16|(uint32_t)file[2]<<8|file[3])^0x7f9a0218u;
  uint32_t size=((uint32_t)file[4]<<24|(uint32_t)file[5]<<16|(uint32_t)file[6]<<8|file[7])^key;
  need(size==rows[i].font_bytes&&rows[i].font_bytes%4==0,"shared font header");
  for(uint64_t k=8;k<rows[i].file_bytes;k+=4)for(int b=0;b<4;b++)file[k+b]^=(uint8_t)(key>>(24-8*b));
  need(file[8]==0&&file[9]==1&&file[10]==0&&file[11]==0,"shared font is not TrueType after decoding");
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

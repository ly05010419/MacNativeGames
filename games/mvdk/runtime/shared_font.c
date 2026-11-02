/* nn::pl shared fonts: the verified system BFTTF files laid out back to back as in the pl shared memory.
   GetSharedFontAddress points at each file's BFTTF header: games hand it to nn::fontll::ScalableFontEngineHelper::Decode,
   which checks the header's magic, decrypts the body in place and returns the font 8 bytes further on (Super Mario Bros.
   Wonder's button icons come from the Nintendo Extension font this way). The size is the decoded font size. */
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
  offsets[i]=at;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): it asks for each font to be loaded before reading it; the fonts are loaded at
   startup. 1.6.0 also asks whether Labo VR mode is on: never on a Mac. */
static void request_load(int type){(void)type;}
static unsigned char vr_mode(void){return 0;}
static void vr_mode_change(void){fprintf(stderr,"[botw] Labo VR mode is not available\n");}
/* 1.9.0 asks whether it runs on a Switch 2 ("Ounce"): no, it gets the original Switch's paths. */
static unsigned char running_on_ounce(void){return 0;}
#endif
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}
#ifdef GAME_BOTW
  ,{"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},{"_ZN2nn2pl8IsVrModeEv",vr_mode},
  {"_ZN2nn2pl11BeginVrModeEv",vr_mode_change},{"_ZN2nn2pl9EndVrModeEv",vr_mode_change},
  {"_ZN2nn2pl16IsRunningOnOunceEv",running_on_ounce}
#endif
  };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

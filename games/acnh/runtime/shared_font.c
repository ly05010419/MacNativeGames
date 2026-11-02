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
 /* Bayonetta 3: the NSPs do not carry the system fonts; scripts/prepare_runtime.py leaves the rows empty without them. */
 if(!rows[0].path[0])runtime_fail("the game asked for the system shared fonts: extract them from your own Switch firmware (tools/extract_shared_fonts.py) and run build_host.py again with --shared-fonts");
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
  /* BFTTF stores each big-endian payload word XORed with the key encoded in
     its first word. The Switch pl service exposes decoded sfnt bytes. */
  uint8_t *font=region+at+8,*header=region+at;
  uint32_t encoded=((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
  uint32_t key=encoded^0x7f9a0218u;
  need(rows[i].font_bytes+8==rows[i].file_bytes,"shared font BFTTF length");
  for(uint64_t j=0;j<rows[i].font_bytes;j++)font[j]^=(uint8_t)(key>>(24-8*(j&3)));
  need((font[0]==0&&font[1]==1&&font[2]==0&&font[3]==0)||
       (font[0]=='O'&&font[1]=='T'&&font[2]=='T'&&font[3]=='O'),"shared font decoded sfnt header");
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static void request_load(int type){valid(type);}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}

typedef struct {
    int32_t type;
    int32_t pad;
    const void *address;
    size_t size;
} SharedFontData;

static unsigned char get_shared_font(int *loaded_count, SharedFontData *out, int max_count, uint64_t language_code) {
    (void)language_code;
    if (!loaded_count || !out || max_count <= 0) return 0;
    int count = max_count < SHARED_FONT_COUNT ? max_count : SHARED_FONT_COUNT;
    for (int i = 0; i < count; i++) {
        out[i].type = valid(i);
        out[i].pad = 0;
        out[i].address = address(i);
        out[i].size = size(i);
    }
    *loaded_count = count;
    return 1;
}

static unsigned char get_shared_font_priority(int *loaded_count, int *types, const void **addresses, size_t *sizes, int max_count, uint64_t language_code) {
    (void)language_code;
    if (!loaded_count || max_count <= 0) return 0;
    int count = max_count < SHARED_FONT_COUNT ? max_count : SHARED_FONT_COUNT;
    for (int i = 0; i < count; i++) {
        if (types) types[i] = valid(i);
        if (addresses) addresses[i] = address(i);
        if (sizes) sizes[i] = size(i);
    }
    *loaded_count = count;
    return 1;
}

static unsigned char is_vr_mode(void) { return 0; }
static void vr_nop(void) {}
static unsigned char is_ounce(void) { return 0; }

void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",get_shared_font},
  {"_ZN2nn2pl30GetSharedFontInOrderOfPriorityEPiPNS0_14SharedFontTypeEPPKvPmiNS_8settings12LanguageCodeE",get_shared_font_priority},
  {"_ZN2nn2pl39GetSharedFontInOrderOfPriorityForSystemEPiPNS0_23SharedFontTypeForSystemEPPKvPmiNS_8settings12LanguageCodeE",get_shared_font_priority},
  {"_ZN2nn2pl8IsVrModeEv",is_vr_mode},
  {"_ZN2nn2pl11BeginVrModeEv",vr_nop},
  {"_ZN2nn2pl9EndVrModeEv",vr_nop},
  {"_ZN2nn2pl16IsRunningOnOunceEv",is_ounce}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

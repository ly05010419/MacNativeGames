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
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <errno.h>
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
static const FontRow rows[SHARED_FONT_COUNT]={SHARED_FONT_ROWS};
static uint8_t *region;static uint64_t offsets[SHARED_FONT_COUNT];
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void need(int c,const char *m){if(!c)runtime_fail(m);}
static size_t stand_in_bytes;   /* non-zero: one plain TTF served for every type, BFTTF marker header + TTF */
static void load(void){
 /* Stand-in shared font (the nxnative route): one plain TrueType font behind the decoded BFTTF marker, used for
    every type. The game's CJK glyph lookups return NULL with the firmware fonts even after decoding them; the
    reference build of this game renders all dynamic text with macOS Arial Unicode here, so use the same. */
 { const char *p = getenv("PLA_SHARED_FONT_TTF"); if (!p || !*p) p = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
   FILE *f = fopen(p, "rb");
   if (f) {
     fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
     if (n > 0 && n < (200L << 20)) {
       region = malloc((size_t)n + 8); need(region != NULL, "shared font stand-in memory");
       need(fread(region + 8, 1, (size_t)n, f) == (size_t)n, "shared font stand-in read"); fclose(f);
       region[0] = 0x7f; region[1] = 0x9a; region[2] = 0x02; region[3] = 0x18;
       uint32_t l = (uint32_t)n ^ 0x49621806u;
       region[4] = (uint8_t)(l >> 24); region[5] = (uint8_t)(l >> 16); region[6] = (uint8_t)(l >> 8); region[7] = (uint8_t)l;
       for (int i = 0; i < SHARED_FONT_COUNT; i++) offsets[i] = 8;
       stand_in_bytes = (size_t)n + 8;
       fprintf(stderr, "[totk] shared font stand-in: %s (%ld byte TTF behind the BFTTF marker)\n", p, n);
       return;
     }
     fclose(f);
   }
   fprintf(stderr, "[totk] no stand-in shared font at %s; using the firmware BFTTF files\n", p);
 }
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
  /* Decode the archive BFTTF now. The file is big-endian words XORed with key = word0 ^ 0x7f9a0218; the decoded
     form is the marker 0x7f9a0218, the length, and the plain TrueType/OpenType font. The pl service decodes the
     font on hardware; leaving it obfuscated only works if the game calls ScalableFontEngineHelper::Decode on this
     address, and PLA's shared-font consumer does not (its own BFOTF files are decoded, these were not): the font
     stayed ciphertext and every dynamic glyph fell back to the ellipsis. nxnative sidesteps the same issue with a
     plain TTF + the marker; decoding keeps the original Nintendo font and its metrics. */
  { uint8_t *p=region+at; uint32_t w0=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    if(w0!=0x7f9a0218u){ uint32_t w1=((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
      uint32_t key=w0^0x7f9a0218u; p[0]=0x7f;p[1]=0x9a;p[2]=0x02;p[3]=0x18;
      uint32_t l=w1^key; p[4]=l>>24;p[5]=l>>16;p[6]=l>>8;p[7]=l;
      for(uint64_t o=8;o+4<=rows[i].file_bytes;o+=4){ uint32_t v=((uint32_t)p[o]<<24)|((uint32_t)p[o+1]<<16)|((uint32_t)p[o+2]<<8)|p[o+3]; v^=key; p[o]=v>>24;p[o+1]=v>>16;p[o+2]=v>>8;p[o+3]=v; }
      fprintf(stderr,"[totk] shared font %d decoded: BFTTF key %#x, %lu byte font\n",i,key,(unsigned long)(rows[i].file_bytes-8)); } }
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);
    /* PLA: the game gates its staged asset loading on GetLoadState. Return Loaded only after the game has
       done its initial file reads (shader/font/probe loading), so the boot movie data gets loaded too. */
    extern unsigned runtime_fs_read_count(void);
    unsigned reads = runtime_fs_read_count();
    static int nlog; if (nlog < 12) { nlog++; fprintf(stderr, "[totk] GetLoadState type=%d fs_reads=%u -> %s\n", type, reads, reads > 100 ? "Loaded" : "NotLoaded"); }
    return reads > 100 ? 1 : 0;}
/* PLA's main imports nn::fontll::ScalableFontEngineHelper::Decode, so the address handed to the game must be
   the **BFTTF header** (the 8-byte decoded marker + obfuscated length) followed by the TTF that Decode
   deobfuscates, and the size must include the header. Serving the TTF past the header (the old default) made
   Decode treat the font data as ciphertext and XOR it again: LoadFont then failed and every dynamic glyph fell
   back to the ellipsis (the language warning, the name banner and all dialogue text were blank). A game that
   does not import Decode takes the TTF itself; PLA_FONT_HEADER_SKIP=8 selects that form. */
static int header_form(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PLA_FONT_HEADER_SKIP");
        v = e ? (atoi(e) >= 16) : 1;
        fprintf(stderr, "[totk] shared font form: %s\n", v ? "BFTTF header + data (Decode)" : "TTF past the header");
    }
    return v;
}
static size_t size(int type){valid(type);if(stand_in_bytes)return header_form()?stand_in_bytes:stand_in_bytes-8;return (size_t)(header_form()?rows[type].file_bytes:rows[type].font_bytes);}
static const void *address(int type){
    valid(type);
    { extern void runtime_font_hook_set_region(const void *); runtime_font_hook_set_region(region); }
    const void *p = region + offsets[valid(type)] - (header_form() ? 8 : 0);
    {static int n;if(n++<10)fprintf(stderr,"[totk] GetSharedFontAddress(%d) -> %p (%s, %lu bytes)\n",type,p,header_form()?"header+data":"TTF",(unsigned long)size(type));}
    return p;}
/* For the pl:u IPC service: the region base (32-bit reachable) and the per-font offsets/sizes. */
uint32_t runtime_shared_font_base(void){valid(0);return (uint32_t)(uintptr_t)region;}
uint32_t runtime_shared_font_offset(int type){valid(type);return (uint32_t)offsets[type];}
const void *runtime_shared_font_address(int type){return address(type);}
size_t runtime_shared_font_size(int type){return size(type);}
unsigned runtime_shared_font_count(void){return SHARED_FONT_COUNT;}
const void *runtime_shared_font_region(void){valid(0);return region;}
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
/* PLA_FONT_ENGINE=1: the SDK scalable-font entry points, wrapped for the font probe (see runtime_font_override). */
typedef uint32_t (*font_load_fn)(void *, char *, const void *, unsigned, unsigned);
typedef uint32_t (*font_set_fn)(void *, char *);
typedef uint32_t (*font_err_fn)(void *);
static uint32_t font_engine_error(void *self){
 static font_err_fn f;if(!f)f=(font_err_fn)runtime_symbol("_ZN2nn6fontll18ScalableFontEngine8GetErrorEv");
 return f?f(self):0xffffffffu;}
static uint32_t h_font_load(void *self, char *name, const void *data, unsigned index, unsigned cap){
 static font_load_fn f;if(!f)f=(font_load_fn)runtime_symbol("_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj");
 /* The shared font is served as the decoded BFTTF marker + TTF. A caller that hands that header address to
    LoadFont must get the TTF payload; this wrapper runs on the host stack, so the adjust costs the guest thread
    nothing (the hooked-sdk-text route hung the font init on small stacks). */
 int shared = data && region && data == (const void *)region;
 if (shared) data = region + 8;
 uint32_t r=f?f(self,name,data,index,cap):0xffffffffu;
 if (shared) fprintf(stderr,"[font] LoadFont: shared font header -> TTF payload %p\n", data);
 fprintf(stderr,"[font] LoadFont self=%p name=%s data=%p index=%u cap=%u -> %#x error=%#x\n",self,name?name:"-",data,index,cap,r,font_engine_error(self));
 return r;}
typedef unsigned (*font_check_fn)(void *, unsigned);
typedef void *(*font_acq_fn)(void *, unsigned, unsigned);
static unsigned h_font_check(void *self, unsigned ch){
 static font_check_fn f;if(!f)f=(font_check_fn)runtime_symbol("_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj");
 unsigned r=f?f(self,ch):0;
 static unsigned n;if(n++<300)fprintf(stderr,"[font] CheckGlyphExist self=%p ch=%#x -> %u\n",self,ch,r);
 return r;}
static void *h_font_acq(void *self, unsigned ch, unsigned sz){
 static font_acq_fn f;if(!f)f=(font_acq_fn)runtime_symbol("_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt");
 void *p=f?f(self,ch,sz):NULL;
 static unsigned n;if(n++<60){fprintf(stderr,"[font] AcquireGlyphmap self=%p ch=%#x sz=%u -> %p",self,ch,sz,p);
  if(p){uint8_t d[0x40];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),(uintptr_t)p,sizeof d,(uintptr_t)d,&got)==KERN_SUCCESS&&got==sizeof d){fprintf(stderr,":");for(int i=0;i<0x40;i+=8){uint64_t v;memcpy(&v,d+i,8);fprintf(stderr," %llx",(unsigned long long)v);}}}
  fprintf(stderr,"\n");}
 return p;}
typedef const void *(*font_decode_fn)(const void *, unsigned);
static const void *h_font_decode(const void *addr, unsigned size){
 static font_decode_fn f;if(!f)f=(font_decode_fn)runtime_symbol("_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj");
 const void *r=f?f(addr,size):addr;
 fprintf(stderr,"[font] Decode addr=%p size=%u -> %p%s\n",addr,size,r,(const uint8_t*)addr==region?" (shared font)":"");
 return r;}
static uint32_t h_font_set(void *self, char *name){
 static font_set_fn f;if(!f)f=(font_set_fn)runtime_symbol("_ZN2nn6fontll18ScalableFontEngine7SetFontEPc");
 uint32_t r=f?f(self,name):0xffffffffu;
 fprintf(stderr,"[font] SetFont self=%p name=%s -> %#x error=%#x\n",self,name?name:"-",r,font_engine_error(self));
 return r;}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",get_shared_font},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;
 /* PLA_FONT_ENGINE=1: wrap the SDK's scalable-font entry points to log their arguments, return codes and the
    engine's error state. The game's own BFOTF fonts are decoded to valid OTF in memory yet every dynamic glyph
    falls back to the ellipsis, so this separates "the engine rejects the font" from "it loads it and the glyph
    build fails". The wrappers call the SDK's real functions (runtime_symbol) and change no behaviour. */
 if(getenv("PLA_FONT_ENGINE")){
  if(!strcmp(name,"_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj"))return h_font_load;
  if(!strcmp(name,"_ZN2nn6fontll18ScalableFontEngine7SetFontEPc"))return h_font_set;
  if(!strcmp(name,"_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj"))return h_font_decode;
  if(!strcmp(name,"_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj"))return h_font_check;
  if(!strcmp(name,"_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt"))return h_font_acq;
 }
 return NULL;
}

/* Offline native-name validation uses an empty profanity dictionary. This
   is explicit platform behavior, not an emulation of Nintendo's word list. */
#include "runtime.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
static void construct(void *self){
 void(*original)(void*)=runtime_symbol("_ZN2nn3ngc15ProfanityFilterC1Ev");
 if(!original)runtime_fail("ProfanityFilter constructor missing");original(self);
}
static void finalize(void *self){(void)self;}
static unsigned initialize(void *self,void *work,size_t size){
 (void)self;if(!work||!size)runtime_fail("Text-filter work buffer missing");
 fprintf(stderr,"[tomodachi] local name filter: empty profanity dictionary\n");return 0;
}
static unsigned version(void *self){(void)self;return 0;}
static unsigned check16(void *self,unsigned *out,unsigned flags,const uint16_t **words,size_t count){
 (void)self;(void)flags;if((!out||!words)&&count)runtime_fail("Text-filter word array missing");
 for(size_t i=0;i<count;i++){if(!words[i])runtime_fail("Text-filter word missing");out[i]=0;}return 0;
}
static unsigned check8(void *self,unsigned *out,unsigned flags,const char **words,size_t count){
 (void)self;(void)flags;if((!out||!words)&&count)runtime_fail("Text-filter word array missing");
 for(size_t i=0;i<count;i++){if(!words[i])runtime_fail("Text-filter word missing");out[i]=0;}return 0;
}
static int count_numbers(const char *text){
 if(!text)return 0;CFStringRef string=CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);if(!string)return 0;
 CFCharacterSetRef digits=CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);int count=0;
 for(CFIndex i=0;i<CFStringGetLength(string);i++)if(CFCharacterSetIsCharacterMember(digits,CFStringGetCharacterAtIndex(string,i)))count++;
 CFRelease(string);return count;
}
void *runtime_title_filter_override(const char *name){
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn3ngc15ProfanityFilterC1Ev",construct},
 {"_ZN2nn3ngc15ProfanityFilterD1Ev",finalize},
 {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv",finalize},
 {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm",initialize},
 {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv",version},
 {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm",check16},
 {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm",check8},
 {"_ZN2nn3ngc12CountNumbersEPKc",count_numbers}};
 for(unsigned i=0;i<sizeof map/sizeof *map;i++)if(!strcmp(name,map[i].name))return map[i].fn;return NULL;
}

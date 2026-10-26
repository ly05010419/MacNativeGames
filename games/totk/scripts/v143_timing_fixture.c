/* Apply production v143 code patches to a private local image for instruction
 * emulation. No guest entry, graphics, services, or user saves are executed. */
#include "runtime.h"
#include "game_patches.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
void *runtime_symbol(const char *n){(void)n;return 0;}
int main(int argc,char **argv){
 if(argc<4)return 2;
 FILE *f=fopen(argv[1],"rb");struct stat st;
 if(!f||fstat(fileno(f),&st))return 2;
 size_t data=((size_t)st.st_size+0x3fff)&~(size_t)0x3fff,size=data+0x4000;
 unsigned char *base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
 if(base==MAP_FAILED||fread(base,1,(size_t)st.st_size,f)!=(size_t)st.st_size)return 2;fclose(f);
 Module m={.name="main",.base=base,.file_size=(size_t)st.st_size,.size=size,.text_end=strtoull(argv[3],NULL,0)};
 runtime_game_patches(&m);runtime_frame_step_pin(10.0);
 float values[3];memcpy(values,base+data,sizeof values);
 printf("{\"pin\":[%.9g,%.9g,%.9g]",values[0],values[1],values[2]);
 runtime_frame_step_pin(0);runtime_frame_step_update(1000000000ULL);runtime_frame_step_update(1025000000ULL);
 memcpy(values,base+data,sizeof values);printf(",\"sample\":[%.9g,%.9g,%.9g]",values[0],values[1],values[2]);
 /* Each further argument is a repeating pattern of present intervals in ns ("4166667,12500000"): 6000 presents after a
    gap that empties the sample ring; prints the real time and the game time (the sum of the stored frame steps). */
 printf(",\"realtime\":[");
 for(int a=4;a<argc;a++){
  uint64_t pattern[16];unsigned n=0;char *p=argv[a];
  while(*p&&n<16){pattern[n++]=strtoull(p,&p,10);if(*p==',')p++;}
  if(!n)return 2;
  uint64_t t=1000000000ULL*(uint64_t)(a+1);runtime_frame_step_update(t);t+=1000000000ULL;runtime_frame_step_update(t);
  double real=0,game=0;
  for(unsigned i=0;i<6000;i++){t+=pattern[i%n];runtime_frame_step_update(t);memcpy(values,base+data,sizeof values);real+=pattern[i%n]*1e-9;game+=values[1];}
  printf("%s[%.9f,%.9f]",a>4?",":"",real,game);
 }
 printf("]}\n");
 f=fopen(argv[2],"wb");if(!f||fwrite(base,1,size,f)!=size||fclose(f))return 2;
 munmap(base,size);return 0;
}

/* CPU-only regression: an authored sparse data fixture, never executable game code. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
typedef struct {const char *name;unsigned char *base;uint64_t file_size,text_end,ro_off,size;} Module;
#define CODE_PATCHED_AT_BUILD 0
static jmp_buf fail_jump;
static int armed,writes,checks;
static const char *last_failure;
_Noreturn static void runtime_fail(const char *m){last_failure=m;if(armed)longjmp(fail_jump,1);fprintf(stderr,"unexpected runtime_fail: %s\n",m);exit(90);}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"CHECK FAILED line%d: %s\n",__LINE__,#x);exit(91);}}while(0)

#include "patch_helper.h"

static void tracked_put_word(Module *m,uint64_t off,uint32_t word,const char *what){writes++;put_word(m,off,word,what);}
#define put_word tracked_put_word

#include "heap_helper.h"

#undef put_word
static int invoke(Module *m){armed=1;writes=0;last_failure=NULL;int failed=setjmp(fail_jump);if(!failed)patch_area_merge_heap(m);armed=0;return failed;}
int main(int argc,char **argv){
 CHECK(argc==2);int fd=open(argv[1],O_RDONLY);CHECK(fd>=0);struct stat st;CHECK(!fstat(fd,&st));size_t bytes=(size_t)st.st_size;
 const unsigned char *actual=mmap(NULL,bytes,PROT_READ,MAP_PRIVATE,fd,0);CHECK(actual!=MAP_FAILED);close(fd);
 unsigned char *image=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);CHECK(image!=MAP_FAILED);memcpy(image,actual,bytes);
 unsigned char *before=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);CHECK(before!=MAP_FAILED);
 Module m={.name="main",.base=image,.file_size=bytes,.text_end=0x2aa6a10,.ro_off=0x2aa7000,.size=bytes};
#if defined(TOTK_GAME_V121) || TARGET_OS_IPHONE
 CHECK(!invoke(NULL)&&writes==0);
 CHECK(!invoke(&m)&&writes==0&&!memcmp(image,actual,bytes));
 m.name="sdk";m.base=NULL;m.file_size=0;m.text_end=0;CHECK(!invoke(&m)&&writes==0);
 printf("PASS excluded platform/version: %d checks, no mutation, no Module dereference\n",checks);
#else
 const size_t start=0xec1f60,site=0xec1f78,name=0x3710a05;const size_t name_bytes=sizeof("AreaMergeTarRenderer");
 memcpy(before,image,bytes);
 if(getenv("TEST_HEAP_OFF")){CHECK(!invoke(&m));CHECK(writes==0&&!memcmp(image,actual,bytes));puts("PASS disabled: original heap unchanged");return 0;}
 CHECK(!invoke(&m));CHECK(writes==1);uint32_t word;memcpy(&word,image+site,4);CHECK(word==0x52a00020);
 CHECK(!memcmp(image,before,site));CHECK(!memcmp(image+site+4,before+site+4,bytes-site-4));
 memcpy(before,image,bytes);CHECK(invoke(&m));CHECK(writes==0&&!memcmp(image,before,bytes)); // non-idempotent mismatch is strict
 memcpy(image,actual,bytes);
 // Every byte of the source's twelve instruction guards and terminated name.
 for(unsigned block=0;block<2;block++)for(size_t b=0;b<(block?name_bytes:48);b++){
  size_t at=(block?name:start)+b;image[at]^=1;memcpy(before,image,bytes);
  CHECK(invoke(&m));CHECK(writes==0&&last_failure);CHECK(!memcmp(image,before,bytes));image[at]^=1;
 }
 const char *wrong[]={"sdk","subsdk0","","MAIN"};
 for(unsigned i=0;i<4;i++){m.name=wrong[i];memcpy(before,image,bytes);CHECK(invoke(&m));CHECK(!writes&&!memcmp(image,before,bytes));}m.name="main";
 uint64_t files[]={0,start,start+47,name,name+name_bytes-1};
 for(unsigned i=0;i<sizeof files/sizeof *files;i++){m.file_size=files[i];memcpy(before,image,bytes);CHECK(invoke(&m));CHECK(!writes&&!memcmp(image,before,bytes));}m.file_size=bytes;
 uint64_t texts[]={0,start,start+47};
 for(unsigned i=0;i<sizeof texts/sizeof *texts;i++){m.text_end=texts[i];memcpy(before,image,bytes);CHECK(invoke(&m));CHECK(!writes&&!memcmp(image,before,bytes));}
 // Rejection occurs before *any* image access when bounds/module fail.
 void *inaccessible=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);CHECK(inaccessible!=MAP_FAILED);
 m.base=inaccessible;m.text_end=start+47;CHECK(invoke(&m)&&!writes);
 m.text_end=0x2aa6a10;m.file_size=name+name_bytes-1;CHECK(invoke(&m)&&!writes);
 m.name="sdk";m.file_size=bytes;CHECK(invoke(&m)&&!writes);CHECK(!munmap(inaccessible,4096));
 // Exact boundary accepted; only the intended single word is changed.
 m.name="main";m.base=image;m.text_end=start+48;m.file_size=name+name_bytes;CHECK(!invoke(&m)&&writes==1);
 CHECK(!memcmp(image,actual,site));CHECK(!memcmp(image+site+4,actual+site+4,bytes-site-4));
 printf("PASS v0: %d checks; synthetic fixture, single-word delta, all byte mismatches/truncations/name/module/version-contract guards\n",checks);
#endif
 CHECK(!munmap(before,bytes));CHECK(!munmap(image,bytes));CHECK(!munmap((void*)actual,bytes));return 0;
}

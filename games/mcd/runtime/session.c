/* Boot-only writable resources. Direct/headless launches follow the same ownership
   rules as app launches. Explicit shared cache/input paths fail closed, never race. */
#include "runtime.h"
#include "fs_host.h"
#include "user_lock.h"
#include <string.h>
#include <limits.h>

static void directory(const char *path){
 char copy[PATH_MAX];
 if(!path||!*path||snprintf(copy,sizeof copy,"%s",path)>=(int)sizeof copy)runtime_fail("session directory path");
 for(char *p=copy+1;;p++){
  if(*p!='/'&&*p)continue;
  char saved=*p;*p=0;
  if(mkdir(copy,0700)&&errno!=EEXIST)runtime_fail("create session directory");
  struct stat st;
  /* macOS /var and /tmp are system symlinks; permit ancestors but reject
     a substituted final cache directory. mng_lock_file pins that final inode. */
  if((saved?stat(copy,&st):lstat(copy,&st))||!S_ISDIR(st.st_mode))runtime_fail("invalid session directory");
  *p=saved;if(!saved)break;
 }
}
void runtime_session_prepare(void){
 static int ready;if(ready)return;
 uint64_t uid[2];runtime_account_uid(uid);
 const char *profile=getenv("TOTK_PROFILE_DIR");if(!profile)profile="user-data";
 char path[PATH_MAX];
 if(!getenv("TOTK_SHADER_CACHE")){
  if(snprintf(path,sizeof path,"%s/runtime-users/%02llu/shader-cache",profile,(unsigned long long)uid[0])>=(int)sizeof path)runtime_fail("shader cache path");
  if(setenv("TOTK_SHADER_CACHE",path,1))runtime_fail("set shader cache path");
 }
 const char *cache=getenv("TOTK_SHADER_CACHE");directory(cache);
 /* Never release this descriptor: the kernel closes it on exit/crash. Shader
    worker threads may still write after any account handle has been closed. */
 if(mng_lock_file(cache,".runtime-owner.lock",LOCK_EX|LOCK_NB)<0)
  runtime_fail("shader cache already in use or cannot be locked; use a separate cache directory");
 fprintf(stderr,"[session] user %llu owns shader cache %s\n",(unsigned long long)uid[0],cache);
 ready=1;
}

/* Local read-only Mii database. The player's SDK supplies default appearance
   data; this host supplies stable local identities and IPC framing.
   Command framing: switchbrew/libnx nx/source/services/mii.c (ISC). */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t a,b;} CreateId;
typedef struct {uint16_t text[11];} Nickname;
typedef struct {uint8_t bytes[0x58];} CharInfo;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static CharInfo defaults[6];
static int ready;
static void ensure_defaults(void) {
 pthread_mutex_lock(&mutex);
 if(!ready){
  typedef void(*SetDefault)(void*,int,CreateId,int,Nickname);
  SetDefault set=(SetDefault)runtime_symbol("_ZN2nn3mii6detail11CharInfoRaw13SetDefaultMiiEiNS0_8CreateIdENS0_13FavoriteColorENS0_8NicknameE");
  if(!set)runtime_fail("Mii default builder missing from pinned SDK");
  for(int i=0;i<6;i++){
   CreateId id={0x4d4e470000004000ULL+(uint64_t)i,0x80544f4d4f444183ULL};
   Nickname nickname={{'M','i','i',' ',(uint16_t)('1'+i),0}};
   set(&defaults[i],i,id,i,nickname);
   typedef unsigned char(*IsValid)(const void*);
   IsValid valid=(IsValid)runtime_symbol("_ZNK2nn3mii8CreateId7IsValidEv");
   if(!valid||!valid(defaults[i].bytes))runtime_fail("Generated Mii identity failed the SDK validity check");
  }
  ready=1;
 }
 pthread_mutex_unlock(&mutex);
}
static int count(uint32_t flags){return flags&2?6:0;}
static void database(IpcSession *session,const IpcRequest *rq,IpcResponse *rs){
 fprintf(stderr,"[tomodachi-mii] command %u\n",rq->cmd);
 switch(rq->cmd){
 case 0:{uint8_t updated=0;IPC_OUT(rs,updated);return;}
 case 1:{uint8_t full=0;IPC_OUT(rs,full);return;}
 case 2:{int32_t n=count(IPC_ARG(rq,uint32_t,0));IPC_OUT(rs,n);return;}
 case 4:{
  uint32_t flags=IPC_ARG(rq,uint32_t,0);
  if(rq->nb!=1)runtime_fail("Mii Get: expected one output buffer");
  int32_t n=count(flags);if((uint64_t)n>rq->b[0].size/sizeof(CharInfo))n=(int32_t)(rq->b[0].size/sizeof(CharInfo));
  if(n){ensure_defaults();memcpy(ipc_guest(rq->b[0].addr,(uint64_t)n*sizeof(CharInfo)),defaults,(size_t)n*sizeof(CharInfo));}
  IPC_OUT(rs,n);return;
 }
 case 7:{int32_t index=IPC_ARG(rq,int32_t,0);if(index<0||index>=6)runtime_fail("Mii default index out of range");ensure_defaults();ipc_out(rs,&defaults[index],sizeof(CharInfo));return;}
 case 22:{uint32_t version=IPC_ARG(rq,uint32_t,0);if(version>4)runtime_fail("Unsupported Mii interface version");return;}
 default:ipc_unimplemented(session,rq);
 }
}
static void root(IpcSession *session,const IpcRequest *rq,IpcResponse *rs){
 if(rq->cmd!=0)ipc_unimplemented(session,rq);
 uint32_t key=IPC_ARG(rq,uint32_t,0);
 if(key!=0&&key!=0xa523b78f)runtime_fail("Unknown Mii database key code");
 rs->move[rs->nmove++]=runtime_ipc_new_session("Tomodachi Mii database",database,NULL,NULL);
}
__attribute__((constructor))static void register_mii(void){
 runtime_ipc_register("mii:e",root,NULL,NULL);
 runtime_ipc_register("mii:u",root,NULL,NULL);
}

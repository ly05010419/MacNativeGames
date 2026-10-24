#pragma once
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
/* Player actions are separate from raw controller/UI input. The loader installs
   an immutable translator. A sequence-checked table publishes coherent layouts. */
static RuntimeActionBindings actionRequest;
typedef struct { _Atomic uint32_t sequence;uint32_t reserved;_Atomic uint16_t target[ACTION_BINDING_COUNT]; } ActionLive;
_Static_assert(offsetof(ActionLive,target)==8&&sizeof(ActionLive)==40,"guest action table ABI");
static _Atomic(ActionLive *) actionPage;
/* The mutex serializes writers. Odd/even sequence numbers let guest readers
   retry a live update without taking a native lock or changing executable code. */
static void action_publish(ActionLive *page,const RuntimeActionBindings *b){
 atomic_fetch_add_explicit(&page->sequence,1,memory_order_acq_rel);
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)atomic_store_explicit(&page->target[i],b->target[i],memory_order_relaxed);
 atomic_fetch_add_explicit(&page->sequence,1,memory_order_release);
}
static _Atomic int actionPatchFailed;
static pthread_once_t actionOnce=PTHREAD_ONCE_INIT;
static pthread_mutex_t actionLock=PTHREAD_MUTEX_INITIALIZER;
static int action_version_supported(void){
#if TARGET_OS_OSX && !defined(GAME_BOTW) && !defined(GAME_BOTW160) && !defined(TOTK_GAME_V112) && !defined(TOTK_GAME_V121)
 return getenv("TOTK_EXPORT_IOS")==NULL;
#else
 return 0;
#endif
}
int runtime_action_bindings_supported(void){return action_version_supported();}
static int actionConfigured;
static int action_settings_path(char *out,size_t cap){
 const char *controls=getenv("TOTK_CONTROLS");if(!controls||!*controls)return 0;
 const char *slash=strrchr(controls,'/');int n=slash?snprintf(out,cap,"%.*s/player-actions.txt",(int)(slash-controls),controls):snprintf(out,cap,"player-actions.txt");
 return n>=0&&(size_t)n<cap;
}
static void action_initialize(void){
 action_bindings_default(&actionRequest);if(!action_version_supported())return;
 char path[4096];if(action_settings_path(path,sizeof path)){
  FILE *f=fopen(path,"rb");if(f){char text[512];size_t n=fread(text,1,sizeof text-1,f);int whole=feof(f);fclose(f);text[n]=0;
   while(n&&(text[n-1]=='\n'||text[n-1]=='\r'))text[--n]=0;
   RuntimeActionBindings next;if(whole&&!memchr(text,0,n)&&action_bindings_parse(text,&next)){actionRequest=next;actionConfigured=1;}
   else fprintf(stderr,"[totk] invalid player-actions.txt; original actions retained\n");
  }
 }
 const char *e=getenv("TOTK_ACTION_LAYOUT");
 if(e&&*e&&strcasecmp(e,"switch")&&strcmp(e,"0")){
  if(!strcasecmp(e,"xbox")){action_bindings_xbox(&actionRequest);actionConfigured=1;}
  else fprintf(stderr,"[totk] unknown TOTK_ACTION_LAYOUT; player actions keep the original layout\n");
 }
 const char *m=getenv("TOTK_ACTION_MAP");
 if(m&&*m){RuntimeActionBindings b=actionRequest;if(action_bindings_parse(m,&b)){actionRequest=b;actionConfigured=1;}else fprintf(stderr,"[totk] TOTK_ACTION_MAP ignored: expected 16 (or legacy 14) comma-separated action codes\n");}
}
int runtime_action_bindings_enabled(void){
 if(!action_version_supported())return 0;pthread_once(&actionOnce,action_initialize);
 const char *e=getenv("TOTK_PLAYER_ACTIONS");return e?!strcmp(e,"1"):actionConfigured;
}
void runtime_action_bindings_get(RuntimeActionBindings *b){pthread_once(&actionOnce,action_initialize);pthread_mutex_lock(&actionLock);*b=actionRequest;pthread_mutex_unlock(&actionLock);}
int runtime_action_bindings_available(void){return atomic_load(&actionPage)!=NULL;}
int runtime_action_bindings_set(const RuntimeActionBindings *b){
 if(!b||!action_version_supported()||atomic_load(&actionPatchFailed)||!action_bindings_valid(b))return 0;
 pthread_once(&actionOnce,action_initialize);pthread_mutex_lock(&actionLock);actionRequest=*b;
 ActionLive *page=atomic_load(&actionPage);if(page)action_publish(page,b);
 pthread_mutex_unlock(&actionLock);runtime_input_clear();
 fprintf(stderr,"[totk] player actions: A=%s B=%s X=%s Y=%s L=%s R=%s ZL=%s ZR=%s Up=%s Down=%s Left=%s Right=%s LStick=%s RStick=%s Plus=%s Minus=%s\n",action_binding_name(b->target[0]),action_binding_name(b->target[1]),action_binding_name(b->target[2]),action_binding_name(b->target[3]),action_binding_name(b->target[4]),action_binding_name(b->target[5]),action_binding_name(b->target[6]),action_binding_name(b->target[7]),action_binding_name(b->target[8]),action_binding_name(b->target[9]),action_binding_name(b->target[10]),action_binding_name(b->target[11]),action_binding_name(b->target[12]),action_binding_name(b->target[13]),action_binding_name(b->target[14]),action_binding_name(b->target[15]));return 1;
}
void runtime_action_bindings_attach(void *word){
 if(!word){pthread_once(&actionOnce,action_initialize);pthread_mutex_lock(&actionLock);action_bindings_default(&actionRequest);atomic_store(&actionPage,NULL);atomic_store(&actionPatchFailed,1);pthread_mutex_unlock(&actionLock);return;}
 pthread_once(&actionOnce,action_initialize);pthread_mutex_lock(&actionLock);
 ActionLive *page=word;action_publish(page,&actionRequest);
 atomic_store(&actionPage,page);pthread_mutex_unlock(&actionLock);
}

#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
void *runtime_title_keyboard_override(const char*);
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int typed,accept,initialized,changes,entered,finished,cancels,shows,hide_requested;
static void *active_context;
static pthread_t guest;
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"%s\n",message);abort();}
void runtime_front_text_test(const char *op,const char *text){(void)op;(void)text;}
void runtime_front_text_cancel(void *context){pthread_mutex_lock(&lock);assert(context==active_context);hide_requested=1;pthread_cond_signal(&condition);pthread_mutex_unlock(&lock);}
int runtime_front_text_input(const RuntimeTextRequest *req,char *out,size_t size){
 assert(req->max_units==10);assert(req->cancelled);req->changed("猫",req->context);
 pthread_mutex_lock(&lock);active_context=req->context;shows++;typed=1;pthread_cond_signal(&condition);
 while(!accept&&!hide_requested)pthread_cond_wait(&condition,&lock);int hidden=hide_requested;pthread_mutex_unlock(&lock);
 if(hidden){assert(req->cancelled(req->context));return 0;}
 snprintf(out,size,"猫咪");return 1;
}
static void init_callback(void){assert(pthread_equal(guest,pthread_self()));initialized++;}
static void changed_callback(const void *arg){assert(pthread_equal(guest,pthread_self()));const uint16_t *text=arg;assert(text[0]==0x732b);changes++;}
static void enter_callback(const void *arg){assert(pthread_equal(guest,pthread_self()));const uint16_t *text=arg;uint32_t length;memcpy(&length,(const uint8_t*)arg+1004,4);assert(length==2&&text[0]==0x732b&&text[1]==0x54aa);entered++;}
static void finished_callback(void){finished++;}
static void cancelled_callback(void){cancels++;}
#define KB(method) runtime_title_keyboard_override("_ZN2nn5swkbd14InlineKeyboard" method)
int main(void){
 guest=pthread_self();uint64_t object[2];char work[0x6e0];
 ((void(*)(void*))KB("C1Ev"))(object);
 assert(((unsigned char(*)(void*,void*))KB("10InitializeEPv"))(object,work));
 ((void(*)(void*,void(*)(void)))KB("29SetFinishedInitializeCallbackEPFvvE"))(object,init_callback);
 ((void(*)(void*,void(*)(const void*)))KB("24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE"))(object,changed_callback);
 ((void(*)(void*,void(*)(const void*)))KB("23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE"))(object,enter_callback);
 ((void(*)(void*,void(*)(void)))KB("27SetFinishedKeyboardCallbackEPFvvE"))(object,finished_callback);
 ((void(*)(void*,void(*)(void)))KB("24SetDecidedCancelCallbackEPFvvE"))(object,cancelled_callback);
 void(*calc)(void*)=KB("4CalcEv");calc(object);assert(initialized==1);calc(object);assert(initialized==1);
 uint8_t arg[0x48]={0};int max=10;memcpy(arg+0x20,&max,4);
 ((void(*)(void*,const void*))KB("6AppearERKNS0_9AppearArgE"))(object,arg);
 pthread_mutex_lock(&lock);while(!typed)pthread_cond_wait(&condition,&lock);pthread_mutex_unlock(&lock);
 calc(object);assert(changes==1&&entered==0);
 /* Guest echo and repeated Appear must not recreate the native field or
    repeatedly notify the title of the same text. */
 uint8_t text[0x3f0]={0};uint16_t cat=0x732b;uint32_t one=1;memcpy(text,&cat,2);memcpy(text+1004,&one,4);
 ((void(*)(void*,const void*))KB("12SetInputTextERKNS0_9InputTextE"))(object,text);
 ((void(*)(void*,const void*))KB("6AppearERKNS0_9AppearArgE"))(object,arg);calc(object);assert(changes==1&&shows==1);
 pthread_mutex_lock(&lock);accept=1;pthread_cond_signal(&condition);pthread_mutex_unlock(&lock);
 for(int i=0;i<100&&!entered;i++){usleep(1000);calc(object);}
 assert(entered==1&&finished==1&&changes==2);
 pthread_mutex_lock(&lock);typed=accept=0;pthread_mutex_unlock(&lock);
 ((void(*)(void*,const void*))KB("6AppearERKNS0_9AppearArgE"))(object,arg);
 pthread_mutex_lock(&lock);while(!typed)pthread_cond_wait(&condition,&lock);pthread_mutex_unlock(&lock);
 ((void(*)(void*))KB("9DisappearEv"))(object);
 ((void(*)(void*))KB("9DisappearEv"))(object);
 for(int i=0;i<100&&finished<2;i++){usleep(1000);calc(object);}
 assert(finished==2&&cancels==0&&entered==1&&shows==2);
 for(int i=0;i<10;i++)calc(object);assert(finished==2);
 ((void(*)(void*))KB("8FinalizeEv"))(object);assert(!object[0]);
 puts("Inline keyboard Unicode, guest echo, repeated appearance and programmatic-hide lifecycle tests passed");
}

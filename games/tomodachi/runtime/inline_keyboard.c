/* Native inline text entry. Callback payload sizes are verified against the
   pinned SDK: Changed 0x3fc, Moved 0x3f4, Enter 0x3f0, object 16 bytes. */
#include "runtime.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint16_t text[502];uint32_t length;} EnterArg;
typedef struct {EnterArg input;int32_t cursor;} CursorArg;
typedef struct {EnterArg input;int32_t start,end,cursor;} ChangedArg;
_Static_assert(sizeof(EnterArg)==0x3f0&&sizeof(CursorArg)==0x3f4&&sizeof(ChangedArg)==0x3fc,"Inline callback ABI");
typedef struct {
 pthread_mutex_t lock;_Atomic int refs;
 EnterArg text;int cursor,initialized,init_pending,changed,pending,active,closed,image_dirty,hiding;
 int max,min,digits;uint32_t disabled;
 void(*init_cb)(void),(*finish_cb)(void),(*cancel_cb)(void);
 void(*change_cb)(const ChangedArg*);void(*cursor_cb)(const CursorArg*);void(*enter_cb)(const EnterArg*);
} Keyboard;
static Keyboard *state(void *object){Keyboard *s=NULL;if(object)memcpy(&s,object,8);return s;}
static void release(Keyboard *s){if(atomic_fetch_sub(&s->refs,1)==1){pthread_mutex_destroy(&s->lock);free(s);}}
static void utf8(const EnterArg *text,char *out,size_t capacity){
 CFStringRef string=CFStringCreateWithCharacters(NULL,text->text,text->length);
 if(!string||!CFStringGetCString(string,out,(CFIndex)capacity,kCFStringEncodingUTF8))out[0]=0;
 if(string)CFRelease(string);
}
static void utf16(EnterArg *out,const char *text){
 CFStringRef string=CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);memset(out,0,sizeof *out);
 if(!string)return;CFIndex n=CFStringGetLength(string);if(n>500)n=500;
 CFStringGetCharacters(string,CFRangeMake(0,n),out->text);out->length=(uint32_t)n;CFRelease(string);
}
static void changed(const char *text,void *context){Keyboard *s=context;pthread_mutex_lock(&s->lock);
 if(!s->closed&&!s->hiding){EnterArg value;utf16(&value,text);if(memcmp(&s->text,&value,sizeof value)){s->text=value;s->cursor=(int)s->text.length;s->changed=1;}}pthread_mutex_unlock(&s->lock);}
static int cancelled(void *context){Keyboard *s=context;pthread_mutex_lock(&s->lock);int value=s->closed||s->hiding;pthread_mutex_unlock(&s->lock);return value;}
static void *input_worker(void *context){
 Keyboard *s=context;char initial[2048],answer[2048];RuntimeTextRequest request={0};
 pthread_mutex_lock(&s->lock);utf8(&s->text,initial,sizeof initial);
 request=(RuntimeTextRequest){"Enter text",initial,s->max,s->min,0,s->digits,s->disabled,changed,s,cancelled};pthread_mutex_unlock(&s->lock);
 int accepted=runtime_front_text_input(&request,answer,sizeof answer);
 pthread_mutex_lock(&s->lock);
 if(!s->closed){if(accepted&&!s->hiding){utf16(&s->text,answer);s->cursor=(int)s->text.length;s->changed=1;}s->pending=s->hiding?3:accepted?1:2;}
 s->active=0;pthread_mutex_unlock(&s->lock);release(s);return NULL;
}
static void construct(void *object){memset(object,0,16);}
static size_t work_size(void){return 0x6e0;}
static void finalize(void *object){Keyboard *s=state(object);if(!s)return;memset(object,0,16);
 pthread_mutex_lock(&s->lock);s->closed=1;int active=s->active;pthread_mutex_unlock(&s->lock);
 if(active)runtime_front_text_cancel(s);release(s);
}
static unsigned char initialize(void *object,void *work){
 if(!object||!work)return 0;finalize(object);Keyboard *s=calloc(1,sizeof *s);if(!s)runtime_fail("Inline keyboard allocation");
 pthread_mutex_init(&s->lock,NULL);atomic_init(&s->refs,1);s->max=500;s->initialized=1;s->init_pending=1;s->image_dirty=1;
 memcpy(object,&s,8);return 1;
}
static void image_requirement(void *object,size_t *size,size_t *alignment){(void)object;if(size)*size=1280*720*4;if(alignment)*alignment=4096;}
static void window_size(const void *object,int *w,int *h){(void)object;if(w)*w=1280;if(h)*h=720;}
static unsigned char get_image(void *object,void *out,size_t size){
 Keyboard *s=state(object);if(!s||!out||size<1280*720*4)return 0;
 pthread_mutex_lock(&s->lock);int dirty=s->image_dirty;s->image_dirty=0;pthread_mutex_unlock(&s->lock);
 if(dirty)memset(out,0,1280*720*4);return (unsigned char)dirty;
}
static void input_text(void *object,const void *text){Keyboard *s=state(object);if(!s||!text)return;
 pthread_mutex_lock(&s->lock);memcpy(&s->text,text,sizeof s->text);if(s->text.length>500)runtime_fail("Inline input length");
 /* Guest-set text is not a new user edit; echoing it back creates a feedback
    loop when the title synchronizes its field from ChangedString. */
 pthread_mutex_unlock(&s->lock);}
static void cursor_pos(void *object,int cursor){Keyboard *s=state(object);if(!s)return;
 pthread_mutex_lock(&s->lock);s->cursor=cursor<0?0:cursor>(int)s->text.length?(int)s->text.length:cursor;pthread_mutex_unlock(&s->lock);}
static void appear(void *object,const uint8_t *arg){Keyboard *s=state(object);if(!s||!arg)return;
 int32_t type,max,min;uint32_t disabled;memcpy(&type,arg,4);memcpy(&disabled,arg+0x1c,4);memcpy(&max,arg+0x20,4);memcpy(&min,arg+0x24,4);
 pthread_mutex_lock(&s->lock);if(s->active||s->pending||s->closed){pthread_mutex_unlock(&s->lock);return;}
 s->hiding=0;
 s->max=max>0&&max<=500?max:500;s->min=min>0&&min<=s->max?min:0;s->digits=type==1;s->disabled=disabled;s->active=1;
 atomic_fetch_add(&s->refs,1);pthread_mutex_unlock(&s->lock);pthread_t worker;
 if(pthread_create(&worker,NULL,input_worker,s))runtime_fail("Inline input thread");pthread_detach(worker);
}
static void disappear(void *object){Keyboard *s=state(object);if(!s)return;pthread_mutex_lock(&s->lock);int active=s->active&&!s->hiding;if(active)s->hiding=1;pthread_mutex_unlock(&s->lock);if(active)runtime_front_text_cancel(s);}
static void calc(void *object){Keyboard *s=state(object);if(!s)return;atomic_fetch_add(&s->refs,1);
 pthread_mutex_lock(&s->lock);EnterArg text=s->text;int init=s->init_pending,change=s->changed,pending=s->pending,cursor=s->cursor;
 s->init_pending=s->changed=s->pending=0;
 void(*init_cb)(void)=s->init_cb;void(*finish_cb)(void)=s->finish_cb;void(*cancel_cb)(void)=s->cancel_cb;
 void(*change_cb)(const ChangedArg*)=s->change_cb;void(*cursor_cb)(const CursorArg*)=s->cursor_cb;void(*enter_cb)(const EnterArg*)=s->enter_cb;
 pthread_mutex_unlock(&s->lock);
 if(init&&init_cb)init_cb();
 if(change&&change_cb){ChangedArg arg={text,-1,-1,cursor};change_cb(&arg);}
 if(change&&cursor_cb){CursorArg arg={text,cursor};cursor_cb(&arg);}
 if(pending==1&&enter_cb)enter_cb(&text);if(pending==2&&cancel_cb)cancel_cb();
 if(pending&&finish_cb)finish_cb();release(s);
}
#define SET_CALLBACK(function,field,type) static void function(void *object,type callback){Keyboard*s=state(object);if(!s)return;pthread_mutex_lock(&s->lock);s->field=callback;pthread_mutex_unlock(&s->lock);}
typedef void(*VoidCallback)(void);typedef void(*ChangeCallback)(const ChangedArg*);typedef void(*CursorCallback)(const CursorArg*);typedef void(*EnterCallback)(const EnterArg*);
SET_CALLBACK(set_init,init_cb,VoidCallback)
SET_CALLBACK(set_finish,finish_cb,VoidCallback)
SET_CALLBACK(set_cancel,cancel_cb,VoidCallback)
SET_CALLBACK(set_changed,change_cb,ChangeCallback)
SET_CALLBACK(set_cursor,cursor_cb,CursorCallback)
SET_CALLBACK(set_enter,enter_cb,EnterCallback)
void *runtime_title_keyboard_override(const char *name){
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn5swkbd14InlineKeyboardC1Ev",construct},
 {"_ZN2nn5swkbd14InlineKeyboard8FinalizeEv",finalize},
 {"_ZN2nn5swkbd14InlineKeyboard25GetRequiredWorkBufferSizeEv",work_size},
 {"_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPmS2_",image_requirement},
 {"_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_",window_size},
 {"_ZN2nn5swkbd14InlineKeyboard10InitializeEPv",initialize},
 {"_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE",set_init},
 {"_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE",set_finish},
 {"_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE",set_changed},
 {"_ZN2nn5swkbd14InlineKeyboard22SetMovedCursorCallbackEPFvPKNS0_14MovedCursorArgEE",set_cursor},
 {"_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE",set_enter},
 {"_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE",set_cancel},
 {"_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE",input_text},
 {"_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi",cursor_pos},
 {"_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE",appear},
 {"_ZN2nn5swkbd14InlineKeyboard9DisappearEv",disappear},
 {"_ZN2nn5swkbd14InlineKeyboard4CalcEv",calc},
 {"_ZN2nn5swkbd14InlineKeyboard8GetImageEPvm",get_image}};
 for(unsigned i=0;i<sizeof map/sizeof *map;i++)if(!strcmp(name,map[i].name))return map[i].fn;return NULL;
}

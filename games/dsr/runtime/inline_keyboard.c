/* Dark Souls Remastered's inline keyboard (nn::swkbd::InlineKeyboard, SDK 4.5.0) answered on the host.

   Adapted from games/tomodachi/runtime/inline_keyboard.c (a newer SDK's payloads, verified there): on the console the
   inline keyboard is an applet whose picture reaches the game through a vi indirect layer. The SDK's own
   GetImageMemoryRequirement asks vi::GetIndirectImageMemoryRequirement, which this host does not serve: at boot the game
   stopped there (sdk+0x1d4050, a null vi session). Here the keyboard is the runtime's text bar in the game window
   (runtime_front_text_input): Appear opens it on a worker thread, Calc delivers the result to the game's callbacks on the
   game's own thread. This runtime's text bar has no cancel and no live "changed" callback (games/tomodachi's has both),
   so Disappear and Finalize only mark the state; an open bar stays until Return or Escape.

   Unverified for SDK 4.5.0 (no character has been named yet): the AppearArg offsets and the callback payload sizes are
   games/tomodachi's; SetUtf8Mode is logged, not applied. */
#include "runtime.h"
#include "game_version.h"
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint16_t text[502];uint32_t length;} EnterArg;
typedef struct {EnterArg input;int32_t cursor;} CursorArg;
typedef struct {EnterArg input;int32_t start,end,cursor;} ChangedArg;
_Static_assert(sizeof(EnterArg)==0x3f0&&sizeof(CursorArg)==0x3f4&&sizeof(ChangedArg)==0x3fc,"Inline callback ABI");

enum {IMAGE_W=1280,IMAGE_H=720,MAX_UNITS=500,WORK_SIZE=0x6e0};
enum {PENDING_NONE,PENDING_ENTER,PENDING_CANCEL};
enum {STATE_NONE=0,STATE_INITIALIZED=1,STATE_SHOWN=3,STATE_DISAPPEARING=4};

typedef struct {
 pthread_mutex_t lock;_Atomic int refs;
 EnterArg text;int cursor,init_pending,changed,pending,active,closed,image_dirty,hiding;
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
 if(!string)return;CFIndex n=CFStringGetLength(string);if(n>MAX_UNITS)n=MAX_UNITS;
 CFStringGetCharacters(string,CFRangeMake(0,n),out->text);out->length=(uint32_t)n;CFRelease(string);
}

static void *input_worker(void *context){
 Keyboard *s=context;char initial[2048],answer[2048];
 pthread_mutex_lock(&s->lock);utf8(&s->text,initial,sizeof initial);
 RuntimeTextRequest request={"Enter text",initial,s->max,s->min,0,s->digits,s->disabled};pthread_mutex_unlock(&s->lock);
 const char *headless_answer=getenv("TOTK_HEADLESS")?getenv("TOTK_KB_ANSWER"):NULL;
 int accepted=headless_answer?(snprintf(answer,sizeof answer,"%s",headless_answer),1):runtime_front_text_input(&request,answer,sizeof answer);
 pthread_mutex_lock(&s->lock);
 if(!s->closed){
  if(accepted&&!s->hiding){utf16(&s->text,answer);s->cursor=(int)s->text.length;s->changed=1;}
  s->pending=accepted&&!s->hiding?PENDING_ENTER:PENDING_CANCEL;
 }
 s->active=0;pthread_mutex_unlock(&s->lock);release(s);return NULL;
}

static void construct(void *object){if(object)memset(object,0,16);}
static size_t work_size(void){return WORK_SIZE;}
static void finalize(void *object){
 Keyboard *s=state(object);if(!s)return;memset(object,0,16);
 pthread_mutex_lock(&s->lock);s->closed=1;pthread_mutex_unlock(&s->lock);release(s);
}
static void destruct(void *object){finalize(object);}
static unsigned char initialize(void *object,void *work){
 if(!object||!work)return 0;finalize(object);Keyboard *s=calloc(1,sizeof *s);if(!s)runtime_fail("Inline keyboard allocation");
 pthread_mutex_init(&s->lock,NULL);atomic_init(&s->refs,1);s->max=MAX_UNITS;s->init_pending=1;s->image_dirty=1;
 memcpy(object,&s,8);return 1;
}
static void launch(void *object){(void)object;fprintf(stderr,"[" GAME_LOG "] inline keyboard: Launch (host text bar, nothing to start)\n");}
static void image_requirement(void *object,size_t *size,size_t *alignment){
 (void)object;if(size)*size=(size_t)IMAGE_W*IMAGE_H*4;if(alignment)*alignment=4096;
}
static unsigned char get_image(void *object,void *out,size_t size){
 Keyboard *s=state(object);if(!s||!out||size<(size_t)IMAGE_W*IMAGE_H*4)return 0;
 pthread_mutex_lock(&s->lock);int dirty=s->image_dirty;s->image_dirty=0;pthread_mutex_unlock(&s->lock);
 if(dirty)memset(out,0,(size_t)IMAGE_W*IMAGE_H*4);   /* transparent: the host draws its own bar over the game */
 return (unsigned char)dirty;
}
static void input_text(void *object,const void *text){
 Keyboard *s=state(object);if(!s||!text)return;
 pthread_mutex_lock(&s->lock);memcpy(&s->text,text,sizeof s->text);
 if(s->text.length>MAX_UNITS){pthread_mutex_unlock(&s->lock);runtime_fail("Inline input length");}
 pthread_mutex_unlock(&s->lock);
}
static void cursor_pos(void *object,int cursor){
 Keyboard *s=state(object);if(!s)return;
 pthread_mutex_lock(&s->lock);s->cursor=cursor<0?0:cursor>(int)s->text.length?(int)s->text.length:cursor;pthread_mutex_unlock(&s->lock);
}
static void utf8_mode(void *object,unsigned char on){(void)object;fprintf(stderr,"[" GAME_LOG "] inline keyboard: SetUtf8Mode(%d) (not applied)\n",on);}
static void window_mode(void *object,int mode){(void)object;(void)mode;}
static void keytop_scale(void *object,float scale){(void)object;(void)scale;}
static void keytop_translate(void *object,float x,float y){(void)object;(void)x;(void)y;}
static void appear(void *object,const uint8_t *arg){
 Keyboard *s=state(object);if(!s||!arg)return;
 int32_t type,max,min;uint32_t disabled;memcpy(&type,arg,4);memcpy(&disabled,arg+0x1c,4);memcpy(&max,arg+0x20,4);memcpy(&min,arg+0x24,4);
 pthread_mutex_lock(&s->lock);if(s->active||s->pending||s->closed){pthread_mutex_unlock(&s->lock);return;}
 s->hiding=0;s->max=max>0&&max<=MAX_UNITS?max:MAX_UNITS;s->min=min>0&&min<=s->max?min:0;s->digits=type==1;s->disabled=disabled;s->active=1;
 atomic_fetch_add(&s->refs,1);pthread_mutex_unlock(&s->lock);
 fprintf(stderr,"[" GAME_LOG "] inline keyboard: Appear type %d, %d..%d units\n",type,min,max);
 pthread_t worker;if(pthread_create(&worker,NULL,input_worker,s))runtime_fail("Inline input thread");pthread_detach(worker);
}
static void disappear(void *object){
 Keyboard *s=state(object);if(!s)return;
 pthread_mutex_lock(&s->lock);if(s->active)s->hiding=1;pthread_mutex_unlock(&s->lock);
}
/* Calc returns nn::swkbd::State (games/tomodachi: a void answer left x0 arbitrary and the title misread it). */
static int calc(void *object){
 Keyboard *s=state(object);if(!s)return STATE_NONE;atomic_fetch_add(&s->refs,1);
 pthread_mutex_lock(&s->lock);EnterArg text=s->text;int init=s->init_pending,change=s->changed,pending=s->pending,cursor=s->cursor;
 s->init_pending=s->changed=0;s->pending=PENDING_NONE;
 void(*init_cb)(void)=s->init_cb;void(*cancel_cb)(void)=s->cancel_cb;
 void(*change_cb)(const ChangedArg*)=s->change_cb;void(*cursor_cb)(const CursorArg*)=s->cursor_cb;void(*enter_cb)(const EnterArg*)=s->enter_cb;
 pthread_mutex_unlock(&s->lock);
 if(init&&init_cb)init_cb();
 if(change&&change_cb){ChangedArg arg={text,-1,-1,cursor};change_cb(&arg);}
 if(change&&cursor_cb){CursorArg arg={text,cursor};cursor_cb(&arg);}
 if(pending==PENDING_ENTER&&enter_cb)enter_cb(&text);
 if(pending==PENDING_CANCEL&&cancel_cb)cancel_cb();
 pthread_mutex_lock(&s->lock);int current=s->closed?STATE_NONE:s->active?(s->hiding?STATE_DISAPPEARING:STATE_SHOWN):STATE_INITIALIZED;pthread_mutex_unlock(&s->lock);
 release(s);return current;
}

#define SET_CALLBACK(function,field,type) static void function(void *object,type callback){Keyboard*s=state(object);if(!s)return;pthread_mutex_lock(&s->lock);s->field=callback;pthread_mutex_unlock(&s->lock);}
typedef void(*VoidCallback)(void);typedef void(*ChangeCallback)(const ChangedArg*);typedef void(*CursorCallback)(const CursorArg*);typedef void(*EnterCallback)(const EnterArg*);
SET_CALLBACK(set_init,init_cb,VoidCallback)
SET_CALLBACK(set_finish,finish_cb,VoidCallback)
SET_CALLBACK(set_cancel,cancel_cb,VoidCallback)
SET_CALLBACK(set_changed,change_cb,ChangeCallback)
SET_CALLBACK(set_cursor,cursor_cb,CursorCallback)
SET_CALLBACK(set_enter,enter_cb,EnterCallback)

void *runtime_inline_keyboard_override(const char *name){
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn5swkbd14InlineKeyboardC1Ev",construct},
 {"_ZN2nn5swkbd14InlineKeyboardD1Ev",destruct},
 {"_ZN2nn5swkbd14InlineKeyboard8FinalizeEv",finalize},
 {"_ZN2nn5swkbd14InlineKeyboard25GetRequiredWorkBufferSizeEv",work_size},
 {"_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPmS2_",image_requirement},
 {"_ZN2nn5swkbd14InlineKeyboard10InitializeEPv",initialize},
 {"_ZN2nn5swkbd14InlineKeyboard6LaunchEv",launch},
 {"_ZN2nn5swkbd14InlineKeyboard11SetUtf8ModeEb",utf8_mode},
 {"_ZN2nn5swkbd14InlineKeyboard13SetWindowModeENS1_10WindowModeE",window_mode},
 {"_ZN2nn5swkbd14InlineKeyboard14SetKeytopScaleEf",keytop_scale},
 {"_ZN2nn5swkbd14InlineKeyboard18SetKeytopTranslateEff",keytop_translate},
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
 for(unsigned i=0;i<sizeof map/sizeof *map;i++)if(!strcmp(name,map[i].name))return map[i].fn;
 return NULL;
}

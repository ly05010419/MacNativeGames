/* Probe for TOTK_AUTOLOAD (runtime/autoload.c) through the production controller state (runtime/input.c): which A presses
   the game would see. One scenario per process, chosen by argv[1]; the Makefile sets the environment of each:
     press   TOTK_AUTOLOAD=1: nothing before a save is found; after a slot's caption.sav, A presses arrive at the set cadence;
             after a slot's progress.sav, none any more (a later caption.sav does not restart them)
     nosave  TOTK_AUTOLOAD=1 but only option.sav / missing slots: no presses
     key     TOTK_AUTOLOAD=1: a key from the window (runtime_input_key) stops the presses
     pad     TOTK_AUTOLOAD=1: a controller button edge stops the presses
     limit   TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_LIMIT_S=1: the presses stop by themselves after the limit
     off     no TOTK_AUTOLOAD, or TOTK_INPUT_SCRIPT set: no presses after a caption.sav */
#include "../runtime/runtime.h"
#include "../runtime/autoload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
uint64_t runtime_now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
void dm_gfx_shot_at(long frames,const char *path){(void)frames;(void)path;runtime_fail("unexpected screenshot in unit probe");}
void dm_gfx_passdump_next(long n){(void)n;runtime_fail("unexpected pass dump in unit probe");}
void dm_gfx_texdump_next(const char *path){(void)path;runtime_fail("unexpected texture dump in unit probe");}
/* Keys 1, 3 and 4 (frontend.m, game_patches*.c, aspect_patch.h) are not part of this probe. */
void runtime_shortcut_help_toggle(void){}
int runtime_output_mode_cycle(void){return 0;}
int runtime_aspect_toggle(void){return 0;}
void dm_gfx_set_flag(const char *name,int v){(void)name;(void)v;runtime_fail("unexpected runtime flag in unit probe");}
void dm_gfx_querylog_next(long n){(void)n;runtime_fail("unexpected query log in unit probe");}
void dm_gfx_anomdump(void){runtime_fail("unexpected anomaly dump in unit probe");}
void dm_gfx_window_shot(const char *p){(void)p;runtime_fail("unexpected window shot in unit probe");}
int runtime_survey_command(const char *line){(void)line;return 0;}
void runtime_survey_cancel_user(void){}
void runtime_front_request_close(void){runtime_fail("unexpected window close in unit probe");}
static RuntimeGamepadState fake_pad;
void runtime_gamepad_read(RuntimeGamepadState *out){*out=fake_pad;}
void runtime_gamepad_clear(void){fake_pad=(RuntimeGamepadState){0};}
int runtime_handheld_mode(void){return 0;}   /* docked, as the Mac runs */
/* The controller feedback side (runtime/gamepad_feedback.m) is not linked; auto-load never makes the game rumble. */
void runtime_rumble_set(unsigned side,float al,float fl,float ah,float fh){(void)side;(void)al;(void)fl;(void)ah;(void)fh;runtime_fail("unexpected rumble in unit probe");}
void runtime_swkbd_selftest(void){runtime_fail("unexpected keyboard self-test in unit probe");}
void runtime_dump_threads(void){runtime_fail("unexpected thread dump in unit probe");}
void runtime_hle_trace_set(int mode){(void)mode;runtime_fail("unexpected host-call trace in unit probe");}
void runtime_frame_step_pin(double ms){(void)ms;runtime_fail("unexpected frame step pin in unit probe");}
void runtime_front_text_test(const char *op,const char *text){(void)op;(void)text;runtime_fail("unexpected keyboard test in unit probe");}
int runtime_output_res_toggle(void){runtime_fail("unexpected output size switch in unit probe");}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"autoload probe: %s\n",s);exit(1);}

typedef struct{int64_t sample;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attr,pad;} State;
static int (*read_state)(State *,int,const uint32_t *);
static void need(int c,const char *s){if(!c)runtime_fail(s);}
/* A presses the game sees in ms milliseconds (rising edges of button A over 10 ms samples) */
static int presses(unsigned ms){
    uint32_t id=0;State st;int n=0,was=0;
    for(unsigned t=0;t<ms;t+=10){need(read_state(&st,1,&id)==1,"controller read");int a=(int)(st.buttons&1);if(a&&!was)n++;was=a;usleep(10000);}
    return n;
}
int main(int argc,char **argv){
    if(argc!=2)runtime_fail("usage: autoload-probe press|nosave|key|pad|limit|off");
    void(*init)(void)=runtime_input_override("_ZN2nn3hid14InitializeNpadEv");
    read_state=runtime_input_override("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
    need(init&&read_state,"controller entry points");init();
    const char *sc=argv[1];int checks=0;
    /* TOTK_AUTOLOAD_PERIOD_MS=400 in every scenario: about 2.5 presses per second */
    if(!strcmp(sc,"press")){
        need(presses(900)==0,"no presses before the game finds a save");checks++;
        runtime_autoload_save_file_opened("save:/option.sav");runtime_autoload_save_file_opened("save:/slot_00/direct_file_save_related.sav");
        need(presses(900)==0,"option.sav and other slot files do not start the presses");checks++;
        runtime_autoload_save_file_opened("save:/slot_00/caption.sav");
        uint64_t t0=runtime_now_ns();int n=presses(2000);double expect=(double)(runtime_now_ns()-t0)/1e6/400.0;   /* 400 ms period */
        if(n<expect-1.5||n>expect+1.5)fprintf(stderr,"autoload probe: %d presses, about %.1f expected\n",n,expect);
        need(n>=expect-1.5&&n<=expect+1.5,"about one A press per period after a caption.sav");checks++;
        runtime_autoload_save_file_opened("save:/slot_01/progress.sav");presses(500);   /* a press begun before the stop is still delivered (short-tap latch) */
        need(presses(1500)==0,"no presses after the load started (progress.sav opened)");checks++;
        runtime_autoload_save_file_opened("save:/slot_02/caption.sav");
        need(presses(1500)==0,"a later caption.sav does not restart the presses");checks++;
    }else if(!strcmp(sc,"nosave")){
        runtime_autoload_save_file_opened("save:/option.sav");runtime_autoload_save_file_opened("save:/slot_00/footprint.sav");
        need(presses(1500)==0,"no presses without a save");checks++;
    }else if(!strcmp(sc,"key")||!strcmp(sc,"pad")){
        runtime_autoload_save_file_opened("save:/slot_00/caption.sav");
        need(presses(1300)>=2,"presses after a caption.sav");checks++;
        if(!strcmp(sc,"key")){runtime_input_key(13,1);runtime_input_key(13,0);}   /* W from the window */
        else{fake_pad.buttons=2;fake_pad.connected=1;uint32_t id=0;State st;read_state(&st,1,&id);fake_pad.buttons=0;}
        presses(500);need(presses(1500)==0,"the user's own input stops the presses");checks++;
    }else if(!strcmp(sc,"limit")){
        runtime_autoload_save_file_opened("save:/slot_00/caption.sav");
        need(presses(800)>=1,"presses after a caption.sav");checks++;
        presses(1200);need(presses(1500)==0,"the presses stop after TOTK_AUTOLOAD_LIMIT_S");checks++;
    }else if(!strcmp(sc,"off")){
        runtime_autoload_save_file_opened("save:/slot_00/caption.sav");
        need(presses(1500)==0,"no presses when auto-load is off");checks++;
    }else runtime_fail("unknown scenario");
    printf("{\"probe\":\"autoload\",\"scenario\":\"%s\",\"checks\":%d,\"ok\":true}\n",sc,checks);
    return 0;
}

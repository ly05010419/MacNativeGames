#pragma once
#include <assert.h>
#include "../games/totk/runtime/input.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Interactive diagnostics are not exercised by this service ABI probe. */
/* A fake clock: real user input reads it to rate-limit its log line. */
/* Motion checks pin it (probe_clock_fixed) to the times of the samples they feed. */
static uint64_t probe_now;static int probe_clock_fixed;
uint64_t runtime_now_ns(void){return probe_clock_fixed?probe_now:(probe_now+=1000000);}
void dm_gfx_shot_at(long frames,const char *path){(void)frames;(void)path;runtime_fail("unexpected screenshot in unit probe");}
void dm_gfx_passdump_next(long n){(void)n;runtime_fail("unexpected pass dump in unit probe");}
void dm_gfx_texdump_next(const char *path){(void)path;runtime_fail("unexpected texture dump in unit probe");}
static int help_toggles,interp_toggles,output_cycles,aspect_toggles,taa_toggles,pad_type_cycles,mouse_toggles;   // keys 1 / 2 / 3 / 4 / 5 / 6 / 7 (see the key checks)
void runtime_swkbd_selftest(void){runtime_fail("unexpected keyboard self-test in unit probe");}
void runtime_dump_threads(void){runtime_fail("unexpected thread dump in unit probe");}
void runtime_hle_trace_set(int mode){(void)mode;runtime_fail("unexpected host-call trace in unit probe");}
void runtime_frame_step_pin(double ms){(void)ms;runtime_fail("unexpected frame step pin in unit probe");}
void runtime_front_text_test(const char *op,const char *text){(void)op;(void)text;runtime_fail("unexpected keyboard test in unit probe");}
int runtime_output_res_toggle(void){runtime_fail("unexpected output size switch in unit probe");return 0;}
void runtime_settings_toggle(void){help_toggles++;}   // key 1 (main: the settings panel)
int runtime_output_mode_cycle(void){output_cycles++;return 1;}
int runtime_aspect_toggle(void){aspect_toggles++;return 1;}
void taa_toggle(void){taa_toggles++;}
int runtime_gamepad_type_cycle(void){return ++pad_type_cycles;}
int runtime_mouse_toggle(void){return ++mouse_toggles&1;}
void dm_gfx_set_flag(const char *name,int v){if(!strcmp(name,"mfx_interp")&&v==-1){interp_toggles++;return;}runtime_fail("unexpected runtime flag in unit probe");}
void dm_gfx_querylog_next(long n){(void)n;runtime_fail("unexpected query log in unit probe");}
void dm_gfx_anomdump(void){runtime_fail("unexpected anomaly dump in unit probe");}
void dm_gfx_window_shot(const char *p){(void)p;runtime_fail("unexpected window shot in unit probe");}
int runtime_survey_command(const char *line){(void)line;return 0;}
void runtime_survey_cancel_user(void){}
void runtime_front_request_close(void){runtime_fail("unexpected window close in unit probe");}
/* Test double for the native controller layer (runtime/gamepad.m is not linked into this probe). */
static RuntimeGamepadState fake_pad;
void runtime_gamepad_read(RuntimeGamepadState *out){*out=fake_pad;}
void runtime_gamepad_clear(void){fake_pad=(RuntimeGamepadState){0};}
/* Test double for the Mac controller's haptics (runtime/gamepad_feedback.m): the last value per side. */
static int rumble_calls;static float rumble_seen[2][4];
void runtime_rumble_set(unsigned side,float al,float fl,float ah,float fh){if(side>1)runtime_fail("rumble side out of range");rumble_calls++;rumble_seen[side][0]=al;rumble_seen[side][1]=fl;rumble_seen[side][2]=ah;rumble_seen[side][3]=fh;}
static int handheld;
int runtime_handheld_mode(void){return handheld;}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}

void runtime_autoload_user_input(void){}
int runtime_hid_feedback_flag(const char *name,int v){(void)name;(void)v;return 0;}

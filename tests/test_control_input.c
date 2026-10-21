/* Exercise the production input dispatcher without a game, window or player data. */
#include "../games/totk/runtime/input.c"
#include <assert.h>
static int active=1,actions,userNotices,legacyOverlay,legacyFI,legacyOutput,legacyAspect,legacyTAA;
static unsigned binding=13,bindingMods=1;
int runtime_controls_active(void){return active;}
int runtime_controls_handle_key(unsigned c,unsigned m){if(active&&c==binding&&m==bindingMods){actions++;return 1;}return 0;}
void runtime_survey_cancel_user(void){}
void runtime_autoload_user_input(void){userNotices++;}
static uint64_t testNow=1000000000;
uint64_t runtime_now_ns(void){return testNow;}
static RuntimeGamepadState testPad;
void runtime_gamepad_read(RuntimeGamepadState *p){*p=testPad;}
int runtime_handheld_mode(void){return 0;}
void runtime_gamepad_clear(void){}
void taa_toggle(void){legacyTAA++;}
void runtime_shortcut_help_toggle(void){legacyOverlay++;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;legacyFI++;}
int runtime_output_mode_cycle(void){legacyOutput++;return 1;}
int runtime_aspect_toggle(void){legacyAspect++;return 1;}
/* Constructor-installed diagnostic streams are disabled in this probe. Any accidental
   diagnostic call must fail rather than silently behaving like the real game. */
#define UNREACHABLE(name,args) void name args { abort(); }
UNREACHABLE(dm_gfx_anomdump,(void))
UNREACHABLE(dm_gfx_passdump_next,(long n))
UNREACHABLE(dm_gfx_querylog_next,(long n))
UNREACHABLE(dm_gfx_shot_at,(long n,const char *p))
UNREACHABLE(dm_gfx_texdump_next,(const char *p))
UNREACHABLE(dm_gfx_window_shot,(const char *p))
UNREACHABLE(runtime_dump_threads,(void))
UNREACHABLE(runtime_frame_step_pin,(double n))
UNREACHABLE(runtime_front_control_test,(const char *s))
UNREACHABLE(runtime_front_request_close,(void))
UNREACHABLE(runtime_front_text_test,(const char *s,const char *p))
UNREACHABLE(runtime_hle_trace_set,(int n))
UNREACHABLE(runtime_swkbd_selftest,(void))
int runtime_survey_command(const char *s){(void)s;abort();}
int runtime_hid_feedback_flag(const char *s,int n){(void)s;(void)n;abort();}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"unexpected runtime failure: %s\n",s);abort();}
int main(void){
 runtime_input_key_modifiers(13,1,1);assert(actions==1&&!keys[13]&&!pending&&userNotices==1);
 runtime_input_key_modifiers(13,1,1);assert(actions==1); // held keys never toggle repeatedly
 runtime_input_key_modifiers(13,0,0);assert(!keys[13]);
 runtime_input_key_modifiers(13,1,0);assert(actions==1&&keys[13]); // modifiers must match exactly
 runtime_input_key_modifiers(13,0,0);
 bindingMods=0;
 runtime_input_key_modifiers(13,1,1);assert(actions==1&&keys[13]); // Cmd+W must not fire plain W
 runtime_input_clear();
 runtime_input_key_synthetic(13,1);runtime_input_key_synthetic(13,1);assert(actions==2&&!keys[13]);
 runtime_input_key_synthetic(13,0);runtime_input_key_synthetic(13,1);assert(actions==3);
 runtime_input_clear();
 runtime_input_key_synthetic(23,1);runtime_input_key_synthetic(23,1);runtime_input_key_synthetic(23,0);assert(legacyTAA==1);
 active=0;
 unsigned legacy[]={18,19,20,21};for(unsigned i=0;i<4;i++){
  runtime_input_key_synthetic(legacy[i],1);runtime_input_key_synthetic(legacy[i],1);runtime_input_key_synthetic(legacy[i],0);
 }
 assert(legacyOverlay==1&&legacyFI==1&&legacyOutput==1&&legacyAspect==1);
 runtime_input_clear();initialize();uint32_t id=0;NpadState state;
 RuntimeKeyboardBindings b,original;runtime_input_bindings_get(&b);original=b;assert(input_bindings_valid(&b));
 runtime_input_key(38,1);full_states(&state,1,&id);assert(state.buttons==1);runtime_input_key(38,0);
 runtime_input_key(49,1);full_states(&state,1,&id);assert(state.buttons==2);runtime_input_key(49,0);
 runtime_input_key(13,1);full_states(&state,1,&id);assert(state.ly==32767);runtime_input_clear();
 b.source[0][0]=12;b.source[1][0]=129;assert(runtime_input_bindings_set(&b));
 runtime_input_key(38,1);full_states(&state,1,&id);assert(!state.buttons);runtime_input_clear();
 runtime_input_key(11,1);full_states(&state,1,&id);assert(state.buttons==1);runtime_input_key(11,0);full_states(&state,1,&id);assert(!state.buttons);
 runtime_input_mouse_button(0,1);full_states(&state,1,&id);assert(state.buttons==2);runtime_input_mouse_button(0,0);
 runtime_input_key(49,1);full_states(&state,1,&id);assert(state.buttons==2);runtime_input_key(49,0);full_states(&state,1,&id);assert(!state.buttons);
 runtime_input_mouse_button(0,1);runtime_input_mouse_button(0,0);full_states(&state,1,&id);assert(state.buttons==2);full_states(&state,1,&id);assert(!state.buttons);
 runtime_input_key(11,1);assert(runtime_input_bindings_set(&b));full_states(&state,1,&id);assert(!state.buttons);
 RuntimeKeyboardBindings bad=b;bad.source[2][0]=b.source[0][0];assert(!runtime_input_bindings_set(&bad));bad=b;bad.source[2][0]=137;assert(!runtime_input_bindings_set(&bad));runtime_input_bindings_get(&bad);assert(!memcmp(&bad,&b,sizeof b));
 runtime_input_key(0,1);runtime_input_key(2,1);full_states(&state,1,&id);assert(!state.lx);runtime_input_clear();
 b.mouse_look=1;assert(runtime_input_bindings_set(&b));runtime_input_mouse_motion(10,10);full_states(&state,1,&id);assert(state.rx>0&&state.ry<0);
 testNow+=81000000;full_states(&state,1,&id);assert(!state.rx&&!state.ry);
 b.mouse_invert_y=1;assert(runtime_input_bindings_set(&b));runtime_input_mouse_motion(1e6,1e6);full_states(&state,1,&id);assert(state.rx==32767&&state.ry==32767);
 runtime_input_mouse_motion(0,0);full_states(&state,1,&id);assert(!state.rx&&!state.ry);
 testPad=(RuntimeGamepadState){.buttons=4,.lx=20000};full_states(&state,1,&id);assert(state.buttons==4&&state.lx==20000);
 runtime_input_suspend(1);runtime_input_key(11,1);runtime_input_mouse_button(0,1);runtime_input_mouse_motion(99,99);full_states(&state,1,&id);assert(state.attributes==1&&!state.buttons&&!state.lx&&!state.rx&&!state.ry);
 testPad=(RuntimeGamepadState){0};runtime_input_suspend(0);full_states(&state,1,&id);assert(!state.buttons&&!state.rx);assert(runtime_input_bindings_set(&original));
 RuntimeControllerBindings c={0};controller_map_default(c.map,c.modes);assert(controller_bindings_valid(&c));c.swap_sticks=1;c.invert_look_y=1;assert(controller_bindings_valid(&c));c.map[0]=3;assert(!controller_bindings_valid(&c));c.map[0]=1;
 c.modes[0]=CONTROLLER_AIM;assert(!controller_bindings_valid(&c));c.modes[0]=CONTROLLER_AIM|(1<<3);assert(controller_bindings_valid(&c));c.modes[0]=CONTROLLER_AIM|(31<<3);assert(!controller_bindings_valid(&c));
 puts("PASS: exact modifiers, held keys, keyboard/mouse remapping, axes, suspension, validation and legacy shortcuts");
}

/* Exercise production mapping/modes and transition suppression without hardware. */
#include "../games/totk/runtime/gamepad.m"
#include <assert.h>
static uint64_t now=1000000000;
uint64_t runtime_now_ns(void){return now;}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);abort();}
void runtime_feedback_attach(GCController *c){(void)c;}
void runtime_feedback_detach(GCController *c){(void)c;}
void runtime_feedback_quiet(void){}
int main(void){@autoreleasepool{
 load_controller_map();RuntimeControllerBindings b;runtime_gamepad_bindings_get(&b);assert(controller_bindings_valid(&b));
 // Applying a mapping takes effect in the real state transition.
 b.map[0]=1u<<3;assert(runtime_gamepad_bindings_set(&b));RuntimeGamepadState in={.connected=1},out;
 update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));
 runtime_gamepad_clear();update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(!out.buttons);
 update_gamepad_state(in,0);update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));
 runtime_gamepad_clear();update_gamepad_state(in,0);b.modes[0]=CONTROLLER_TAP;assert(runtime_gamepad_bindings_set(&b));
 update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));now+=410000000;update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(!out.buttons);
 b.modes[0]=CONTROLLER_TOGGLE;assert(runtime_gamepad_bindings_set(&b));update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));
 in.lx=20000;update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));in.lx=0;update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(!out.buttons);
 b.modes[0]=CONTROLLER_AIM|(9<<3);assert(runtime_gamepad_bindings_set(&b));update_gamepad_state(in,1);runtime_gamepad_read(&out);assert(out.buttons==(1u<<3));
 update_gamepad_state(in,1|(1<<9));runtime_gamepad_read(&out);assert(!out.buttons);update_gamepad_state(in,0);
 b.modes[0]=CONTROLLER_HOLD;b.swap_sticks=1;b.invert_look_y=1;assert(runtime_gamepad_bindings_set(&b));
 update_gamepad_state(in,0);in.lx=10000;in.ly=20000;in.rx=-30000;in.ry=7000;update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(out.lx==-30000&&out.ly==7000&&out.rx==10000&&out.ry==-20000);
 runtime_gamepad_clear();update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(!out.lx&&!out.rx);in=(RuntimeGamepadState){.connected=1};update_gamepad_state(in,0);in.lx=15000;update_gamepad_state(in,0);runtime_gamepad_read(&out);assert(out.rx==15000);
 RuntimeControllerBindings bad=b;bad.modes[0]=CONTROLLER_AIM;assert(!runtime_gamepad_bindings_set(&bad));runtime_gamepad_bindings_get(&bad);assert(bad.modes[0]==CONTROLLER_HOLD);
 puts("PASS: controller remap, hold/tap/toggle/aim, swap/invert and held-input suppression");
}}

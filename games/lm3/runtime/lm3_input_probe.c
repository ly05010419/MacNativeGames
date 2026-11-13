/* Standalone, isolated checks through the actual SDK override registrations.
   No GameController discovery, window or game entry is started. */
#include "runtime.h"
#include "lm3_controller_policy.h"
#include "lm3_controller_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t sampling; uint64_t buttons; int32_t lx,ly,rx,ry; uint32_t attributes,pad;
} ProbeNpadState;
_Static_assert(sizeof(ProbeNpadState)==40,"SDK 7 FullKey state ABI");
_Static_assert(sizeof(RuntimeControllerSupportResult)==8,"SDK controller result ABI");
_Static_assert(offsetof(RuntimeControllerSupportResult,selected)==4,"SDK selected controller offset");
static void *input_fn(const char *name) {
    void *function=runtime_input_override(name);
    if(!function)runtime_fail("input probe: SDK entry is not registered");
    return function;
}
#define INPUT_FN(type,name) ((type)input_fn(name))
typedef void (*InitInput)(void);
typedef void (*SetIds)(const uint32_t *,uint64_t);
typedef void (*SetStyles)(uint32_t);
typedef uint32_t (*GetStyle)(const uint32_t *);
typedef void (*GetState)(ProbeNpadState *,const uint32_t *);
typedef int (*GetStates)(ProbeNpadState *,int,const uint32_t *);
typedef void (*DisconnectInput)(const uint32_t *);
typedef uint32_t (*ControllerSupport)(RuntimeControllerSupportResult *,const void *);
typedef uint32_t (*ControllerSupportPlain)(const void *);
extern void runtime_input_key_synthetic(unsigned code,int down);

void runtime_lm3_input_probe(void) {
    const char *owner=getenv("TOTK_SLOT_OWNER"),*probe=getenv("LM3_INPUT_PROBE");
    if(!owner||strcmp(owner,"codex")||!probe||strcmp(probe,"1"))runtime_fail("input probe requires an isolated codex owner");
    int checked=runtime_gamepad_probe();
    checked+=runtime_hid_feedback_probe();
#define CHECK(condition,message) do { if(!(condition))runtime_fail("input probe: " message);checked++; } while(0)
    /* The same pure selection policy is used by the Cocoa pairing handshake. */
    const struct { int min,max;unsigned allowed,physical;int single,status;unsigned count,selected; } choices[]={
        {2,2,3,1,0,0,0,0},{2,2,3,3,0,1,2,0},{1,1,3,0,0,1,1,0},
        {1,1,2,0,0,0,0,0},{1,1,2,2,0,1,1,1},{1,2,3,0,0,0,0,0},
        {1,2,3,2,0,1,1,1},{2,2,1,3,0,0,0,0},{3,4,3,3,0,-1,0,0},
        {0,2,3,0,0,0,0,0},{0,2,3,2,0,1,1,1},{2,1,3,3,0,-1,0,0},
        {1,8,3,3,0,1,2,0},{2,2,1,0,1,1,1,0}
    };
    for(unsigned i=0;i<sizeof choices/sizeof *choices;i++) {
        LM3ControllerChoice selected;
        int status=lm3_controller_choose(choices[i].min,choices[i].max,choices[i].allowed,choices[i].physical,choices[i].single,&selected);
        CHECK(status==choices[i].status,"controller count, supported-ID or single-mode policy");
        if(status>0)CHECK(selected.count==choices[i].count&&selected.selected==choices[i].selected,"selected controller ID");
    }
    InitInput init=INPUT_FN(InitInput,"_ZN2nn3hid14InitializeNpadEv");
    SetIds set_ids=INPUT_FN(SetIds,"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm");
    SetStyles set_styles=INPUT_FN(SetStyles,"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    GetStyle style=INPUT_FN(GetStyle,"_ZN2nn3hid15GetNpadStyleSetERKj");
    GetState state=INPUT_FN(GetState,"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
    GetState handheld=INPUT_FN(GetState,"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj");
    GetStates states=INPUT_FN(GetStates,"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
    DisconnectInput disconnect=INPUT_FN(DisconnectInput,"_ZN2nn3hid14DisconnectNpadERKj");
    ControllerSupport support=INPUT_FN(ControllerSupport,"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE");
    ControllerSupportPlain plain=INPUT_FN(ControllerSupportPlain,"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE");
    const uint32_t p1=0,p2=1,hh=0x20,unknown=7,ids[]={0,1,0x20};
    init();set_ids(ids,3);set_styles(0x1f);runtime_input_clear();
    RuntimeGamepadState first={.buttons=1,.lx=23000,.ly=-11000,.rx=-7000,.ry=9000,.connected=1};
    RuntimeGamepadState second={.buttons=2,.lx=-19000,.ly=17000,.rx=13000,.ry=-21000,.connected=1};
    RuntimeGamepadState neutral={.connected=1},absent={0};
    runtime_gamepad_probe_publish(0,&first);runtime_gamepad_probe_publish(1,&second);
    struct { uint64_t before;ProbeNpadState value;uint64_t after; } guarded;
    memset(&guarded,0xa5,sizeof guarded);ProbeNpadState other={0};
    CHECK(states(NULL,0,NULL)==0,"zero sample capacity must not touch output");
    if(runtime_handheld_mode()) {
        CHECK(style(&p1)==0&&style(&p2)==0&&style(&hh)==2,"handheld exposes only the original handheld slot");
        handheld(&guarded.value,&hh);
        CHECK(guarded.value.buttons==1&&guarded.value.lx==23000&&guarded.value.attributes==1,"handheld preserves player-one input");
        state(&other,&p2);CHECK(!other.attributes&&!other.buttons,"handheld does not expose player two");
    }else{
        CHECK(style(&p1)==1&&style(&p2)==1&&style(&unknown)==0&&style(&hh)==0,"FullKey style and connected ID isolation");
        state(&guarded.value,&p1);state(&other,&p2);
        CHECK(guarded.value.attributes==1&&other.attributes==1&&guarded.value.buttons==1&&other.buttons==2,"buttons must not merge between players");
        CHECK(guarded.value.lx==23000&&guarded.value.ly==-11000&&guarded.value.rx==-7000&&guarded.value.ry==9000,"player-one four-axis state");
        CHECK(other.lx==-19000&&other.ly==17000&&other.rx==13000&&other.ry==-21000,"player-two opposite-axis state");
        int64_t second_sample=other.sampling;
        state(&guarded.value,&p1);state(&guarded.value,&p1);state(&other,&p2);
        CHECK(other.sampling==second_sample+1,"player-one reads must not advance player-two sampling");
        runtime_gamepad_probe_publish(0,&neutral);runtime_gamepad_probe_publish(1,&neutral);runtime_input_clear();
        RuntimeGamepadState tap=neutral;tap.buttons=4;
        runtime_gamepad_probe_publish(1,&tap);runtime_gamepad_probe_publish(1,&neutral);
        CHECK(style(&p2)==1&&runtime_gamepad_connected_mask()==3,"connection/style queries must not consume taps");
        state(&guarded.value,&p1);CHECK(!guarded.value.buttons,"player-two short press leaked to player one");
        state(&other,&p2);CHECK(other.buttons==4,"player-one poll consumed player-two short press");
        state(&other,&p2);CHECK(!other.buttons,"player-two short press was not consumed exactly once");
        runtime_input_key_synthetic(38,1);runtime_input_key_synthetic(38,0);
        state(&other,&p2);CHECK(!other.buttons,"keyboard must not enter player two");
        state(&guarded.value,&p1);CHECK(guarded.value.buttons==1,"player-two poll consumed keyboard pending edge");
        state(&guarded.value,&p1);CHECK(!guarded.value.buttons,"keyboard pending edge was not consumed once");
        set_ids(&p1,1);CHECK(style(&p2)==0,"unsupported player-two ID reported a style");
        state(&other,&p2);CHECK(!other.attributes&&!other.buttons&&!other.lx,"unsupported ID exposed input");
        set_ids(ids,3);set_styles(2);CHECK(!style(&p1)&&!style(&p2),"unsupported FullKey style exposed players");
        state(&other,&p2);CHECK(!other.attributes&&!other.buttons,"unsupported style exposed state");set_styles(0x1f);
        runtime_gamepad_probe_publish(1,&second);disconnect(&p1);state(&other,&p2);
        CHECK(other.buttons==2&&other.lx==-19000&&other.attributes==1,"disconnecting player one cleared player-two state");
        set_ids(ids,3);runtime_gamepad_probe_publish(1,&neutral);runtime_input_clear();
        runtime_gamepad_probe_publish(1,&absent);CHECK(!style(&p2)&&style(&p1)==1,"physical player-two disconnect and keyboard player-one continuity");
        state(&other,&p2);CHECK(!other.attributes&&!other.buttons&&!other.lx,"disconnected player two returned held input");
        runtime_gamepad_probe_publish(1,&neutral);
    }
    CHECK(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5)&&guarded.after==UINT64_C(0xa5a5a5a5a5a5a5a5),"Npad state ABI canaries");
    struct { uint64_t before;RuntimeControllerSupportResult value;uint64_t after; } result;
    memset(&result,0xa5,sizeof result);
    uint8_t request[7]={2,2,1,1,1,0,0};
    if(runtime_handheld_mode()) {
        CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED,"handheld cannot fabricate two players");
        request[0]=request[1]=1;request[5]=1;
        CHECK(!support(&result.value,request)&&result.value.players==1&&result.value.selected==hh,"handheld one-player pairing compatibility");
    }else{
        runtime_gamepad_probe_publish(0,&neutral);runtime_gamepad_probe_publish(1,&absent);
        CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED&&result.value.players==0,"headless insufficient controllers must cancel");
        CHECK(plain(request)==LM3_CONTROLLER_SUPPORT_CANCELED,"original-game plain support entry must also cancel");
        runtime_gamepad_probe_publish(1,&neutral);
        disconnect(&p2);CHECK(!style(&p2)&&style(&p1)==1,"DisconnectNpad must isolate selected player");
        CHECK(!support(&result.value,request)&&result.value.players==2&&result.value.selected==0,"two real players must pair and recover disconnected IDs");
        CHECK(style(&p2)==1,"successful pairing did not recover player-two style");
        set_ids(&p1,1);CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED,"pairing counted an unsupported ID");
        set_ids(&p2,1);request[0]=request[1]=1;
        CHECK(!support(&result.value,request)&&result.value.players==1&&result.value.selected==1,"lone allowed player two selected wrong ID");
        set_ids(ids,3);request[0]=3;request[1]=4;
        CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED,"unsupported player count must cancel");
        request[0]=2;request[1]=1;CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED,"invalid min/max must cancel");
        request[0]=request[1]=2;request[3]=2;CHECK(support(&result.value,request)==LM3_CONTROLLER_SUPPORT_CANCELED,"malformed option byte must cancel");request[3]=1;
        request[5]=1;CHECK(!support(&result.value,request)&&result.value.players==1,"single mode did not select one player");request[5]=0;
        runtime_input_key_synthetic(38,1);runtime_gamepad_probe_publish(1,&second);
        CHECK(!support(&result.value,request),"ready two-player handshake");
        state(&guarded.value,&p1);state(&other,&p2);
        CHECK(!guarded.value.buttons&&!other.buttons&&!other.lx&&other.attributes==1,"pairing confirmation leaked input or disconnected physical slot");
        runtime_input_key_synthetic(38,0);
    }
    CHECK(result.before==UINT64_C(0xa5a5a5a5a5a5a5a5)&&result.after==UINT64_C(0xa5a5a5a5a5a5a5a5),"controller-support result ABI canaries");
    CHECK(!result.value.reserved[0]&&!result.value.reserved[1]&&!result.value.reserved[2],"controller-support result reserved bytes");
    GetState active_state=runtime_handheld_mode()?handheld:state;const uint32_t *active_id=runtime_handheld_mode()?&hh:&p1;
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_SETTINGS,1);
    runtime_gamepad_probe_publish(0,&first);runtime_gamepad_probe_publish(1,&second);
    runtime_input_key_synthetic(38,1);
    active_state(&guarded.value,active_id);state(&other,&p2);
    CHECK(runtime_input_suspended()&&guarded.value.attributes==1&&!guarded.value.buttons&&!guarded.value.lx,"host settings must gate gameplay while preserving connection");
    CHECK(!runtime_input_buttons()&&!other.buttons&&!other.lx,"host settings leaked player-two or cheat-key input");
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_PAIRING,1);
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_SETTINGS,0);
    runtime_gamepad_probe_publish(1,&second);state(&other,&p2);
    CHECK(runtime_input_suspended()&&!other.buttons&&!other.lx,"closing one host menu resumed another menu's input");
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_PAIRING,0);
    active_state(&guarded.value,active_id);state(&other,&p2);
    CHECK(!runtime_input_suspended()&&!guarded.value.buttons&&!other.buttons&&!other.lx,"host-menu transition leaked pending controller input");
    runtime_input_key_synthetic(38,1);active_state(&guarded.value,active_id);
    CHECK(!guarded.value.buttons,"held menu key resumed without release");
    runtime_input_key_synthetic(38,0);runtime_input_key_synthetic(38,1);runtime_input_key_synthetic(38,0);
    active_state(&guarded.value,active_id);CHECK(guarded.value.buttons==1,"fresh keyboard press after host-menu release was lost");
    runtime_input_clear();runtime_gamepad_probe_publish(0,&absent);runtime_gamepad_probe_publish(1,&absent);
    printf("{\"status\":\"pass\",\"checks\":%d,\"scope\":\"SDK Npad routing, per-player edges/samples, keyboard isolation, pairing min/max/cancel and ABI; synthetic isolated host snapshots\"}\n",checked);
#undef CHECK
}

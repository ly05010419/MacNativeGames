/* Optional TOTK 1.4.3 arrow-material repeat (Mac). The two veneers run on the original actor update thread.
   Host callbacks only choose an action; the game's own request, stock check, attachment and refund run as before on
   the guest stack. No inventory quantities, actor links or equipment strings are written by the host. */
#include "runtime.h"
#include "auto_attach_state.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(TOTK_GAME_V143) && !TARGET_OS_IPHONE
#include "bow_state.h"
#include "game_version.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
static _Atomic int installed, enabled;
static _Atomic uint32_t shown_arrow, remove_arrow;
static _Atomic uint64_t previous_buttons;
static int swallow_action;
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static AutoAttachState state;
static char last_item[64];
static uint8_t *main_base;
static BowTotkCache input_player;
static int log_actions;
static uint64_t u64(uint64_t p) { uint64_t n;memcpy(&n,(const void *)(uintptr_t)p,8);return n; }
static uint32_t u32(uint64_t p) { uint32_t n;memcpy(&n,(const void *)(uintptr_t)p,4);return n; }
static int actor_named(uint64_t actor, const char *name, size_t n) {
    uint64_t text=actor?u64(actor+0x218):0;
    return text && !memcmp((const void *)(uintptr_t)text,name,n);
}
static int read_live(uint64_t address,void *out,size_t n) {
    mach_vm_size_t got=0;
    return mach_vm_read_overwrite(mach_task_self(),address,n,(mach_vm_address_t)(uintptr_t)out,&got)==KERN_SUCCESS && got==n;
}
static int live_bow_drawn(void) {
    return main_base && bow_state_totk143((uintptr_t)main_base,GAME_ADDR(0x4663f08),GAME_ACTOR_NAME,read_live,&input_player)==BOW_DRAWN;
}

/* Synchronous callbacks from verified code sites: their actor and component arguments are live in the caller's update. */
static uintptr_t decide(uint64_t equipment, uint64_t bullet_state, uint64_t bow) {
    uint64_t player=u64(equipment+0x18), arrow=u64(bullet_state+0x18);
    if (!actor_named(player,"Player",7) || !actor_named(arrow,"NormalArrow",12)) return 0;
    uint64_t block=u64(player+0x248), draw=block?u64(block+0x760):0;
    uint64_t component=u64(bullet_state+0x50), link=component?u64(component+0x130):0;
    AutoAttachFrame f={.player=player,.arrow=arrow,.player_id=u32(player+0x380),.arrow_id=u32(arrow+0x380),
        .active=runtime_front_game_input(),.paused=!!(u32(player+0x5ac)&0x40),
        .drawn=draw && !!(*(const uint8_t *)(uintptr_t)(draw+9)&0x20),
        .pending=!!u64(bow+0x2b8),.attached=link && u64(link+8) && u32(link+0x10)!=UINT32_MAX};
    uint32_t remove=atomic_exchange(&remove_arrow,0);
    atomic_store(&shown_arrow,f.active && !f.paused && f.drawn && f.attached?f.arrow_id:0);
    pthread_mutex_lock(&state_lock);
    if (state.player && (state.player!=player || state.player_id!=f.player_id)) last_item[0]=0;
    f.have_item=!!last_item[0];
    int action=auto_attach_decide(&state,f,remove);
    static _Thread_local char item[64];
    static _Thread_local const char *name;
    if (action==AUTO_ATTACH_REPEAT) { memcpy(item,last_item,sizeof item);name=item; }
    pthread_mutex_unlock(&state_lock);
    if (log_actions && action) fprintf(stderr,"[auto-attach] arrow %u: %s%s\n",f.arrow_id,
        action==AUTO_ATTACH_REMOVE?"remove and stop repeat":"request last material ",action==AUTO_ATTACH_REPEAT?item:"");
    return action==AUTO_ATTACH_REMOVE?1:action==AUTO_ATTACH_REPEAT?(uintptr_t)&name:0;
}
static void manual(uint64_t equipment, const char *const *name) {
    uint64_t player=u64(equipment+0x18);
    if (!actor_named(player,"Player",7) || !name || !*name) return;
    size_t n=strnlen(*name,sizeof last_item);
    if (n==sizeof last_item) return;
    pthread_mutex_lock(&state_lock);
    if (state.player!=player || state.player_id!=u32(player+0x380))
        state=(AutoAttachState){.player=player,.player_id=u32(player+0x380)};
    memcpy(last_item,*name,n+1);auto_attach_manual(&state);
    pthread_mutex_unlock(&state_lock);
    if (log_actions) fprintf(stderr,"[auto-attach] manual selection: %s\n",*name);
}

/* The input has already passed through the user's bindings. Action A is Triangle in the user's layout; keyboard and
   other controllers use the same logical action. Swallow the whole press, so removing a material cannot also interact. */
uint64_t runtime_auto_attach_input(uint64_t buttons) {
    uint64_t before=atomic_exchange(&previous_buttons,buttons);
    if (!(buttons&1)) swallow_action=0;
    /* Actor updates stop under menus. Re-read the live Player/pause state on the edge instead of trusting shown_arrow. */
    if (atomic_load(&enabled) && runtime_front_game_input() && (buttons&1) && !(before&1) && live_bow_drawn()) {
        uint32_t arrow=atomic_load(&shown_arrow);
        if (arrow) { atomic_store(&remove_arrow,arrow);swallow_action=1; }
    }
    return swallow_action?buttons&~1ULL:buttons;
}
int runtime_auto_attach_available(void) { return atomic_load(&installed); }
int runtime_auto_attach_choice(void) { return atomic_load(&enabled); }
void runtime_auto_attach_choose(int on) {
    if (!runtime_auto_attach_available()) return;
    atomic_store(&enabled,0);atomic_store(&shown_arrow,0);atomic_store(&remove_arrow,0);
    pthread_mutex_lock(&state_lock);state=(AutoAttachState){0};last_item[0]=0;pthread_mutex_unlock(&state_lock);
    atomic_store(&enabled,!!on);
    runtime_setting_save("auto-attach.txt",on?"on":"off","arrow auto-attach");
    fprintf(stderr,"[auto-attach] %s; choose a material manually to start repeating\n",on?"on":"off");
}

/* Feature-owned poll, fallback and manual-selection tail call. Branches and literals relocated below. */
static const uint32_t veneer[]={
    0x58000650u,0x88dffe10u,0x340005d0u,0xa9bb7bfdu,
    0x910003fdu,0xa90153f3u,0xa9025bf5u,0xa90363f7u,
    0xf90023f9u,0xaa0003f5u,0xaa0103f6u,0xaa0203f7u,
    0xaa1303f8u,0xaa1403e0u,0xaa1303e1u,0xaa1503e2u,
    0x58000490u,0xd63f0200u,0xb4000280u,0xf100041fu,
    0x54000200u,0xaa0003f9u,0x58000408u,0xf9400108u,
    0xf9400100u,0xb40001a0u,0xaa1903e1u,0x52800022u,
    0x2a1f03e3u,0x52800024u,0x94000000u,0x360000e0u,
    0xaa1503e0u,0xaa1903e1u,0x94000000u,0x14000003u,
    0xaa1803e0u,0x94000000u,0xaa1503e0u,0xaa1603e1u,
    0xaa1703e2u,0x94000000u,0xa94153f3u,0xa9425bf5u,
    0xa94363f7u,0xf94023f9u,0xa8c57bfdu,0xd65f03c0u,
    0x14000000u,0xd503201fu,0x00000000u,0x00000000u,
    0x00000000u,0x00000000u,0x00000000u,0x00000000u,
    0x58000190u,0x88dffe10u,0x34000130u,0xa9be7bfdu,
    0x910003fdu,0xa90107e0u,0xaa1303e0u,0x580000f0u,
    0xd63f0200u,0xa94107e0u,0xa8c27bfdu,0x14000000u,
    0x00000000u,0x00000000u,0x00000000u,0x00000000u,
};

static uint32_t jump(uint64_t from,uint64_t to,int call) {
    int64_t d=(int64_t)to-(int64_t)from;
    if ((d&3)||d<-(1<<27)||d>=(1<<27)) runtime_fail("auto-attach branch outside range");
    return (call?0x94000000u:0x14000000u)|((uint32_t)(d>>2)&0x3ffffffu);
}
void runtime_auto_attach_install(Module *m) {
    /* The Mac exporter embeds its patched text in the iOS app. Never export Mac-only host callbacks. */
    if (getenv("TOTK_EXPORT_IOS")) return;
    /* Guard the caller's register roles, material queue, stock query and detach entry before writing either call. */
    static const struct { uint64_t off;uint32_t word; } guards[]={
        {0x1d1f9c8,0x97f91435},{0xe261b0,0x9434fa80},
        {0x1d1f7d0,0xf9402808},{0x1d1f804,0xaa0003f4},{0x1d1f9a0,0xf9410500},
        {0x1b64ac8,0xf9415ea8},{0x1b64bb0,0xa9ba7bfd},{0x1b64bd4,0x910a6013},
        {0xe2619c,0xf9414e68},{0xe261a0,0x910a6261},{0xd62b58,0xd10303ff},
        {0x1d1fe48,0xd103c3ff},{0x1d1eeb0,0xf9409917}
    };
    for (unsigned i=0;i<sizeof guards/sizeof *guards;i++)
        if (guards[i].off+4>m->text_end || u32((uintptr_t)m->base+guards[i].off)!=guards[i].word)
            runtime_fail("auto-attach: main does not match TOTK 1.4.3");
    /* The benchmark camera and live graphics stubs occupy the first 496 bytes of this tail. */
    uint64_t tail=(m->file_size+15)&~15ULL,stub=tail+512,data=(m->file_size+0x3fff)&~0x3fffULL;
    if (stub<m->patch_off || stub+sizeof veneer>data || data+0x4000!=m->size)
        runtime_fail("auto-attach veneer layout");
    for (size_t i=0;i<sizeof veneer;i++) if (m->base[stub+i]) runtime_fail("auto-attach padding not empty");
    uint32_t words[sizeof veneer/4];memcpy(words,veneer,sizeof words);
    words[30]=jump(stub+120,0xd62b58,1);
    words[34]=jump(stub+136,0x1b64bb0,1);
    words[37]=jump(stub+148,0x1d1fe48,1);
    words[41]=jump(stub+164,0x1b64a9c,1);
    words[48]=jump(stub+192,0x1b64a9c,0);
    words[67]=jump(stub+268,0x1b64bb0,0);
    { uint64_t value=(uintptr_t)&enabled;memcpy((uint8_t *)words+200,&value,8); }
    { uint64_t value=(uintptr_t)&enabled;memcpy((uint8_t *)words+272,&value,8); }
    { uint64_t value=(uintptr_t)runtime_hle_wrap_named((void *)decide,"TOTK arrow repeat decision");memcpy((uint8_t *)words+208,&value,8); }
    { uint64_t value=(uintptr_t)runtime_hle_wrap_named((void *)manual,"TOTK manual arrow material");memcpy((uint8_t *)words+280,&value,8); }
    { uint64_t value=(uintptr_t)m->base+0x39601e8;memcpy((uint8_t *)words+216,&value,8); }
    memcpy(m->base+stub,words,sizeof words);
    uint32_t call=jump(0x1d1f9c8,stub,1);memcpy(m->base+0x1d1f9c8,&call,4);
    call=jump(0xe261b0,stub+224,1);memcpy(m->base+0xe261b0,&call,4);
    main_base=m->base;
    char word[16];int on=runtime_setting_word("auto-attach.txt",word,sizeof word) && !strcmp(word,"on");
    log_actions=getenv("TOTK_AUTO_ATTACH_LOG") && !strcmp(getenv("TOTK_AUTO_ATTACH_LOG"),"1");
    atomic_store(&enabled,on);atomic_store(&installed,1);
    fprintf(stderr,"[auto-attach] TOTK 1.4.3 optional arrow repeat installed (%s); stock, attachment and refund use the game\n",on?"on":"off");
}
#else
int runtime_auto_attach_available(void) { return 0; }
int runtime_auto_attach_choice(void) { return 0; }
void runtime_auto_attach_choose(int on) { (void)on; }
uint64_t runtime_auto_attach_input(uint64_t buttons) { return buttons; }
void runtime_auto_attach_install(Module *m) { (void)m; }
#endif

/* Exercise the actual Mac frontend and key-1 dispatch, without a guest or GPU
   commands. Build with -undefined dynamic_lookup for unused runtime hooks.
   Run with a temporary profile; repeat with MTL_HUD_ENABLED=0 and logging=1. */
#include "../games/totk/runtime/frontend.m"
#include <assert.h>

uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t dm_nvn_frame_count(void) { return 0; }
void runtime_gamepad_start(void) {}
void runtime_input_clear(void) {}
void runtime_oe_push_message(uint32_t message) { (void)message; }
void hle_signal_system_event(void *event) { (void)event; }
static void no_guest(void) {}

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 2);
    const char *hud=getenv("MTL_HUD_ENABLED"), *log=getenv("MTL_HUD_LOG_ENABLED");
    BOOL expectedHUD=!hud || atoi(hud)!=0, expectedLog=log && atoi(log)!=0;
    setenv("TOTK_PROFILE_DIR",argv[1],1);
    setenv("TOTK_ASSUME_FOCUS","1",1);
    setenv("TOTK_SLOT_OWNER","codex",1);
    setenv("TOTK_SLOT_PURPOSE","Metal HUD controls (no GPU work)",1);
    setenv("TOTK_WINDOW_SIZE","640x360",1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_main_queue(),^{
        [window orderOut:nil];
        assert([metalLayer.developerHUDProperties[@"mode"] isEqual:expectedHUD?@"main":@"disabled"]);
        assert([metalLayer.developerHUDProperties[@"logging"] isEqual:expectedLog?@"default":@"disabled"]);
        assert(fps_overlay_enabled());
        assert(!strcmp(getenv("TOTK_LOAD_OVERLAY"),"1"));
        assert(!strcmp(getenv("MTL_HUD_ALIGNMENT"),"topleft"));
        assert(fps_overlay_hints_enabled() && !shortcutHelp.hidden);
        NSMutableDictionary *properties=[metalLayer.developerHUDProperties mutableCopy];
        properties[@"positionX"]=@17; metalLayer.developerHUDProperties=properties;
        runtime_shortcut_help_toggle();
        dispatch_async(dispatch_get_main_queue(),^{
            assert(shortcutHelp.hidden && !fps_overlay_hints_enabled());
            assert([metalLayer.developerHUDProperties[@"mode"] isEqual:@"disabled"]);
            runtime_shortcut_help_toggle();
            dispatch_async(dispatch_get_main_queue(),^{
                assert(!shortcutHelp.hidden && fps_overlay_hints_enabled());
                assert([metalLayer.developerHUDProperties[@"mode"] isEqual:expectedHUD?@"main":@"disabled"]);
                assert([metalLayer.developerHUDProperties[@"logging"] isEqual:expectedLog?@"default":@"disabled"]);
                assert([metalLayer.developerHUDProperties[@"positionX"] isEqual:@17]);
                puts("pass: frontend HUD defaults, opt-out/logging, key 1 hide/restore, preserved layer configuration (no GPU work)");
                fflush(stdout); _exit(0);
            });
        });
    });
    runtime_front_run(no_guest);
}}

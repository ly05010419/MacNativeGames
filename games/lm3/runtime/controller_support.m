#include "runtime.h"
#include "lm3_controller_policy.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

enum { LM3_CONTROLLER_CANCELLED = 0x183aca };
_Static_assert(sizeof(RuntimeControllerSupportResult) == 8, "controller support result ABI");
static _Atomic int controller_request_active;
static void controller_request_release(int *owned) {
    if (*owned) atomic_store(&controller_request_active,0);
}

#if TARGET_OS_OSX
#import <Cocoa/Cocoa.h>
#include "lm3_controller_dispatch.h"

@interface LM3ControllerSupportPanel : NSWindowController <NSWindowDelegate>
@property int minimum, maximum, singleMode;
@property unsigned allowedMask;
@property(strong) NSTextField *summary;
@property(strong) NSArray<NSTextField *> *playerLabels;
@property LM3ControllerChoice choice;
@property BOOL accepted;
@property BOOL finished;
-(void)poll;
-(void)cancel:(id)sender;
@end

@implementation LM3ControllerSupportPanel
-(instancetype)init {
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,480,230)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:panel])) return nil;
    panel.title = @"连接游戏手柄"; panel.delegate = self; panel.releasedWhenClosed = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    NSView *view = panel.contentView;
    NSTextField *title = [NSTextField labelWithString:@"连接游戏手柄"];
    title.font = [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(24,184,432,26); [view addSubview:title];
    self.summary = [NSTextField wrappingLabelWithString:@"正在等待手柄连接……"];
    self.summary.frame = NSMakeRect(24,140,432,38); [view addSubview:self.summary];
    NSMutableArray *labels = [NSMutableArray array];
    for (unsigned i=0;i<2;i++) {
        NSTextField *label = [NSTextField labelWithString:@""];
        label.frame = NSMakeRect(24,107-32*i,432,24); [view addSubview:label]; [labels addObject:label];
    }
    self.playerLabels = labels;
    NSButton *cancel = [NSButton buttonWithTitle:@"取消，返回游戏" target:self action:@selector(cancel:)];
    cancel.frame = NSMakeRect(298,20,158,32); cancel.bezelStyle = NSBezelStyleRounded;
    cancel.keyEquivalent = @"\033"; [view addSubview:cancel];
    return self;
}
-(void)poll {
    if (self.finished || NSApp.modalWindow != self.window) return;
    if (!NSApp.running) { [self cancel:nil]; return; }
    unsigned physical = runtime_gamepad_connected_mask();
    for (unsigned i=0;i<2;i++) {
        char name[160]; runtime_gamepad_player_name(i,name,sizeof name);
        NSString *status = !(self.allowedMask & (1u<<i)) ? @"本次游戏未启用" :
            physical & (1u<<i) ? ([NSString stringWithUTF8String:name] ?: @"已连接") : @"等待连接";
        self.playerLabels[i].stringValue = [NSString stringWithFormat:@"玩家 %u：%@",i+1,status];
    }
    LM3ControllerChoice selected;
    int ready = lm3_controller_choose(self.minimum,self.maximum,self.allowedMask,physical,self.singleMode,&selected);
    if (ready > 0) { self.choice = selected; self.accepted = YES; self.finished = YES; [NSApp stopModal]; return; }
    self.summary.stringValue = ready < 0 ? @"当前版本支持最多两位玩家。请取消后选择单人或双人模式。" :
        [NSString stringWithFormat:@"本次需要 %d 位玩家。连接完成后会自动返回游戏；请以列表中的玩家编号为准。",self.minimum>0?self.minimum:1];
}
-(void)cancel:(id)sender { (void)sender; if(self.finished)return;self.finished=YES;self.accepted = NO; [NSApp abortModal]; }
-(BOOL)windowShouldClose:(NSWindow *)sender { (void)sender; [self cancel:nil]; return NO; }
@end
#endif

uint32_t runtime_controller_support_run(int minimum,int maximum,unsigned allowed_mask,int single_mode,
                                        RuntimeControllerSupportResult *out) {
    if (out) *out = (RuntimeControllerSupportResult){0};
    int expected = 0;
    if (!atomic_compare_exchange_strong(&controller_request_active,&expected,1)) return LM3_CONTROLLER_CANCELLED;
    __attribute__((cleanup(controller_request_release))) int owns_request = 1;
    LM3ControllerChoice choice;
    int ready = lm3_controller_choose(minimum,maximum,allowed_mask,runtime_gamepad_connected_mask(),single_mode,&choice);
    if (ready < 0) return LM3_CONTROLLER_CANCELLED;
    if (!ready) {
#if TARGET_OS_OSX
        /* CPU probes and command-line runs have no UI event loop. Never block
           them or return success with fewer players than the game requested. */
        if (!NSApp || !NSApp.running || getenv("TOTK_HEADLESS") || getenv("LM3_INPUT_PROBE")) return LM3_CONTROLLER_CANCELLED;
        /* The synchronous SDK is called by a guest thread. A main-thread
           caller cannot wait for this UI without risking queue reentrancy. */
        if (NSThread.isMainThread) return LM3_CONTROLLER_CANCELLED;
        runtime_input_suspend(RUNTIME_INPUT_SUSPEND_PAIRING,1);
        __block BOOL accepted = NO;
        __block LM3ControllerChoice selection = {0};
        void (^show)(void) = ^{
            LM3ControllerSupportPanel *controller = [LM3ControllerSupportPanel new];
            controller.minimum = minimum; controller.maximum = maximum;
            controller.allowedMask = allowed_mask; controller.singleMode = single_mode;
            NSWindow *parent = NSApp.mainWindow;
            if (parent) [controller.window setFrameOrigin:NSMakePoint(NSMidX(parent.frame)-240,NSMidY(parent.frame)-115)];
            else [controller.window center];
            [controller.window makeKeyAndOrderFront:nil];
            NSTimer *timer = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) { (void)t; [controller poll]; }];
            [NSRunLoop.mainRunLoop addTimer:timer forMode:NSModalPanelRunLoopMode];
            [NSApp runModalForWindow:controller.window];
            [timer invalidate]; [controller.window orderOut:nil];
            accepted = controller.accepted; selection = controller.choice;
            if (parent) [parent makeKeyAndOrderFront:nil];
        };
        int away = runtime_core_away();
        lm3_controller_perform_main_and_wait(show);
        runtime_core_back(&away);
        runtime_input_suspend(RUNTIME_INPUT_SUSPEND_PAIRING,0);
        if (!accepted) return LM3_CONTROLLER_CANCELLED;
        choice = selection;
#else
        return LM3_CONTROLLER_CANCELLED;
#endif
    }
    if (out) *out = (RuntimeControllerSupportResult){.players=(int8_t)choice.count,.selected=choice.selected};
    fprintf(stderr,"[lm3-input] controller selection: %u player(s), mask=%#x, selected=%u\n",choice.count,choice.mask,choice.selected);
    return 0;
}

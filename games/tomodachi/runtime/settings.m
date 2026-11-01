/* Tomodachi's native settings. Preferences stay beside this title's profile;
   diagnostic save-copy sessions therefore keep independent settings too. */
#import <Cocoa/Cocoa.h>
#include "runtime.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

extern void fps_overlay_set_enabled(int);
extern void runtime_front_refresh_drawable(void);
static _Atomic int panelOpen, prepared, lowResolution, swapButtons;
static _Atomic unsigned keyMap[128];
static NSMutableDictionary *preferences;
static NSString *preferencesPath;
static NSWindow *gameWindow;
static NSArray<NSNumber *> *actionCodes;
static NSArray<NSString *> *actionNames;
static NSArray<NSNumber *> *availableCodes;
static NSArray<NSString *> *availableNames;
static NSArray<NSString *> *languages;
int runtime_title_settings_key(unsigned);
static void title_controls_load(void);
static uint64_t title_map_buttons(uint64_t);
static NSView *performanceView;
static NSTimer *performanceTimer;
extern int fps_overlay_value(void);
double runtime_title_drawable_scale(void);
static NSString *build_description(void) {
    NSString *commit=[NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGSourceCommit"] ?: @"development";
    return [NSString stringWithFormat:@"Ver. 1.0.0 · %@ · %@",DM_NVN_M3?@"Metal 3":@"Metal 4",commit];
}
@interface TomodachiPerformanceView : NSView
@property NSMutableArray<NSNumber *> *samples;
@end
@implementation TomodachiPerformanceView
- (BOOL)isOpaque{return NO;}
- (NSView *)hitTest:(NSPoint)point{(void)point;return nil;}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;[[NSColor colorWithWhite:0 alpha:.78] setFill];[[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:10 yRadius:10] fill];
    NSColor *green=[NSColor colorWithRed:.35 green:1 blue:.55 alpha:1];
    NSDictionary *style=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],NSForegroundColorAttributeName:green};
    CGFloat scale=gameWindow.backingScaleFactor*runtime_title_drawable_scale();NSSize size=gameWindow.contentView.bounds.size;
    NSString *heading=[NSString stringWithFormat:@"FPS %d     %.0f × %.0f\n%@\nSettings: key 1",fps_overlay_value(),round(size.width*scale),round(size.height*scale),build_description()];
    [heading drawInRect:NSMakeRect(14,98,self.bounds.size.width-28,65) withAttributes:style];
    if(self.samples.count<2)return;
    int lo=INT_MAX,hi=0;for(NSNumber *sample in self.samples){lo=MIN(lo,sample.intValue);hi=MAX(hi,sample.intValue);}
    [[NSString stringWithFormat:@"min %d   max %d   current %d",lo,hi,fps_overlay_value()] drawAtPoint:NSMakePoint(14,76) withAttributes:style];
    NSRect chart=NSMakeRect(16,17,self.bounds.size.width-32,50);NSBezierPath *path=[NSBezierPath bezierPath];
    for(NSUInteger i=0;i<self.samples.count;i++){NSPoint point=NSMakePoint(chart.origin.x+chart.size.width*i/(self.samples.count-1),chart.origin.y+chart.size.height*self.samples[i].doubleValue/MAX(60,hi));if(i)[path lineToPoint:point];else[path moveToPoint:point];}
    [green setStroke];path.lineWidth=1.5;[path stroke];
}
@end

int runtime_title_input_blocked(void) { return atomic_load(&panelOpen); }
double runtime_title_drawable_scale(void) { return atomic_load(&lowResolution) ? .5 : 1; }
unsigned runtime_title_map_key(unsigned key) {
    return key < 128 && atomic_load(&prepared) ? atomic_load(&keyMap[key]) : key;
}
uint64_t runtime_title_map_buttons(uint64_t buttons) { return atomic_load(&prepared)?title_map_buttons(buttons):buttons; }
static void load_keymap(void) {
    for (unsigned i = 0; i < 128; i++) atomic_store(&keyMap[i], i);
    NSArray *saved = preferences[@"keys"];
    if (![saved isKindOfClass:NSArray.class] || saved.count != actionCodes.count) { [preferences removeObjectForKey:@"keys"]; return; }
    NSMutableSet *seen = [NSMutableSet set];
    for (id code in saved) {
        if (![code isKindOfClass:NSNumber.class] || ![availableCodes containsObject:code] || [seen containsObject:code]) { [preferences removeObjectForKey:@"keys"]; return; }
        [seen addObject:code];
    }
    /* Unbind old keys first, then assign all selected keys (including swaps). */
    for (NSNumber *code in actionCodes) atomic_store(&keyMap[code.unsignedIntValue], 127);
    for (NSUInteger i = 0; i < saved.count; i++) atomic_store(&keyMap[[saved[i] unsignedIntValue]], actionCodes[i].unsignedIntValue);
}
void runtime_title_settings_prepare(void) {
    if (atomic_load(&prepared)) return;
    actionCodes = @[@38,@40,@34,@32,@6,@8,@12,@14,@36,@48];
    actionNames = @[@"A — Confirm",@"B — Back",@"X — Menu",@"Y — Miis",@"L",@"R",@"ZL",@"ZR",@"+",@"−"];
    availableCodes = @[@38,@40,@34,@32,@6,@8,@12,@14,@36,@48,@49,@7,@9,@11,@45,@46];
    availableNames = @[@"J",@"K",@"I",@"U",@"Z",@"C",@"Q",@"E",@"Return",@"Tab",@"Space",@"X",@"V",@"B",@"N",@"M"];
    languages = @[@"",@"en-US",@"ja",@"zh-Hant",@"zh-Hans",@"ko",@"fr",@"de",@"es",@"it",@"nl",@"pt-BR"];
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (profile) preferencesPath = [[NSString stringWithUTF8String:profile] stringByAppendingPathComponent:@"settings.json"];
    NSData *data = preferencesPath ? [NSData dataWithContentsOfFile:preferencesPath] : nil;
    id loaded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    preferences = [loaded isKindOfClass:NSDictionary.class] ? [loaded mutableCopy] : [NSMutableDictionary dictionary];
    atomic_store(&lowResolution, [preferences[@"lowResolution"] boolValue]);
    atomic_store(&swapButtons, [preferences[@"swapButtons"] boolValue]);
    NSString *language = preferences[@"language"];
    if ([language isKindOfClass:NSString.class] && language.length && [languages containsObject:language]) setenv("TOTK_LANG", language.UTF8String, 1);
    load_keymap();title_controls_load(); atomic_store(&prepared, 1);
}
static void save_preferences(void) {
    if (!preferencesPath) return;
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:preferences options:NSJSONWritingPrettyPrinted error:&error];
    if (data && ![data writeToFile:preferencesPath options:NSDataWritingAtomic error:&error])
        fprintf(stderr,"[tomodachi-settings] Save failed: %s\n",error.localizedDescription.UTF8String);
}
#include "settings_controls.h"
static NSTextField *label(NSView *view, NSString *text, CGFloat y) {
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = NSMakeRect(24,y,530,24); field.font = [NSFont systemFontOfSize:13]; [view addSubview:field]; return field;
}
@interface TomodachiSettingsPanel : NSView
@end
@interface TomodachiSettings : NSObject<NSWindowDelegate>
@property TomodachiSettingsPanel *panel;
@property NSMutableArray<NSPopUpButton *> *bindings;
@property NSPopUpButton *windowModes;
@property NSResponder *previousResponder;
@property NSTabView *tabs;
@property TOTKKeyboardDiagram *keyboardDiagram;
@property TOTKControllerDiagram *padDiagram;
@property NSSegmentedControl *deviceTabs;
@property NSSegmentedControl *pageTabs;
@property NSInteger padIndex;
@property uint64_t previousPad;
@property int previousDirection;
@property double repeatAt;
- (void)controlsTick;
- (void)layoutPanel;
- (void)assignInput:(int)input action:(int)action;
- (void)assignPad:(int)physical action:(int)action;
- (void)show:(id)sender;
- (void)close:(id)sender;
@end
static TomodachiSettings *settings;
@implementation TomodachiSettingsPanel
- (BOOL)acceptsFirstResponder{return YES;}
- (void)keyDown:(NSEvent *)event {
    if (event.keyCode == 18 || event.keyCode == 53) [settings close:nil];
    else [super keyDown:event];
}
@end
@implementation TomodachiSettings
- (NSView *)row:(NSView *)view title:(NSString *)title y:(CGFloat)y {
    NSView *row=[[NSView alloc] initWithFrame:NSMakeRect(0,y,932,34)];row.wantsLayer=YES;row.layer.cornerRadius=6;row.layer.backgroundColor=[NSColor colorWithWhite:0 alpha:.28].CGColor;[view addSubview:row];
    NSTextField *caption=label(row,title,12);caption.frame=NSMakeRect(12,8,620,18);caption.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];return row;
}
- (NSPopUpButton *)popup:(NSView *)view title:(NSString *)title items:(NSArray<NSString *> *)items y:(CGFloat)y action:(SEL)action {
    NSView *row=[self row:view title:title y:y];
    NSPopUpButton *popup=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(664,4,260,26) pullsDown:NO];[popup addItemsWithTitles:items];popup.target=self;popup.action=action;[popup setAccessibilityLabel:title];[row addSubview:popup];return popup;
}
- (NSSwitch *)toggle:(NSView *)view title:(NSString *)title key:(NSString *)key y:(CGFloat)y action:(SEL)action defaultValue:(BOOL)fallback {
    NSView *row=[self row:view title:title y:y];NSSwitch *button=[[NSSwitch alloc] initWithFrame:NSMakeRect(882,4,42,26)];button.target=self;button.action=action;button.state=preferences[key]?[preferences[key] boolValue]:fallback;[button setAccessibilityLabel:title];[row addSubview:button];return button;
}
- (void)tab:(NSSegmentedControl *)sender{[self.tabs selectTabViewItemAtIndex:sender.selectedSegment];self.padIndex=0;[self layoutPanel];}
- (void)build {
    self.panel=[[TomodachiSettingsPanel alloc] initWithFrame:NSMakeRect(0,0,960,530)];
    NSView *root=self.panel;root.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];root.wantsLayer=YES;
    root.layer.backgroundColor=[NSColor colorWithCalibratedRed:.10 green:.16 blue:.20 alpha:.9].CGColor;root.layer.cornerRadius=12;root.layer.borderWidth=1;root.layer.borderColor=[NSColor colorWithWhite:1 alpha:.12].CGColor;
    root.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
    NSTextField *title=label(root,@"Settings",482);title.frame=NSMakeRect(24,482,490,30);title.font=[NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    NSButton *close=[NSButton buttonWithTitle:@"×" target:self action:@selector(close:)];close.frame=NSMakeRect(914,480,30,30);close.bordered=NO;close.font=[NSFont systemFontOfSize:24];[close setAccessibilityLabel:@"Close Settings"];[root addSubview:close];
    NSSegmentedControl *segments=[NSSegmentedControl segmentedControlWithLabels:@[@"Display",@"Input",@"Key bindings"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tab:)];segments.frame=NSMakeRect(14,450,932,26);segments.selectedSegment=0;for(NSInteger i=0;i<3;i++)[segments setWidth:310 forSegment:i];segments.segmentDistribution=NSSegmentDistributionFillEqually;segments.refusesFirstResponder=YES;self.pageTabs=segments;[root addSubview:segments];
    self.tabs=[[NSTabView alloc] initWithFrame:NSMakeRect(14,54,932,380)];self.tabs.tabViewType=NSNoTabsNoBorder;[root addSubview:self.tabs];
    NSMutableArray<NSView *> *pages=[NSMutableArray array];
    for(NSString *name in @[@"Display",@"Input",@"Key bindings"]){NSTabViewItem *tab=[[NSTabViewItem alloc] initWithIdentifier:name];tab.label=name;tab.view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,932,380)];[self.tabs addTabViewItem:tab];[pages addObject:tab.view];}
    NSView *display=pages[0];
    self.windowModes=[self popup:display title:@"Window mode" items:@[@"Windowed",@"Full Screen"] y:332 action:@selector(windowMode:)];
    NSPopUpButton *quality=[self popup:display title:@"Display quality" items:@[@"Native Retina",@"Performance"] y:292 action:@selector(quality:)];[quality selectItemAtIndex:atomic_load(&lowResolution)];
    [self toggle:display title:@"Performance overlay" key:@"fps" y:252 action:@selector(fps:) defaultValue:NO];
    NSPopUpButton *language=[self popup:display title:@"Game language" items:@[@"Use app language",@"English",@"日本語",@"繁體中文",@"简体中文",@"한국어",@"Français",@"Deutsch",@"Español",@"Italiano",@"Nederlands",@"Português (Brasil)"] y:212 action:@selector(language:)];
    NSUInteger languageIndex=[languages indexOfObject:preferences[@"language"]?:@""];[language selectItemAtIndex:languageIndex==NSNotFound?0:languageIndex];
    NSTextField *hint=label(display,@"Language changes apply the next time you open the app.",182);hint.frame=NSMakeRect(14,182,530,24);hint.textColor=NSColor.secondaryLabelColor;
    hint=label(display,@"Performance quality reduces display pixels for a lighter GPU load.",146);hint.frame=NSMakeRect(14,146,530,24);hint.textColor=NSColor.secondaryLabelColor;
    NSView *controls=pages[1];
    NSPopUpButton *type=[self popup:controls title:@"Controller type" items:@[@"Auto",@"Xbox",@"PlayStation",@"Switch"] y:332 action:@selector(controllerType:)];[type selectItemAtIndex:atomic_load(&titlePadType)];
    NSPopUpButton *buttons=[self popup:controls title:@"Controller face buttons" items:@[@"Default",@"Swap A/B and X/Y"] y:292 action:@selector(buttons:)];[buttons selectItemAtIndex:atomic_load(&swapButtons)];
    [self toggle:controls title:@"Controller vibration" key:@"rumble" y:252 action:@selector(rumble:) defaultValue:YES];
    label(controls,@"Open settings with L3 + R3. Use L / R to change tabs.",200);label(controls,@"Keyboard and controller diagrams show the bindings in force.",164);label(controls,@"The mouse remains Tomodachi’s touch input.",128);
    NSView *keys=pages[2];
    NSSegmentedControl *device=[NSSegmentedControl segmentedControlWithLabels:@[@"Keyboard & mouse",@"Controller"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(deviceChanged:)];device.frame=NSMakeRect(4,350,330,24);device.segmentDistribution=NSSegmentDistributionFillEqually;device.selectedSegment=1;self.deviceTabs=device;[keys addSubview:device];
    NSTextField *help=label(keys,@"Click a key or button, then choose what it does.",350);help.frame=NSMakeRect(350,350,570,20);
    NSRect area=NSMakeRect(0,30,932,310);__weak TomodachiSettings *weak=self;
    self.keyboardDiagram=[[TOTKKeyboardDiagram alloc] initWithFrame:area];self.padDiagram=[[TOTKControllerDiagram alloc] initWithFrame:area];
    self.keyboardDiagram.pick=^(int input,NSRect at){[weak popInput:input pad:NO at:at];};self.padDiagram.pick=^(int input,NSRect at){[weak popInput:input pad:YES at:at];};
    [keys addSubview:self.keyboardDiagram];[keys addSubview:self.padDiagram];[self deviceChanged:device];
    NSButton *resetKeys=[NSButton buttonWithTitle:@"Reset keyboard" target:self action:@selector(resetKeyboard:)];resetKeys.frame=NSMakeRect(4,0,160,28);[keys addSubview:resetKeys];
    NSButton *resetPad=[NSButton buttonWithTitle:@"Reset controller" target:self action:@selector(resetController:)];resetPad.frame=NSMakeRect(172,0,160,28);[keys addSubview:resetPad];
    NSTextField *footer=label(root,build_description(),24);footer.frame=NSMakeRect(24,24,552,24);footer.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];footer.textColor=NSColor.secondaryLabelColor;
}
- (void)show:(id)sender {
    (void)sender; if(atomic_load(&panelOpen)){[gameWindow makeFirstResponder:self.panel];return;}
    if(!self.panel)[self build];
    [self.windowModes selectItemAtIndex:!!(gameWindow.styleMask&NSWindowStyleMaskFullScreen)];
    self.previousResponder=gameWindow.firstResponder;
    runtime_input_clear(); extern void runtime_title_pointer(int,double,double);runtime_title_pointer(3,0,0);
    atomic_store(&panelOpen,1); runtime_feedback_quiet();[self layoutPanel];if(!self.panel.superview)[gameWindow.contentView addSubview:self.panel];self.panel.hidden=NO;[gameWindow makeFirstResponder:self.panel];
}
- (void)close:(id)sender {
    (void)sender;self.panel.hidden=YES; runtime_input_clear(); atomic_store(&panelOpen,0);
    NSView *previous=[self.previousResponder isKindOfClass:NSView.class]?(NSView *)self.previousResponder:nil;
    [gameWindow makeFirstResponder:previous.window==gameWindow?previous:gameWindow.contentView];self.previousResponder=nil;
}
- (void)windowMode:(NSPopUpButton *)sender { BOOL full=!!(gameWindow.styleMask&NSWindowStyleMaskFullScreen);BOOL desired=sender.indexOfSelectedItem==1;preferences[@"fullScreen"]=@(desired);save_preferences();if(full!=desired){[self close:nil];[gameWindow toggleFullScreen:nil];} }
- (void)quality:(NSPopUpButton *)sender { preferences[@"lowResolution"]=@(sender.indexOfSelectedItem);atomic_store(&lowResolution,sender.indexOfSelectedItem);runtime_front_refresh_drawable();save_preferences(); }
- (void)fps:(NSButton *)sender { preferences[@"fps"]=@(sender.state==NSControlStateValueOn);performanceView.hidden=sender.state!=NSControlStateValueOn;save_preferences(); }
- (void)language:(NSPopUpButton *)sender { preferences[@"language"]=languages[sender.indexOfSelectedItem];save_preferences(); }
- (void)buttons:(NSPopUpButton *)sender { preferences[@"swapButtons"]=@(sender.indexOfSelectedItem);atomic_store(&swapButtons,sender.indexOfSelectedItem);for(int i=0;i<4;i++)atomic_store(&titlePadMap[i],1u<<(sender.indexOfSelectedItem?(i^1):i));title_controls_save();[self.padDiagram setNeedsDisplay:YES]; }
- (void)rumble:(NSButton *)sender { BOOL on=sender.state==NSControlStateValueOn;preferences[@"rumble"]=@(on);runtime_hid_feedback_flag("rumble",on);save_preferences(); }
#include "settings_controls_ui.h"

@end
int runtime_title_settings_key(unsigned key) { if(key!=18)return 0;if(atomic_load(&panelOpen))[settings close:nil];else[settings show:nil];return 1; }
void runtime_title_frontend_ready(void *window) {
    runtime_title_settings_prepare(); gameWindow=(__bridge NSWindow *)window; settings=[TomodachiSettings new];
    fps_overlay_set_enabled(0);
    TomodachiPerformanceView *hud=[[TomodachiPerformanceView alloc] initWithFrame:NSMakeRect(MAX(8,gameWindow.contentView.bounds.size.width-390),MAX(8,gameWindow.contentView.bounds.size.height-188),380,174)];hud.samples=[NSMutableArray array];hud.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;hud.hidden=![preferences[@"fps"] boolValue];[gameWindow.contentView addSubview:hud];performanceView=hud;
    performanceTimer=[NSTimer timerWithTimeInterval:.03 repeats:YES block:^(NSTimer *timer){(void)timer;[settings controlsTick];static double last;double now=NSProcessInfo.processInfo.systemUptime;if(hud.hidden||now-last<.5)return;last=now;int fps=fps_overlay_value();if(fps<0)return;[hud.samples addObject:@(fps)];if(hud.samples.count>90)[hud.samples removeObjectAtIndex:0];[hud setNeedsDisplay:YES];}];[NSRunLoop.mainRunLoop addTimer:performanceTimer forMode:NSRunLoopCommonModes];
    if(runtime_rumble_enabled())runtime_hid_feedback_flag("rumble",preferences[@"rumble"]?[preferences[@"rumble"] boolValue]:1);
    if([preferences[@"fullScreen"] boolValue]&&!getenv("TOTK_FULLSCREEN"))dispatch_async(dispatch_get_main_queue(), ^{if(!(gameWindow.styleMask&NSWindowStyleMaskFullScreen))[gameWindow toggleFullScreen:nil];});
    if(!NSApp.mainMenu)NSApp.mainMenu=[NSMenu new];
    NSMenuItem *appItem=NSApp.mainMenu.itemArray.firstObject;
    if(!appItem){appItem=[NSMenuItem new];[NSApp.mainMenu addItem:appItem];}
    if(!appItem.submenu)appItem.submenu=[[NSMenu alloc] initWithTitle:@"Tomodachi Life"];
    NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(show:) keyEquivalent:@","]; item.target=settings;[appItem.submenu addItem:item];
    [appItem.submenu addItem:NSMenuItem.separatorItem];
    NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@"Quit Tomodachi Life" action:@selector(performClose:) keyEquivalent:@"q"];quit.target=gameWindow;[appItem.submenu addItem:quit];
}

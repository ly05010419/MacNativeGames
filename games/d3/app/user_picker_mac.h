#import <AppKit/AppKit.h>
#include "../runtime/user_store.h"
#include "../runtime/user_avatar.h"

/* Keep the grid usable on smaller displays, and never exceed six columns. */
static NSUInteger mng_user_grid_columns(CGFloat width) {
    return MAX((NSUInteger)1, MIN((NSUInteger)6, (NSUInteger)MAX(1, floor((width + 12) / 144))));
}

@interface MNGUserCard : NSButton
@property(nonatomic) NSUInteger userID;
@property(nonatomic) NSUInteger columns;
@property(nonatomic) BOOL chosen;
@property(nonatomic,copy) NSString *userName;
@property(nonatomic,copy) NSString *avatar;
@end
@implementation MNGUserCard
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    BOOL selected = self.chosen;
    NSRect box = NSInsetRect(self.bounds, 3, 3);
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:box xRadius:16 yRadius:16];
    [(selected ? [NSColor.controlAccentColor colorWithAlphaComponent:0.12] : NSColor.controlBackgroundColor) setFill];
    [path fill];
    [(selected ? NSColor.controlAccentColor : NSColor.separatorColor) setStroke];
    path.lineWidth = selected ? 2.5 : 1; [path stroke];
    if (self.highlighted) { [[NSColor.controlAccentColor colorWithAlphaComponent:0.08] setFill]; [path fill]; }
    if (self.window.firstResponder == self && self.window.isKeyWindow) {
        NSBezierPath *focus=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(box,4,4) xRadius:12 yRadius:12];
        CGFloat dash[]={3,3};[focus setLineDash:dash count:2 phase:0];
        [NSColor.controlAccentColor setStroke];focus.lineWidth=1;[focus stroke];
    }
    NSRect avatar = NSMakeRect((NSWidth(self.bounds)-64)/2, NSHeight(self.bounds)-84, 64, 64);
    [[NSColor.controlAccentColor colorWithAlphaComponent:0.10] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:avatar] fill];
    NSDictionary *attributes=@{NSFontAttributeName:[NSFont systemFontOfSize:40]};
    NSSize size=[self.avatar sizeWithAttributes:attributes];
    [self.avatar drawAtPoint:NSMakePoint(NSMidX(avatar)-size.width/2,NSMidY(avatar)-size.height/2) withAttributes:attributes];
    NSMutableParagraphStyle *center = [NSMutableParagraphStyle new];
    center.alignment = NSTextAlignmentCenter; center.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.userName drawInRect:NSMakeRect(10, 39, NSWidth(self.bounds)-20, 22)
              withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:14 weight:NSFontWeightSemibold],
                               NSForegroundColorAttributeName:NSColor.labelColor, NSParagraphStyleAttributeName:center}];
    NSString *detail = self.enabled ? [NSString stringWithFormat:selected ? @"✓  ID %02lu" : @"ID %02lu", (unsigned long)self.userID] : @"使用中 / Busy";
    [detail drawInRect:NSMakeRect(10, 16, NSWidth(self.bounds)-20, 18)
       withAttributes:@{NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium],
                        NSForegroundColorAttributeName:selected ? NSColor.controlAccentColor : NSColor.secondaryLabelColor,
                        NSParagraphStyleAttributeName:center}];
}
- (void)keyDown:(NSEvent *)event {
    NSInteger step = 0;
    switch (event.keyCode) {
        case 123: step = -1; break; case 124: step = 1; break;
        case 125: step = (NSInteger)self.columns; break; case 126: step = -(NSInteger)self.columns; break;
        default: [super keyDown:event]; return;
    }
    NSArray<NSView *> *cards = self.superview.subviews;
    NSInteger next = (NSInteger)[cards indexOfObject:self] + step;
    if (next >= 0 && next < (NSInteger)cards.count) {
        MNGUserCard *card = (MNGUserCard *)cards[(NSUInteger)next];
        [self.window makeFirstResponder:card]; [card performClick:nil]; [card scrollRectToVisible:card.bounds];
    }
}
@end

@interface MNGUserBackground : NSView
@end
@implementation MNGUserBackground
- (void)drawRect:(NSRect)dirtyRect { [NSColor.windowBackgroundColor setFill];NSRectFill(dirtyRect); }
@end

@interface MNGUserGrid : NSView
@end
@implementation MNGUserGrid
- (BOOL)isFlipped { return YES; }
@end

@interface MNGUserWindow : NSWindowController <NSWindowDelegate>
@property(nonatomic,strong) MNGUserStore *store;
@property(nonatomic,strong) NSScrollView *scroll;
@property(nonatomic,strong) MNGUserGrid *grid;
@property(nonatomic,strong) NSButton *playButton;
@property(nonatomic,strong) NSButton *addButton;
@property(nonatomic,strong) NSButton *renameButton;
@property(nonatomic) NSUInteger selected;
@property(nonatomic) BOOL accepted;
@property(nonatomic,strong) NSPopover *avatarPopover;
- (instancetype)initWithProfile:(NSString *)profile;
- (BOOL)reloadUsers;
- (void)layoutCards;
- (void)choose:(MNGUserCard *)sender;
- (void)play:(id)sender;
- (void)refreshAvailability;
- (void)cancel:(id)sender;
- (void)addUser:(id)sender;
- (void)renameUser:(id)sender;
@end
@implementation MNGUserWindow
- (instancetype)initWithProfile:(NSString *)profile {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,936,560)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) return nil;
    self.store = [[MNGUserStore alloc] initWithRoot:profile];
    window.title = @"选择存档用户 / Choose save user"; window.delegate = self;
    window.contentMinSize = NSMakeSize(920,420); window.releasedWhenClosed = NO;
    window.contentView=[[MNGUserBackground alloc] initWithFrame:window.contentView.bounds];
    NSView *content = window.contentView;
    NSTextField *heading = [NSTextField labelWithString:@"谁来开始这次冒险？"];
    heading.font = [NSFont systemFontOfSize:26 weight:NSFontWeightSemibold];
    heading.frame = NSMakeRect(28,490,880,38); heading.autoresizingMask = NSViewWidthSizable|NSViewMinYMargin;
    [content addSubview:heading];
    NSTextField *note = [NSTextField labelWithString:@"选择一位用户继续 · 每位用户拥有独立存档\nChoose your profile · Each user has separate game progress"];
    note.font = [NSFont systemFontOfSize:12]; note.textColor = NSColor.secondaryLabelColor;
    note.maximumNumberOfLines = 2; note.frame = NSMakeRect(28,442,880,40);
    note.autoresizingMask = NSViewWidthSizable|NSViewMinYMargin; [content addSubview:note];
    self.scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(24,88,888,342)];
    self.scroll.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
    self.scroll.hasVerticalScroller = YES; self.scroll.autohidesScrollers = YES;
    self.scroll.drawsBackground = NO; self.scroll.borderType = NSNoBorder;
    self.grid = [[MNGUserGrid alloc] initWithFrame:NSZeroRect]; self.scroll.documentView = self.grid;
    [content addSubview:self.scroll];
    NSBox *line = [[NSBox alloc] initWithFrame:NSMakeRect(24,73,888,1)]; line.boxType = NSBoxSeparator;
    line.autoresizingMask = NSViewWidthSizable; [content addSubview:line];
    NSButton *cancel = [NSButton buttonWithTitle:@"取消 / Cancel" target:self action:@selector(cancel:)];
    cancel.frame = NSMakeRect(24,24,108,32); cancel.keyEquivalent = @"\033"; [content addSubview:cancel];
    self.addButton = [NSButton buttonWithTitle:@"＋ 新增 / Add" target:self action:@selector(addUser:)];
    self.addButton.frame = NSMakeRect(144,24,124,32); [content addSubview:self.addButton];
    self.renameButton = [NSButton buttonWithTitle:@"改名 / Rename" target:self action:@selector(renameUser:)];
    self.renameButton.frame = NSMakeRect(280,24,124,32); [content addSubview:self.renameButton];
    self.playButton = [NSButton buttonWithTitle:@"开始游戏 / Play" target:self action:@selector(play:)];
    self.playButton.frame = NSMakeRect(728,24,184,32); self.playButton.autoresizingMask = NSViewMinXMargin;
    self.playButton.keyEquivalent = @"\r"; [content addSubview:self.playButton];
    NSButton *avatar=[NSButton buttonWithTitle:@"头像 / Avatar" target:self action:@selector(changeAvatar:)];
    avatar.frame=NSMakeRect(416,24,124,32);[content addSubview:avatar];
    NSButton *remove=[NSButton buttonWithTitle:@"删除 / Delete" target:self action:@selector(deleteUser:)];
    remove.frame=NSMakeRect(552,24,124,32);[content addSubview:remove];
    return self;
}
- (void)showError:(NSString *)error {
    NSAlert *alert = [NSAlert new]; alert.messageText = @"用户操作失败 / User operation failed";
    alert.informativeText = error ?: @"Unknown error"; [alert runModal];
}
- (BOOL)reloadUsers {
    NSString *error = nil;
    if (![self.store reload:&error]) {
        self.playButton.enabled = NO; self.renameButton.enabled = NO; self.addButton.enabled = NO;
        [self showError:error]; return NO;
    }
    if (!self.store.users[@(self.selected)]) self.selected = self.store.selected;
    for (NSView *view in self.grid.subviews.copy) [view removeFromSuperview];
    for (NSNumber *uid in self.store.ids) {
        MNGUserCard *card = [[MNGUserCard alloc] initWithFrame:NSZeroRect];
        card.userID = uid.unsignedIntegerValue; card.userName = self.store.users[uid];
        card.avatar=[self.store avatarForUser:card.userID];
#ifdef MNG_MULTI_INSTANCE_USERS
        int available=mng_user_lock(self.store.root.fileSystemRepresentation,(unsigned)card.userID);
        card.enabled=available>=0;if(available>=0)close(available);

#endif
        card.title = @""; card.bordered = NO; [card setButtonType:NSButtonTypeMomentaryChange];
        card.target = self; card.action = @selector(choose:); card.toolTip = [self.store title:uid];
        [card setAccessibilityLabel:[self.store title:uid]];
        [card setAccessibilityHelp:card.enabled ? @"选择此存档用户 / Select this save user" : @"使用中 / Busy"];
        [self.grid addSubview:card];
    }
    self.playButton.enabled = YES; self.renameButton.enabled = YES; self.addButton.enabled = self.store.ids.count < 32;
    [self layoutCards]; [self updateSelection]; return YES;
}
- (void)refreshAvailability {
#ifdef MNG_MULTI_INSTANCE_USERS
    for(MNGUserCard *card in self.grid.subviews){
        int fd=mng_user_lock(self.store.root.fileSystemRepresentation,(unsigned)card.userID);
        card.enabled=fd>=0;if(fd>=0)close(fd);
        [card setAccessibilityHelp:card.enabled ? @"选择此存档用户 / Select this save user" : @"使用中 / Busy"];
    }
    [self updateSelection];
#endif
}
- (void)updateSelection {
    for (MNGUserCard *card in self.grid.subviews) {
        card.chosen = card.userID == self.selected;
#ifdef MNG_MULTI_INSTANCE_USERS
        if(card.chosen)self.playButton.enabled=card.enabled;
#endif
        card.state = card.chosen ? NSControlStateValueOn : NSControlStateValueOff;
        [card setAccessibilityValue:card.state == NSControlStateValueOn ? @"已选择 / Selected" : @"未选择 / Not selected"];
        card.needsDisplay = YES;
    }
}
- (void)layoutCards {
    CGFloat width = self.scroll.contentSize.width;
    NSUInteger columns = mng_user_grid_columns(width), count = self.grid.subviews.count;
    CGFloat cardWidth = (width - 12*(columns-1))/columns;
    NSUInteger rows = (count + columns - 1)/columns;
    self.grid.frame = NSMakeRect(0,0,width,MAX(self.scroll.contentSize.height, rows*166));
    NSUInteger index = 0;
    for (MNGUserCard *card in self.grid.subviews) {
        card.columns = columns;
        card.frame = NSMakeRect((index%columns)*(cardWidth+12), (index/columns)*166, cardWidth,154); ++index;
    }
}
- (void)windowDidResize:(NSNotification *)notification { (void)notification; [self layoutCards]; }
- (void)choose:(MNGUserCard *)sender { self.selected = sender.userID; [self updateSelection]; }
- (void)addUser:(id)sender {
    (void)sender; NSString *error = nil;
    if (![self.store edit:@"add" user:0 name:nil error:&error]) { [self showError:error]; return; }
    self.selected = self.store.selected;
    [self reloadUsers];
    for(MNGUserCard *card in self.grid.subviews)if(card.userID==self.selected){[card scrollRectToVisible:card.bounds];[self.window makeFirstResponder:card];break;}
}
- (void)renameUser:(id)sender {
    (void)sender;
    NSAlert *alert = [NSAlert new]; alert.messageText = @"用户备注 / Rename user";
    alert.informativeText = @"改名不改变用户 ID、头像或存档。最多 32 个 UTF-8 字节。\nNames do not change IDs, avatars or saves.";
    [alert addButtonWithTitle:@"保存 / Save"]; [alert addButtonWithTitle:@"取消 / Cancel"];
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0,0,360,28)];
    field.stringValue = self.store.users[@(self.selected)]; alert.accessoryView = field;
    alert.window.initialFirstResponder = field;
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        NSString *error = nil;
        if (![self.store edit:@"rename" user:self.selected name:field.stringValue error:&error]) [self showError:error];
        [self reloadUsers];
    }
}
- (void)changeAvatar:(NSButton *)sender {
    NSViewController *controller=[NSViewController new];controller.view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,324,172)];
    NSUInteger i=0;
    for(NSString *emoji in mng_user_avatar_choices()){
        NSButton *button=[NSButton buttonWithTitle:emoji target:self action:@selector(pickAvatar:)];
        button.font=[NSFont systemFontOfSize:28];button.frame=NSMakeRect(6+(i%6)*52,116-(i/6)*52,48,48);
        [controller.view addSubview:button];i++;
    }
    self.avatarPopover=[NSPopover new];self.avatarPopover.animates=NO;self.avatarPopover.contentViewController=controller;
    self.avatarPopover.behavior=NSPopoverBehaviorTransient;
    [self.avatarPopover showRelativeToRect:sender.bounds ofView:sender preferredEdge:NSRectEdgeMaxY];
}
- (void)pickAvatar:(NSButton *)sender {
    NSString *error=nil;
    if(![self.store edit:@"avatar" user:self.selected name:sender.title error:&error])[self showError:error];
    [self.avatarPopover close];[self reloadUsers];
}
- (void)deleteUser:(id)sender {
    (void)sender;NSAlert *alert=[NSAlert new];
    alert.messageText=[NSString stringWithFormat:@"删除用户 %@？",[self.store title:@(self.selected)]];
    alert.informativeText=@"存档将移至 profile/deleted-users 保留。此 ID 可重新使用，新用户不会继承旧进度。至少保留一位用户。\nSaves are archived, not erased. Reusing the ID starts fresh.";
    [alert addButtonWithTitle:@"取消 / Cancel"];[alert addButtonWithTitle:@"删除 / Delete"];
    if([alert runModal]!=NSAlertSecondButtonReturn)return;
    NSString *error=nil;
    if(![self.store edit:@"delete" user:self.selected name:nil error:&error])[self showError:error];
    [self reloadUsers];
}
- (void)play:(id)sender {
    (void)sender; NSString *error = nil;
    #ifdef MNG_MULTI_INSTANCE_USERS
    if (![self.store edit:@"reserve" user:self.selected name:nil error:&error]) { [self showError:error]; [self reloadUsers]; return; }
    int fd=self.store.reservationFD;
    if(fcntl(fd,F_SETFD,0)||setenv("MNG_USER_LOCK_FD",[@(fd).stringValue UTF8String],1)){
        close(fd);self.store.reservationFD=-1;[self showError:@"无法传递用户锁 / Cannot pass user reservation"];return;
    }
    self.store.reservationFD=-1; // descriptor intentionally survives picker destruction and exec
#else
    if (![self.store edit:@"select" user:self.selected name:nil error:&error]) { [self showError:error]; return; }
#endif
    setenv("MNG_USER_ID", [[NSString stringWithFormat:@"%lu",(unsigned long)self.selected] UTF8String], 1);
    self.accepted = YES; [NSApp stopModal]; [self.window orderOut:nil];
}
- (void)cancel:(id)sender { (void)sender; [NSApp abortModal]; [self.window orderOut:nil]; }
- (BOOL)windowShouldClose:(NSWindow *)sender { (void)sender; [self cancel:nil]; return NO; }
@end

static BOOL mng_choose_user(NSString *profile) {
    if (getenv("MNG_USER_ID")) return YES; // explicit headless/test choice is validated by runtime
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    MNGUserWindow *picker = [[MNGUserWindow alloc] initWithProfile:profile];
    if (![picker reloadUsers]) return NO;
    [picker.window center]; [picker.window makeKeyAndOrderFront:nil];
    for (MNGUserCard *card in picker.grid.subviews) if (card.userID == picker.selected) {
        [picker.window makeFirstResponder:card]; [card scrollRectToVisible:card.bounds]; break;
    }
#ifdef MNG_MULTI_INSTANCE_USERS
    NSTimer *timer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){
        (void)t;if(NSApp.modalWindow==picker.window)[picker refreshAvailability];
    }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSModalPanelRunLoopMode];
#endif
    [NSApp runModalForWindow:picker.window];
#ifdef MNG_MULTI_INSTANCE_USERS
    [timer invalidate];
#endif
    return picker.accepted;
}

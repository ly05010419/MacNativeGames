#define main command_q_probe_main
#include "../games/totk/tests/command_q/probe.m"
#undef main
#include <assert.h>
int main(int argc,char **argv){@autoreleasepool{
 assert(argc==2);NSString *folder=@(argv[1]);NSString *file=[folder stringByAppendingPathComponent:@"controls.txt"];setenv("TOTK_CONTROLS",file.fileSystemRepresentation,1);
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];[NSApp finishLaunching];
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,700) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];window.releasedWhenClosed=NO;window.contentView=[TOTKInputView new];
 front_install_menu();TOTKControlsEditor *editor=controlsEditor;NSError *error=nil;
 assert([editor loadLayout:&error]);assert(!editor.original&&editor->draft.keys[38]==1);
 NSString *initial=@"# user comment\nsouth = x tap\nmouse_right = zr aim mouse5\nkey_38 = b # custom key\n";
 assert([initial writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]);
 assert([editor loadLayout:&error]);assert(editor->draft.map[0]==4&&editor->draft.keys[38]==2);
 editor->draft.keys[38]=3;assert([editor saveLayout:&error]);
 NSString *saved=[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:&error];assert([saved containsString:@"# user comment"]&&[saved containsString:@"# custom key"]);
 assert([editor loadLayout:&error]&&editor->draft.keys[38]==3&&editor->draft.modes[18]==(CONTROLLER_AIM|21<<3));
 editor->draft.keys[38]=4;assert([initial writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]);assert(![editor saveLayout:&error]);
 assert([[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:NULL] isEqualToString:initial]);
 assert([@"key_18 = a" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]);assert(![editor loadLayout:&error]);
 assert([initial writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]);
 [editor open:nil];assert(editor.panel&&controlsOpen&&editor.keyTargets.count==128&&editor.padTargets.count==22);
 [editor reset:nil];assert(editor->draft.keys[38]==1&&editor->draft.map[0]==2);
 [editor.panel orderFront:nil];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];[editor.panel display];NSView *content=editor.panel.contentView;NSBitmapImageRep *bitmap=[content bitmapImageRepForCachingDisplayInRect:content.bounds];[content cacheDisplayInRect:content.bounds toBitmapImageRep:bitmap];
 [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[folder stringByAppendingPathComponent:@"editor.png"] atomically:YES];
 [editor cancel:nil];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 assert(!controlsOpen&&!editor.panel);assert([[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:NULL] isEqualToString:initial]);
 puts("PASS: AppKit controls editor, shared file, comments, atomic save, conflict, malformed file, 128 keyboard/22 controller rows, Reset/Cancel");return 0;
}}

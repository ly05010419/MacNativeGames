/* Reuse the existing platform stubs, never the user's catalog or profile. */
#define main settings_probe_main
#include "../settings_panel_probe.m"
#undef main
int main(void){@autoreleasepool{
 [NSApplication sharedApplication];
 NSImage *tile=[[NSImage alloc] initWithSize:NSMakeSize(64,64)];[tile lockFocus];[NSColor.greenColor setFill];NSRectFill(NSMakeRect(0,0,64,64));[tile unlockFocus];
 NSImage *rounded=front_dock_icon(tile);NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithData:rounded.TIFFRepresentation];
 assert([bitmap colorAtX:0 y:0].alphaComponent<0.01);assert([bitmap colorAtX:bitmap.pixelsWide/2 y:bitmap.pixelsHigh/2].alphaComponent>0.99);
 assert([cheat_display_name(@"№ 1a 测试") isEqualToString:@"测试"]);assert([cheat_display_name(@"♯19b 倍率") isEqualToString:@"倍率"]);

 TOTKCheatEntry *scheduled=[TOTKCheatEntry new];scheduled->applied=YES;scheduled->applyOnce=YES;
 assert(!cheat_entry_due(scheduled,1000000000,0));scheduled->applyOnce=NO;scheduled->nextRunNS=500000000;
 assert(!cheat_entry_due(scheduled,499000000,0));assert(cheat_entry_due(scheduled,500000000,0));scheduled->pure=YES;assert(!cheat_entry_due(scheduled,1000000000,0));
 scheduled->hotkeys=YES;assert(cheat_entry_due(scheduled,1000000000,1));
 NSString *dir=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 NSFileManager *fm=NSFileManager.defaultManager;[fm createDirectoryAtPath:[dir stringByAppendingPathComponent:@"extracted/v121/exefs"] withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *before=fm.currentDirectoryPath;[fm changeCurrentDirectoryPath:dir];
 NSString *bid=@"0000000000000000000000000000000000000001";
 [bid writeToFile:@"extracted/v121/exefs/main.build-id" atomically:YES encoding:NSUTF8StringEncoding error:nil];
 NSDictionary *catalog=@{@"build_id":bid,@"entries":@[
  @{@"id":@"a",@"name":@"№ 1a 测试倍率一",@"bulk":@YES,@"bulk_rank":@1,@"category":@"移动",@"group":@"speed",@"words":@[@"04000000",@"00000020",@"00000001"]},
  @{@"id":@"b",@"name":@"♯ 2b 测试倍率二",@"bulk":@YES,@"bulk_rank":@2,@"category":@"移动",@"group":@"speed",@"words":@[@"04000000",@"00000020",@"00000002"]},
  @{@"id":@"independent",@"name":@"独立功能",@"category":@"战斗",@"bulk":@YES,@"words":@[@"04000000",@"00000024",@"00000003"]},
  @{@"id":@"manual",@"name":@"单独操作",@"category":@"更多",@"bulk":@NO,@"words":@[@"04000000",@"00000028",@"00000004"]},
  @{@"id":@"c",@"name":@"缺少代码",@"category":@"更多"}],@"guards":@[]};
 NSData *json=[NSJSONSerialization dataWithJSONObject:catalog options:0 error:nil];[json writeToFile:@"catalog.json" atomically:YES];
 setenv("TOTK_CHEAT_CATALOG",[[dir stringByAppendingPathComponent:@"catalog.json"] fileSystemRepresentation],1);
 setenv("TOTK_PROFILE_DIR",[[dir stringByAppendingPathComponent:@"profile"] fileSystemRepresentation],1);
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,650) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
 TOTKCheatPanel *panel=[TOTKCheatPanel new];[panel show];
 NSTableView *table=[panel valueForKey:@"table"];NSArray *entries=[panel valueForKey:@"entries"];
 assert(table.numberOfRows==5);assert(((TOTKCheatEntry *)entries[0])->program.code);assert(!((TOTKCheatEntry *)entries[4])->program.code);
 NSView *checkbox=[table viewAtColumn:0 row:0 makeIfNecessary:YES];
 for(unsigned i=0;i<10;i++)[panel refreshStatus];assert([table viewAtColumn:0 row:0 makeIfNecessary:NO]==checkbox);
 NSButton *button=[NSButton new];button.tag=0;[panel toggle:button];assert(((TOTKCheatEntry *)entries[0])->selected);
 button.tag=2;[panel toggle:button];assert(((TOTKCheatEntry *)entries[0])->selected&&((TOTKCheatEntry *)entries[2])->selected);
 button.tag=1;[panel toggle:button];assert(!((TOTKCheatEntry *)entries[0])->selected&&((TOTKCheatEntry *)entries[1])->selected);
 assert(((TOTKCheatEntry *)entries[2])->selected);
 [panel disableAll:nil];for(TOTKCheatEntry *e in entries)assert(!e->selected);
 [panel enableAll:nil];assert(!((TOTKCheatEntry *)entries[0])->selected&&((TOTKCheatEntry *)entries[1])->selected&&((TOTKCheatEntry *)entries[2])->selected&&!((TOTKCheatEntry *)entries[3])->selected);
 assert([((TOTKCheatEntry *)entries[0]).record[@"name"] isEqualToString:@"测试倍率一"]);
 NSSearchField *search=[panel valueForKey:@"search"];search.stringValue=@"缺少";[panel filter:nil];assert(table.numberOfRows==1);
 NSButton *disabled=(NSButton *)[panel tableView:table viewForTableColumn:table.tableColumns[0] row:0];assert(!disabled.enabled);
 TOTKCheatPanel *restored=[TOTKCheatPanel new];NSArray *again=[restored valueForKey:@"entries"];assert(((TOTKCheatEntry *)again[1])->selected);
 [panel pause:nil];assert(![[panel valueForKey:@"master"] boolValue]);
 NSPanel *native=[panel valueForKey:@"panel"];[native performClose:nil];assert(!fakeSettingsCapture);
 [(NSTimer *)[panel valueForKey:@"timer"] invalidate];[(NSTimer *)[restored valueForKey:@"timer"] invalidate];
 [fm changeCurrentDirectoryPath:before];[fm removeItemAtPath:dir error:nil];puts("pass: native cheat panel filtering, disabled entries, exclusivity, persistence, capture");return 0;
}}

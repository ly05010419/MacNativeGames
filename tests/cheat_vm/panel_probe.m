/* Reuse the existing platform stubs, never the user's catalog or profile. */
#define main settings_probe_main
#include "../settings_panel_probe.m"
#undef main
int main(void){@autoreleasepool{
 [NSApplication sharedApplication];
 NSString *dir=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 NSFileManager *fm=NSFileManager.defaultManager;[fm createDirectoryAtPath:[dir stringByAppendingPathComponent:@"extracted/v121/exefs"] withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *before=fm.currentDirectoryPath;[fm changeCurrentDirectoryPath:dir];
 NSString *bid=@"0000000000000000000000000000000000000001";
 [bid writeToFile:@"extracted/v121/exefs/main.build-id" atomically:YES encoding:NSUTF8StringEncoding error:nil];
 NSDictionary *catalog=@{@"build_id":bid,@"entries":@[
  @{@"id":@"a",@"name":@"测试倍率一",@"category":@"移动",@"group":@"speed",@"words":@[@"04000000",@"00000020",@"00000001"]},
  @{@"id":@"b",@"name":@"测试倍率二",@"category":@"移动",@"group":@"speed",@"words":@[@"04000000",@"00000020",@"00000002"]},
  @{@"id":@"c",@"name":@"缺少代码",@"category":@"更多"}],@"guards":@[]};
 NSData *json=[NSJSONSerialization dataWithJSONObject:catalog options:0 error:nil];[json writeToFile:@"catalog.json" atomically:YES];
 setenv("TOTK_CHEAT_CATALOG",[[dir stringByAppendingPathComponent:@"catalog.json"] fileSystemRepresentation],1);
 setenv("TOTK_PROFILE_DIR",[[dir stringByAppendingPathComponent:@"profile"] fileSystemRepresentation],1);
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,650) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
 TOTKCheatPanel *panel=[TOTKCheatPanel new];[panel show];
 NSTableView *table=[panel valueForKey:@"table"];NSArray *entries=[panel valueForKey:@"entries"];
 assert(table.numberOfRows==3);assert(((TOTKCheatEntry *)entries[0])->program.code);assert(!((TOTKCheatEntry *)entries[2])->program.code);
 NSButton *button=[NSButton new];button.tag=0;[panel toggle:button];assert(((TOTKCheatEntry *)entries[0])->selected);
 button.tag=1;[panel toggle:button];assert(!((TOTKCheatEntry *)entries[0])->selected&&((TOTKCheatEntry *)entries[1])->selected);
 NSSearchField *search=[panel valueForKey:@"search"];search.stringValue=@"缺少";[panel filter:nil];assert(table.numberOfRows==1);
 NSButton *disabled=(NSButton *)[panel tableView:table viewForTableColumn:table.tableColumns[0] row:0];assert(!disabled.enabled);
 TOTKCheatPanel *restored=[TOTKCheatPanel new];NSArray *again=[restored valueForKey:@"entries"];assert(((TOTKCheatEntry *)again[1])->selected);
 [panel pause:nil];assert(![[panel valueForKey:@"master"] boolValue]);
 NSPanel *native=[panel valueForKey:@"panel"];[native performClose:nil];assert(!fakeSettingsCapture);
 [(NSTimer *)[panel valueForKey:@"timer"] invalidate];[(NSTimer *)[restored valueForKey:@"timer"] invalidate];
 [fm changeCurrentDirectoryPath:before];[fm removeItemAtPath:dir error:nil];puts("pass: native cheat panel filtering, disabled entries, exclusivity, persistence, capture");return 0;
}}

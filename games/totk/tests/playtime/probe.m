#import "../../runtime/playtime.h"
#include <assert.h>
int main(void){@autoreleasepool {
 setenv("TOTK_UI_LANG","en",1);
 NSString *dir=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *path=[dir stringByAppendingPathComponent:@"playtime.json"];
 [@"{\"seconds\":120,\"seed\":{\"seconds\":120}}" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
 TOTKPlaytime *p=[[TOTKPlaytime alloc] initWithPath:path];
 assert(p.seconds==120);assert([p.text isEqualToString:@"Playtime  0 h 02 m"]);
 [p tick:100 active:NO];[p tick:130 active:NO];assert(p.seconds==120);
 [p tick:131 active:YES];[p tick:132 active:YES];assert(p.seconds==121);
 [p tick:132.5 active:NO];assert(p.seconds==121.5);
 TOTKPlaytime *r=[[TOTKPlaytime alloc] initWithPath:path];assert(r.seconds==121.5);
 assert([r.record[@"seed"][@"seconds"] doubleValue]==120);
 [p tick:500 active:NO];[p tick:501 active:YES];[p tick:901 active:YES];assert(p.seconds==121.5);
 for(int i=1;i<=16;i++)[p tick:901+i active:YES];
 r=[[TOTKPlaytime alloc] initWithPath:path];assert(r.seconds==136.5);
 [p tick:917.25 active:NO];[p save];r=[[TOTKPlaytime alloc] initWithPath:path];assert(r.seconds==137.75);
 // Bad data must be preserved instead of silently replacing the lifetime total.
 [@"broken" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
 r=[[TOTKPlaytime alloc] initWithPath:path];assert(!r.writable);[r tick:1 active:YES];[r tick:2 active:YES];assert(![r save]);
 assert([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"broken"]);
 // Separate profiles start at zero and never seed from shared preferences.
 TOTKPlaytime *other=[[TOTKPlaytime alloc] initWithPath:[dir stringByAppendingPathComponent:@"other/playtime.json"]];
 assert(other.seconds==0);[other tick:1 active:YES];[other tick:2 active:NO];assert(other.seconds==1);
 [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
 puts("PASS: seed, foreground/pause, sleep gap, periodic checkpoint, exit checkpoint, reload, metadata, corrupt-file protection, profile isolation");
}}

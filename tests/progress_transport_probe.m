#import "../games/totk/runtime/progress_store.h"
#import "../games/totk/runtime/progress_transport.h"

static unsigned checks;
#define CHECK(c,note) do{checks++;if(!(c)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,note);exit(1);}}while(0)
static NSURL *child(NSURL *root,NSString *name){return [root URLByAppendingPathComponent:name];}
static NSArray *transfer(TOTKProgressTransport *transport,NSURL *cloud,NSArray **problems) {
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSArray *files,*errors;
    [transport exchangeWithFolder:cloud completion:^(NSArray *incoming,NSArray *issues){files=[incoming copy];errors=[issues copy];dispatch_semaphore_signal(done);}];
    CHECK(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0,"transport completes");
    if(problems)*problems=errors;return files;
}
static void json(id object,NSURL *url){CHECK([[NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:nil] writeToURL:url atomically:YES],"fixture json");}
int main(void){@autoreleasepool{
    NSURL *root=child([NSURL fileURLWithPath:NSTemporaryDirectory()],NSUUID.UUID.UUIDString);
    NSURL *profile=child(root,@"profile"),*cloud=child(root,@"cloud");NSError *error=nil;
    NSURL *file=child(profile,@"save/00000000000000020000000000000001/slot/progress.sav");
    CHECK([NSFileManager.defaultManager createDirectoryAtURL:file.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error],"fixture directory");
    uint64_t uid[]={1,2};CHECK([[NSData dataWithBytes:uid length:16] writeToURL:child(profile,@"account.uid") atomically:YES],"identity");
    CHECK([@"ISOLATED TRANSPORT FIXTURE" writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil],"fake save");
    TOTKProgressStore *a=[[TOTKProgressStore alloc] initWithRoot:child(root,@"a") device:@"A" gameVersion:@"1.4.3" error:&error];
    TOTKProgressStore *b=[[TOTKProgressStore alloc] initWithRoot:child(root,@"b") device:@"B" gameVersion:@"1.4.3" error:&error];
    CHECK(a&&b,"stores");NSString *version=[a captureProfile:profile error:&error];CHECK(version!=nil,"capture");
    TOTKProgressTransport *ta=[[TOTKProgressTransport alloc] initWithLocalRoot:a.localRoot],*tb=[[TOTKProgressTransport alloc] initWithLocalRoot:b.localRoot];NSArray *problems=nil;
    CHECK(transfer(ta,cloud,&problems).count==0&&problems.count==0,"publish to isolated file-provider folder");
    NSArray *incoming=transfer(tb,cloud,&problems);CHECK(incoming.count==1&&problems.count==0,"download stages but does not apply");
    CHECK(b.versions.count==0&&b.workingProfile==nil,"transport cannot mutate selected progress");
    CHECK([b ingestRecordAtURL:incoming.firstObject error:&error]&&[b acknowledgeDownloads:&error],"validate on local store queue");
    CHECK([b checkout:version resolving:NO error:&error],"complete version applied before game");
    CHECK(transfer(tb,cloud,&problems).count==0&&problems.count==0,"receipt upload");
    incoming=transfer(ta,cloud,&problems);CHECK(incoming.count==1,"sender receives receiver receipt");
    CHECK([a ingestRecordAtURL:incoming.firstObject error:&error]&&a.receipts.count==1,"receipt indexed");
    CHECK(transfer(ta,cloud,&problems).count==0&&problems.count==0,"unchanged exchange quiet");
    NSURL *remote=child(child(cloud,@"totk-progress-v1"),[version stringByAppendingString:@".json"]);
    NSMutableDictionary *changed=[[a versionRecord:version error:&error] mutableCopy];changed[@"deviceName"]=@"unexpected mutation";json(changed,remote);
    CHECK([NSFileManager.defaultManager setAttributes:@{NSFileModificationDate:[NSDate dateWithTimeIntervalSinceNow:10]} ofItemAtPath:remote.path error:&error],"force changed metadata");
    transfer(ta,cloud,&problems);CHECK(problems.count>0&&[a.versions[version][@"device"] isEqual:@"A"],"immutable collision never replaces local record");
    json([a versionRecord:version error:&error],remote);
    NSString *lateID=NSUUID.UUID.UUIDString.lowercaseString,*parentID=NSUUID.UUID.UUIDString.lowercaseString;
    NSURL *late=child(child(cloud,@"totk-progress-v1"),[lateID stringByAppendingString:@".json"]);
    json(@[@"bad shape"],late);
    incoming=transfer(tb,cloud,&problems);NSURL *staged=nil;for(NSURL *url in incoming)if([url.lastPathComponent.stringByDeletingPathExtension isEqual:lateID])staged=url;
    CHECK(staged&&![b ingestRecordAtURL:staged error:&error],"bad new payload stays pending");
    incoming=transfer(tb,cloud,&problems);CHECK(incoming.count>0,"unaccepted inbox is retried even if remote metadata unchanged");
    NSMutableDictionary *lateRecord=[[a versionRecord:version error:&error] mutableCopy];lateRecord[@"id"]=lateID;lateRecord[@"parents"]=@[parentID];json(lateRecord,late);
    incoming=transfer(tb,cloud,&problems);for(NSURL *url in incoming)[b ingestRecordAtURL:url error:&error];
    CHECK(b.pendingProblems.count>0&&![b checkout:lateID resolving:NO error:&error],"child before parent cannot be applied");
    NSMutableDictionary *parent=[lateRecord mutableCopy];parent[@"id"]=parentID;parent[@"parents"]=@[];
    json(parent,child(child(cloud,@"totk-progress-v1"),[parentID stringByAppendingString:@".json"]));
    incoming=transfer(tb,cloud,&problems);for(NSURL *url in incoming)[b ingestRecordAtURL:url error:&error];
    CHECK([b checkout:lateID resolving:NO error:&error],"late parent resolves lineage without data loss");
    CHECK(a.versions[version]!=nil,"sender original version retained");
    a=nil;b=nil;ta=nil;tb=nil;
    CHECK([NSFileManager.defaultManager removeItemAtURL:root error:&error],"remove disposable transfer fixtures only");
    printf("PASS: %u asynchronous transport checks (local fixture folder, no actual iCloud)\n",checks);
}}

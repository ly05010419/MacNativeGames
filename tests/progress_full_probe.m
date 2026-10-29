#import "../games/totk/runtime/progress_store.h"
#import "../games/totk/runtime/progress_transport.h"
#include <errno.h>
static NSUInteger checks;
#define CHECK(c,n) do{checks++;if(!(c)){fprintf(stderr,"FAIL: %s\n",n);exit(1);}}while(0)
static NSURL *at(NSURL *root,NSString *path){return [root URLByAppendingPathComponent:path];}
static NSString *const Save=@"save/00000000000000020000000000000001/slot/progress.sav";
static BOOL noSpace(NSError *error){if(!error)return NO;return ([error.domain isEqual:NSCocoaErrorDomain]&&error.code==NSFileWriteOutOfSpaceError)||([error.domain isEqual:NSPOSIXErrorDomain]&&error.code==ENOSPC)||noSpace(error.userInfo[NSUnderlyingErrorKey]);}
int main(int argc,const char **argv){@autoreleasepool{
    CHECK(argc==4,"phase and private fixture directories");BOOL prepare=[@(argv[1]) isEqual:@"prepare"];
    CHECK(!noSpace(nil)&&!noSpace([NSError errorWithDomain:NSPOSIXErrorDomain code:EACCES userInfo:nil]),"non-space errors are not misclassified");
    NSURL *volume=[NSURL fileURLWithPath:@(argv[2])],*outside=[NSURL fileURLWithPath:@(argv[3])],*profile=at(outside,@"profile");NSError *error=nil;
    if(prepare){
        CHECK([NSFileManager.defaultManager createDirectoryAtURL:at(profile,Save).URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error],"fixture directories");
        NSData *data=[NSMutableData dataWithLength:1024*1024];uint64_t uid[]={1,2};
        CHECK([data writeToURL:at(profile,Save) atomically:YES]&&[data writeToURL:at(outside,@"baseline.bin") atomically:YES],"synthetic baseline");
        CHECK([[NSData dataWithBytes:uid length:16] writeToURL:at(profile,@"account.uid") atomically:YES],"fake identity");
    }
    TOTKProgressStore *store=[[TOTKProgressStore alloc] initWithRoot:at(volume,@"local") device:@"local" gameVersion:@"1.4.3" error:&error];
    TOTKProgressStore *sender=[[TOTKProgressStore alloc] initWithRoot:at(outside,@"sender") device:@"sender" gameVersion:@"1.4.3" error:&error];CHECK(store&&sender,"local and sender stores");
    NSURL *cloud=at(volume,@"cloud");
    if(prepare){
        NSString *version=[store captureProfile:profile error:&error];CHECK(version&&[store checkout:version resolving:NO error:&error],"initial complete mounted version");
        CHECK([sender captureProfile:profile error:&error]!=nil,"sender retained outside full volume");
        CHECK([NSFileManager.defaultManager createDirectoryAtURL:at(cloud,@"totk-progress-v1") withIntermediateDirectories:YES attributes:nil error:&error],"cloud fixture directory ready");
        return 0;
    }
    NSString *old=store.baseVersion;NSURL *work=store.workingProfile;
    NSData *state=[NSData dataWithContentsOfURL:at(store.localRoot,@"state.json")];
    CHECK([[NSMutableData dataWithLength:2*1024*1024] writeToURL:at(profile,Save) atomically:YES],"new synthetic contents outside full volume");
    error=nil;CHECK(![store captureProfile:profile error:&error]&&noSpace(error),"capture reports actual ENOSPC");
    CHECK([store.baseVersion isEqual:old]&&[[NSData dataWithContentsOfURL:at(store.localRoot,@"state.json")] isEqual:state],"full disk does not switch main or persistent state");
    CHECK([[NSData dataWithContentsOfURL:at(work,Save)] isEqual:[NSData dataWithContentsOfURL:at(outside,@"baseline.bin")]],"old complete work remains byte-identical");
    error=nil;CHECK(![store checkout:old resolving:NO error:&error]&&noSpace(error),"full disk rejects partial new generation");
    CHECK([store.workingProfile isEqual:work]&&[[NSData dataWithContentsOfURL:at(store.localRoot,@"state.json")] isEqual:state],"failed application leaves whole old generation selected");
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSArray *problems;
    TOTKProgressTransport *transport=[[TOTKProgressTransport alloc] initWithLocalRoot:sender.localRoot];
    [transport exchangeWithFolder:cloud completion:^(NSArray *incoming,NSArray *issues){(void)incoming;problems=issues;dispatch_semaphore_signal(done);}];
    CHECK(!dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC)),"full remote transport returns an error");
    CHECK(problems.count>0,"full remote does not report upload success");
    CHECK([sender versionRecord:sender.baseVersion error:&error]!=nil,"sender complete version preserved for retry");
    printf("PASS: %lu full-volume checks (actual bounded filesystem ENOSPC, synthetic data)\n",(unsigned long)checks);
}}

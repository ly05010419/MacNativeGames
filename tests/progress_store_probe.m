#import "../games/totk/runtime/progress_store.h"
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(value, note) do { checks++; if (!(value)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,note); exit(1); } } while (0)
static NSURL *child(NSURL *root, NSString *path) { return [root URLByAppendingPathComponent:path]; }
static void put(NSURL *url, NSData *data) {
    NSError *error = nil;
    CHECK([NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error],"fixture directory");
    CHECK([data writeToURL:url options:NSDataWritingAtomic error:&error],"fixture write");
}
static NSString *const SavePath = @"save/00000000000000020000000000000001/slot_00/progress.sav";
static NSURL *fixture(NSURL *root, NSString *name, NSString *value) {
    NSURL *url = child(root,name); uint64_t uid[] = {1,2};
    put(child(url,@"account.uid"),[NSData dataWithBytes:uid length:sizeof uid]);
    put(child(url,SavePath),[value dataUsingEncoding:NSUTF8StringEncoding]);
    put(child(url,@"video.txt"),[@"local display settings" dataUsingEncoding:NSUTF8StringEncoding]);
    put(child(url,@"cache/do-not-upload"),[@"cache" dataUsingEncoding:NSUTF8StringEncoding]);
    return url;
}
static TOTKProgressStore *store(NSURL *root, NSString *device) NS_RETURNS_RETAINED;
static TOTKProgressStore *store(NSURL *root, NSString *device) {
    NSError *error = nil;
    TOTKProgressStore *s = [[TOTKProgressStore alloc] initWithRoot:root device:device gameVersion:@"1.4.3" error:&error];
    if (!s) fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);
    CHECK(s != nil,"store opens"); return s;
}
static NSString *text(NSURL *url) { return [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil]; }
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 4 && !strcmp(argv[1],"--crash-checkout")) {
            TOTKProgressStore *s = store([NSURL fileURLWithPath:@(argv[2])],@"Crash child");
            [s checkout:@(argv[3]) resolving:NO error:nil]; return 2;
        }
        NSURL *root = child([NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES],NSUUID.UUID.UUIDString);
        NSURL *cloud = child(root,@"cloud"); NSError *error = nil;
        NSURL *original = fixture(root,@"original",@"start");
        TOTKProgressStore *a, *b, *c, *incompatible;
        NSURL *aRoot, *lastWork; NSString *lastBase, *v0;
        __weak TOTKProgressStore *released;
        @autoreleasepool {
        a = store(child(root,@"a"),@"Mac");
        b = store(child(root,@"b"),@"iPad");
        c = store(child(root,@"c"),@"iPhone");
        v0 = [a captureProfile:original error:&error];
        CHECK(v0 != nil,"capture real profile shape");
        CHECK([[a versionRecord:v0 error:&error][@"files"] count] == 2,"identity and save only; no settings/cache");
        CHECK([a versionRecord:v0 error:&error][@"files"][SavePath] != nil,"canonical /var aliases preserve exact save path");
        CHECK(a.versions[v0][@"files"] == nil,"history index does not retain all payloads in memory");
        CHECK([[a captureProfile:original error:&error] isEqual:v0],"unchanged capture does not create duplicate");
        CHECK([a checkout:v0 resolving:NO error:&error],"generation checkout");
        CHECK(![a.workingProfile.path isEqual:original.path],"original stays separate");
        CHECK([text(child(original,SavePath)) isEqual:@"start"],"original intact");
        CHECK([a exchangeWithFolder:cloud error:&error],"initial upload");
        CHECK([b exchangeWithFolder:cloud error:&error],"iPad download");
        CHECK([c exchangeWithFolder:cloud error:&error],"iPhone download");
        CHECK([a exchangeWithFolder:cloud error:&error] && a.receipts.count >= 2,"remote receipt only after verified download");
        CHECK([b checkout:v0 resolving:NO error:&error] && [c checkout:v0 resolving:NO error:&error],"other devices checkout");
        put(child(b.workingProfile,SavePath),[@"iPad offline branch" dataUsingEncoding:NSUTF8StringEncoding]);
        put(child(c.workingProfile,SavePath),[@"iPhone offline branch" dataUsingEncoding:NSUTF8StringEncoding]);
        NSString *vb = [b captureProfile:b.workingProfile error:&error];
        NSString *vc = [c captureProfile:c.workingProfile error:&error];
        CHECK(vb && vc && ![vb isEqual:vc],"offline independent versions");
        CHECK([b exchangeWithFolder:cloud error:&error] && [c exchangeWithFolder:cloud error:&error] && [a exchangeWithFolder:cloud error:&error],"offline replay union");
        CHECK(a.heads.count == 2 && [a.heads containsObject:vb] && [a.heads containsObject:vc],"both branch heads retained");
        CHECK([a isDescendant:vb of:v0] && ![a isDescendant:vb of:vc],"parent ancestry not timestamps");
        CHECK([a checkout:vb resolving:YES error:&error],"resolve to iPad");
        NSString *choice = a.baseVersion;
        CHECK(a.heads.count == 1 && [a isDescendant:choice of:vb] && [a isDescendant:choice of:vc],"choice records both parents");
        CHECK([text(child(a.workingProfile,SavePath)) isEqual:@"iPad offline branch"],"chosen payload");
        CHECK(a.versions[vc] != nil,"unselected branch retained");
        NSString *backup = [a nameBackup:@"最终战之前" forVersion:choice replacing:nil error:&error];
        CHECK(backup && a.backups.count == 1,"named backup");
        CHECK([a nameBackup:@"新的名字" forVersion:choice replacing:backup error:&error] != nil && a.backups.count == 1,"rename without deleting snapshot");
        NSString *restored = [a restoreVersion:v0 error:&error];
        CHECK(restored && [a isDescendant:restored of:choice],"restore is a new successor event");
        CHECK([text(child(a.workingProfile,SavePath)) isEqual:@"start"],"restored old contents");
        CHECK([a restoreVersion:choice error:&error] != nil,"undo restore");
        CHECK([text(child(a.workingProfile,SavePath)) isEqual:@"iPad offline branch"],"pre-restore contents recoverable");
        CHECK([a exchangeWithFolder:cloud error:&error] && [b exchangeWithFolder:cloud error:&error],"choices/restores/backup propagate");
        CHECK(b.backups.count == 1 && b.heads.count == 1,"remote graph converges");
        NSUInteger recordCount = a.versions.count;
        NSURL *badURL = child(child(cloud,@"totk-progress-v1"),[@"00000000-0000-0000-0000-000000000001" stringByAppendingString:@".json"]);
        put(badURL,[@"[\"not an object\"]" dataUsingEncoding:NSUTF8StringEncoding]);
        CHECK([a exchangeWithFolder:cloud error:&error] && a.pendingProblems.count > 0 && a.versions.count == recordCount,"malformed record pending, history retained");
        NSMutableDictionary *bad = [[a versionRecord:v0 error:&error] mutableCopy];
        bad[@"id"] = @"00000000-0000-0000-0000-000000000001";
        NSMutableDictionary *files = [bad[@"files"] mutableCopy];
        NSMutableDictionary *entry = [files[SavePath] mutableCopy]; entry[@"data"] = @"dGFtcGVyZWQ=";
        files[SavePath] = entry; bad[@"files"] = files;
        put(badURL,[NSJSONSerialization dataWithJSONObject:bad options:0 error:nil]);
        CHECK([a exchangeWithFolder:cloud error:&error] && a.pendingProblems.count > 0 && a.versions.count == recordCount,"tamper rejected");
        files = [[a versionRecord:v0 error:&error][@"files"] mutableCopy]; files[@"../../escape"] = files[SavePath]; bad[@"files"] = files;
        put(badURL,[NSJSONSerialization dataWithJSONObject:bad options:0 error:nil]);
        CHECK([a exchangeWithFolder:cloud error:&error] && a.versions.count == recordCount,"path escape rejected");
        CHECK(![NSFileManager.defaultManager fileExistsAtPath:child(root,@"escape").path],"no escaped write");
        files = [[a versionRecord:v0 error:&error][@"files"] mutableCopy];
        files[[SavePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"PROGRESS.SAV"]]=files[SavePath];bad[@"files"]=files;
        put(badURL,[NSJSONSerialization dataWithJSONObject:bad options:0 error:nil]);error=nil;
        CHECK(![a ingestRecordAtURL:badURL error:&error]&&[error.localizedDescription containsString:@"重复"],"case-colliding paths rejected before materialization");
        bad=[ [a versionRecord:v0 error:nil] mutableCopy];bad[@"id"]=@"00000000-0000-0000-0000-000000000001";bad[@"kind"]=@"choice";bad[@"selected"]=@"bad-id";
        put(badURL,[NSJSONSerialization dataWithJSONObject:bad options:0 error:nil]);error=nil;
        CHECK(![a ingestRecordAtURL:badURL error:&error]&&[error.localizedDescription containsString:@"标识"],"invalid choice target rejected");
        NSURL *linked = fixture(root,@"linked",@"safe");
        [NSFileManager.defaultManager removeItemAtURL:child(linked,SavePath) error:nil];
        CHECK(symlink(child(original,SavePath).fileSystemRepresentation,child(linked,SavePath).fileSystemRepresentation) == 0,"symlink fixture");
        CHECK(![a captureProfile:linked error:&error],"symlink capture refused");
        incompatible = [[TOTKProgressStore alloc] initWithRoot:child(root,@"old") device:@"Old" gameVersion:@"1.0.0" error:&error];
        CHECK(incompatible && [incompatible exchangeWithFolder:cloud error:&error] && ![incompatible checkout:v0 resolving:NO error:&error],"incompatible version never applied");
        CHECK(incompatible.workingProfile == nil,"failed application does not select a partial generation");
        released = a;
        aRoot = a.localRoot; lastBase = a.baseVersion; lastWork = a.workingProfile;
        a = nil;
        }
        CHECK(released == nil,"store lifetime releases lock");
        a = store(aRoot,@"Mac");
        CHECK([a.baseVersion isEqual:lastBase] && [a.workingProfile isEqual:lastWork],"atomic state survives reopen");
        for (NSString *point in @[@"before-state",@"after-state"]) {
            NSString *previous = a.baseVersion;
            a = nil;
            NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@(argv[0])];
            task.arguments = @[@"--crash-checkout",aRoot.path,v0];
            NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy]; env[@"TOTK_PROGRESS_TEST_FAIL"] = point; task.environment = env;
            CHECK([task launchAndReturnError:&error],"launch crash fixture child"); [task waitUntilExit];
            CHECK(task.terminationStatus == 77,"crash occurred at requested transaction boundary");
            a = store(aRoot,@"Mac");
            CHECK([a.baseVersion isEqual:[point isEqual:@"before-state"] ? previous : v0],"crash leaves complete old or new selection");
            CHECK([text(child(a.workingProfile,SavePath)) isEqual:[point isEqual:@"before-state"] ? @"iPad offline branch" : @"start"],"crash recovery contents match selected version");
        }
        CHECK([text(child(original,SavePath)) isEqual:@"start"] && [text(child(original,@"video.txt")) isEqual:@"local display settings"],"all original files preserved");
        @autoreleasepool {
            NSString *cachePath=@"save/00000000000000020000000000000001/storage/CacheStorageKey.dat";
            NSURL *profile=fixture(root,@"cache-marker-profile",@"saved progress");
            put(child(profile,cachePath),[@"first boot" dataUsingEncoding:NSUTF8StringEncoding]);
            TOTKProgressStore *cacheStore=store(child(root,@"cache-marker-store"),@"Cache fixture");
            NSString *first=[cacheStore captureProfile:profile error:&error];
            CHECK(first&&[cacheStore checkout:first resolving:NO error:&error],"cache marker fixture checkout");
            put(child(cacheStore.workingProfile,cachePath),[@"next boot" dataUsingEncoding:NSUTF8StringEncoding]);
            CHECK([[cacheStore captureProfile:cacheStore.workingProfile error:&error] isEqual:first]&&cacheStore.versions.count==1,"boot cache marker alone creates no player history or fork");
            CHECK([cacheStore workingProfileMatchesBase:&error],"boot cache marker alone does not trigger damaged-save recovery");
            put(child(cacheStore.workingProfile,SavePath),[@"new player progress" dataUsingEncoding:NSUTF8StringEncoding]);
            NSString *next=[cacheStore captureProfile:cacheStore.workingProfile error:&error];
            CHECK(next&&![next isEqual:first],"actual player progress still creates a new version");
            NSDictionary *record=[cacheStore versionRecord:next error:&error];
            NSData *marker=[[NSData alloc] initWithBase64EncodedString:record[@"files"][cachePath][@"data"] options:0];
            CHECK([[[NSString alloc] initWithData:marker encoding:NSUTF8StringEncoding] isEqual:@"next boot"],"complete new snapshot retains cache marker bytes");
            CHECK([cacheStore checkout:first resolving:NO error:&error]&&[text(child(cacheStore.workingProfile,cachePath)) isEqual:@"first boot"],"old snapshots remain complete and exactly restorable");
        }
        a = nil; b = nil; c = nil; incompatible = nil;
        CHECK([NSFileManager.defaultManager removeItemAtURL:root error:&error],"delete only disposable fixtures");
        printf("PASS: %u progress checks (isolated fixtures, no game or actual cloud)\n",checks);
    }
}

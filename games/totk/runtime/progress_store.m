#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"
#import "progress_store.h"
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

static const NSUInteger MaxSnapshot = 256u * 1024u * 1024u;
static NSString *const GameID = @"0100f2c0115b6000";
static BOOL fail(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"TOTKProgress" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
    return NO;
}
static NSString *uuid(void) { return NSUUID.UUID.UUIDString.lowercaseString; }
static BOOL validID(id value) {
    return [value isKindOfClass:NSString.class] && [[NSUUID alloc] initWithUUIDString:value] && [value isEqual:[value lowercaseString]];
}
static NSString *digest(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
#pragma clang diagnostic pop
    NSMutableString *result = [NSMutableString new];
    for (NSUInteger i = 0; i < sizeof bytes; i++) [result appendFormat:@"%02x", bytes[i]];
    return result;
}
static BOOL makeDir(NSURL *url, NSError **error) {
    return [NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:error];
}
static NSData *jsonData(id object, NSError **error) {
    return [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:error];
}
static id readJSONUnpooled(NSURL *url, NSError **error) {
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:url.path error:error];
    if (!attributes || [attributes[NSFileSize] unsignedLongLongValue] > MaxSnapshot * 2ull ||
        ![attributes[NSFileType] isEqual:NSFileTypeRegular]) { fail(error,@(TOTK_UI("进度记录不是可用的普通文件或过大。", "The progress record is not a usable regular file or is too large."))); return nil; }
    NSData *data = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
    return data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:error] : nil;
}
static id readJSON(NSURL *url, NSError **error) {
    id result=nil;NSError *problem=nil;
    @autoreleasepool {result=readJSONUnpooled(url,&problem);}
    if(error&&problem)*error=problem;return result;
}
static BOOL writeJSONUnpooled(id object, NSURL *url, NSError **error) {
    NSData *data = jsonData(object, error);
    if (!data || ![data writeToURL:url options:NSDataWritingAtomic error:error]) return NO;
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail(error,@(TOTK_UI("不能确认本地进度已落盘。", "Could not confirm that local progress was durably saved.")));
    int result = fsync(fd); close(fd);
    return result == 0 || fail(error,@(TOTK_UI("不能确认本地进度已落盘。", "Could not confirm that local progress was durably saved.")));
}
static BOOL writeJSON(id object, NSURL *url, NSError **error) {
    BOOL result=NO;NSError *problem=nil;
    @autoreleasepool {result=writeJSONUnpooled(object,url,&problem);}
    if(error&&problem)*error=problem;return result;
}
static BOOL safeRelative(id path) {
    if (![path isKindOfClass:NSString.class] || ![path length] || [path hasPrefix:@"/"] || [path containsString:@"\\"] ||
        [path rangeOfCharacterFromSet:[NSCharacterSet characterSetWithRange:NSMakeRange(0,1)]].location != NSNotFound) return NO;
    for (NSString *part in [path componentsSeparatedByString:@"/"])
        if (!part.length || [part isEqual:@"."] || [part isEqual:@".."]) return NO;
    return YES;
}
static NSString *identity(NSData *data) {
    if (data.length != 16) return nil;
    uint64_t words[2]; memcpy(words,data.bytes,16);
    if (!words[0] && !words[1]) return nil;
    return [NSString stringWithFormat:@"%016llx%016llx",(unsigned long long)words[1],(unsigned long long)words[0]];
}
static NSDictionary *fileEntry(NSData *data) {
    return @{@"size":@(data.length),@"sha256":digest(data),@"data":[data base64EncodedStringWithOptions:0]};
}
static NSString *treeDigest(NSDictionary *files) {
    NSMutableData *material = [NSMutableData new];
    for (NSString *path in [[files allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        [material appendData:[[NSString stringWithFormat:@"%@\n%@\n%@\n",path,files[path][@"size"],files[path][@"sha256"]] dataUsingEncoding:NSUTF8StringEncoding]];
    }
    return digest(material);
}
static NSString *progressDigest(NSDictionary *files) {
    NSMutableDictionary *progress=[files mutableCopy];
    NSData *account=[[NSData alloc] initWithBase64EncodedString:files[@"account.uid"][@"data"] options:0];
    NSString *uid=identity(account);
    // This cache marker changes on boot alone; retain it in snapshots without treating it as player progress.
    if(uid)[progress removeObjectForKey:[NSString stringWithFormat:@"save/%@/storage/CacheStorageKey.dat",uid]];
    return treeDigest(progress);
}
static NSDictionary *metadata(NSDictionary *record) {
    NSMutableDictionary *result = [record mutableCopy];
    NSDictionary *files = result[@"files"];
    if (files) {
        NSUInteger bytes = 0; for (NSDictionary *entry in files.allValues) bytes += [entry[@"size"] unsignedIntegerValue];
        result[@"bytes"] = @(bytes); result[@"fileCount"] = @(files.count);
        result[@"progressTree"] = progressDigest(files);
        [result removeObjectForKey:@"files"];
    }
    return result;
}
/* Bound and validate the entire envelope before decoding files into a working directory. */
static BOOL validateRecord(id object, NSError **error) {
    if (![object isKindOfClass:NSDictionary.class]) return fail(error,@(TOTK_UI("无效进度记录。", "Invalid progress record.")));
    NSDictionary *r = object;
    if (![r[@"schema"] isEqual:@1] || ![r[@"game"] isEqual:GameID] || !validID(r[@"id"]) ||
        ![r[@"device"] isKindOfClass:NSString.class] || [r[@"device"] length] > 128 || ![r[@"created"] isKindOfClass:NSNumber.class] ||
        (r[@"deviceName"] && (![r[@"deviceName"] isKindOfClass:NSString.class] || [r[@"deviceName"] length] > 256)))
        return fail(error,@(TOTK_UI("进度格式、游戏或标识不匹配。", "The progress format, game or identifier does not match.")));
    if ([r[@"kind"] isEqual:@"backup"]) {
        return (validID(r[@"target"]) && [r[@"name"] isKindOfClass:NSString.class] && [r[@"name"] length] > 0 &&
                [r[@"name"] length] <= 120 && (!r[@"replaces"] || validID(r[@"replaces"]))) || fail(error,@(TOTK_UI("无效命名备份。", "Invalid named backup.")));
    }
    if ([r[@"kind"] isEqual:@"receipt"]) return validID(r[@"target"]) || fail(error,@(TOTK_UI("无效接收记录。", "Invalid receipt.")));
    if (![@[@"save",@"choice",@"restore"] containsObject:r[@"kind"]] || ![r[@"version"] isKindOfClass:NSString.class] ||
        ![r[@"parents"] isKindOfClass:NSArray.class] || [r[@"parents"] count] > 128)
        return fail(error,@(TOTK_UI("无效版本关系。", "Invalid version ancestry.")));
    NSMutableSet *parents = [NSMutableSet new];
    for (id parent in r[@"parents"]) {
        if (!validID(parent) || [parent isEqual:r[@"id"]] || [parents containsObject:parent]) return fail(error,@(TOTK_UI("版本关系重复或循环。", "Version ancestry contains duplicates or a cycle.")));
        [parents addObject:parent];
    }
    if ((r[@"selected"]&&!validID(r[@"selected"]))||([@[@"choice",@"restore"] containsObject:r[@"kind"]]&&!validID(r[@"selected"]))) return fail(error,@(TOTK_UI("所选版本标识无效。", "The selected version identifier is invalid.")));
    NSDictionary *files = r[@"files"];
    if (![files isKindOfClass:NSDictionary.class] || files.count < 2 || files.count > 10000) return fail(error,@(TOTK_UI("存档清单不完整。", "The save manifest is incomplete.")));
    NSDictionary *account = files[@"account.uid"];
    if (![account isKindOfClass:NSDictionary.class] || ![account[@"data"] isKindOfClass:NSString.class]) return fail(error,@(TOTK_UI("缺少账号身份。", "Account identity is missing.")));
    NSString *uid = identity([[NSData alloc] initWithBase64EncodedString:account[@"data"] options:0]);
    if (!uid) return fail(error,@(TOTK_UI("账号身份损坏。", "Account identity is damaged.")));
    NSString *prefix = [@"save/" stringByAppendingFormat:@"%@/",uid];
    NSUInteger total = 0;
    NSMutableSet *paths=[NSMutableSet new];
    for (NSString *path in files) {
        NSDictionary *entry = files[path];
        if (!safeRelative(path) || (![path isEqual:@"account.uid"] && ![path hasPrefix:prefix]) ||
            ![entry isKindOfClass:NSDictionary.class] || ![entry[@"size"] isKindOfClass:NSNumber.class] ||
            ![entry[@"data"] isKindOfClass:NSString.class] || ![entry[@"sha256"] isKindOfClass:NSString.class])
            return fail(error,@(TOTK_UI("存档清单包含不允许的路径或数据。", "The save manifest contains a disallowed path or data.")));
        NSString *folded=path.precomposedStringWithCanonicalMapping.lowercaseString;
        if([paths containsObject:folded])return fail(error,@(TOTK_UI("存档含重复或大小写冲突的路径。", "The save contains duplicate or case-conflicting paths.")));
        [paths addObject:folded];
        unsigned long long size = [entry[@"size"] unsignedLongLongValue];
        if (size > MaxSnapshot || total > MaxSnapshot - size || [entry[@"data"] length] > MaxSnapshot * 2ull) return fail(error,@(TOTK_UI("存档超过安全大小限制。", "The save exceeds the safe size limit.")));
        total += (NSUInteger)size;
        NSData *data = [[NSData alloc] initWithBase64EncodedString:entry[@"data"] options:0];
        if (!data || data.length != size || ![digest(data) isEqual:entry[@"sha256"]]) return fail(error,@(TOTK_UI("存档内容校验失败，原进度未修改。", "Save content verification failed. The original progress is unchanged.")));
    }
    return [treeDigest(files) isEqual:r[@"tree"]] || fail(error,@(TOTK_UI("完整存档校验失败。", "Complete save verification failed.")));
}

@implementation TOTKProgressStore {
    NSMutableDictionary<NSString *,NSDictionary *> *_records;
    NSMutableDictionary *_state;
    NSMutableArray<NSString *> *_problems;
    NSMutableDictionary<NSString *, NSNumber *> *_completeCache;
    int _lockFD;
}
- (instancetype)initWithRoot:(NSURL *)root device:(NSString *)device gameVersion:(NSString *)version error:(NSError **)error {
    if (!(self = [super init])) return nil;
    _lockFD = -1;
    _localRoot = root; _deviceID = [device copy]; _deviceName = [device copy]; _gameVersion = [version copy];
    _records = [NSMutableDictionary new]; _problems = [NSMutableArray new]; _completeCache = [NSMutableDictionary new];
    if (!root.isFileURL || !device.length || !version.length || !makeDir(root,error)) return nil;
    _lockFD = open([[root URLByAppendingPathComponent:@"store.lock"] fileSystemRepresentation],O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if (_lockFD < 0 || flock(_lockFD,LOCK_EX|LOCK_NB)) { fail(error,@(TOTK_UI("另一个进程正在使用此进度区。", "Another process is using this progress store."))); return nil; }
    for (NSString *name in @[@"records",@"work"])
        if (!makeDir([root URLByAppendingPathComponent:name isDirectory:YES],error)) return nil;
    NSURL *stateURL = [root URLByAppendingPathComponent:@"state.json"];
    if ([NSFileManager.defaultManager fileExistsAtPath:stateURL.path]) {
        id state = readJSON(stateURL,error);
        if (![state isKindOfClass:NSDictionary.class] || ![state[@"schema"] isEqual:@1] ||
            (state[@"base"] && !validID(state[@"base"])) || (state[@"work"] && !validID(state[@"work"]))) {
            fail(error,@(TOTK_UI("本地进度索引损坏，保留所有存档，需恢复。", "The local progress index is damaged. All saves are retained; recovery is required."))); return nil;
        }
        _state = [state mutableCopy];
    } else _state = [@{@"schema":@1} mutableCopy];
    NSURL *records = [root URLByAppendingPathComponent:@"records"];
    NSArray *urls = [NSFileManager.defaultManager contentsOfDirectoryAtURL:records includingPropertiesForKeys:nil options:0 error:error];
    if (!urls) return nil;
    for (NSURL *url in urls) {
        @autoreleasepool {
        if (![url.pathExtension isEqual:@"json"]) continue;
        NSError *problem = nil;
        NSDictionary *r = readJSON(url,&problem);
        if (r && validateRecord(r,&problem) && [[r[@"id"] stringByAppendingString:@".json"] isEqual:url.lastPathComponent]) _records[r[@"id"]] = metadata(r);
        else [_problems addObject:problem.localizedDescription ?: @(TOTK_UI("本地有不完整的记录。", "A local record is incomplete."))];
        }
    }
    if (self.baseVersion && !self.versions[self.baseVersion]) { fail(error,@(TOTK_UI("当前版本记录缺失，拒绝覆盖现有存档。", "The current version record is missing. Existing saves will not be overwritten."))); return nil; }
    if (self.workingProfile && ![NSFileManager.defaultManager fileExistsAtPath:self.workingProfile.path]) { fail(error,@(TOTK_UI("当前工作存档缺失，拒绝创建空档替代。", "The working save is missing. An empty save will not be created in its place."))); return nil; }
    return self;
}
- (void)dealloc { if (_lockFD >= 0) close(_lockFD); }
- (NSString *)baseVersion { return _state[@"base"]; }
- (NSURL *)workingProfile { return _state[@"work"] ? [[_localRoot URLByAppendingPathComponent:@"work"] URLByAppendingPathComponent:_state[@"work"] isDirectory:YES] : nil; }
- (NSArray<NSString *> *)pendingProblems {
    NSMutableArray *result = [_problems mutableCopy];
    for (NSString *key in self.versions) if (![self completeLineage:key visiting:[NSMutableSet new]]) {
        [result addObject:@(TOTK_UI("某版本的父版本尚未到达，暂不应用。", "A parent version has not arrived yet. This version will not be applied."))]; break;
    }
    return result;
}
- (NSDictionary *)versions {
    NSMutableDictionary *result = [NSMutableDictionary new];
    for (NSString *key in _records) if ([@[@"save",@"choice",@"restore"] containsObject:_records[key][@"kind"]]) result[key] = _records[key];
    return result;
}
- (NSArray *)backups {
    NSMutableArray *result = [NSMutableArray new]; NSMutableSet *replaced = [NSMutableSet new];
    for (NSDictionary *r in _records.allValues) if ([r[@"kind"] isEqual:@"backup"] && r[@"replaces"]) [replaced addObject:r[@"replaces"]];
    for (NSDictionary *r in _records.allValues) if ([r[@"kind"] isEqual:@"backup"] && ![replaced containsObject:r[@"id"]]) [result addObject:r];
    return result;
}
- (NSArray *)receipts {
    NSMutableArray *result = [NSMutableArray new];
    for (NSDictionary *r in _records.allValues) if ([r[@"kind"] isEqual:@"receipt"]) [result addObject:r];
    return result;
}
- (BOOL)completeLineage:(NSString *)key visiting:(NSMutableSet *)visiting {
    (void)visiting;
    if (_completeCache[key]) return [_completeCache[key] boolValue];
    NSDictionary *versions = self.versions;
    NSMutableDictionary *colors = [NSMutableDictionary new];
    NSMutableArray *stack = [NSMutableArray arrayWithObject:@[key,@NO]];
    while (stack.count) {
        NSArray *item = stack.lastObject; [stack removeLastObject]; NSString *current = item[0];
        if ([item[1] boolValue]) { colors[current] = @2; continue; }
        if ([colors[current] isEqual:@2] || [_completeCache[current] isEqual:@YES]) continue;
        if ([colors[current] isEqual:@1] || !versions[current]) { _completeCache[key] = @NO; return NO; }
        colors[current] = @1; [stack addObject:@[current,@YES]];
        for (NSString *parent in versions[current][@"parents"]) [stack addObject:@[parent,@NO]];
    }
    for (NSString *item in colors) if ([colors[item] isEqual:@2]) _completeCache[item] = @YES;
    return YES;
}
- (NSArray *)heads {
    NSDictionary *versions = self.versions; NSMutableSet *heads = [NSMutableSet new];
    for (NSString *key in versions) if ([self completeLineage:key visiting:[NSMutableSet new]]) [heads addObject:key];
    for (NSString *key in [heads copy]) for (NSString *parent in versions[key][@"parents"]) [heads removeObject:parent];
    return [[heads allObjects] sortedArrayUsingSelector:@selector(compare:)];
}
- (BOOL)isDescendant:(NSString *)candidate of:(NSString *)ancestor {
    NSDictionary *versions=self.versions;
    NSMutableArray *pending = [NSMutableArray arrayWithObject:candidate]; NSMutableSet *seen = [NSMutableSet new];
    while (pending.count) {
        NSString *key = pending.lastObject; [pending removeLastObject];
        if ([key isEqual:ancestor]) return YES;
        if ([seen containsObject:key]) continue;
        [seen addObject:key]; [pending addObjectsFromArray:versions[key][@"parents"] ?: @[]];
    }
    return NO;
}
- (NSDictionary *)recordWithKind:(NSString *)kind {
    return @{@"schema":@1,@"game":GameID,@"id":uuid(),@"kind":kind,@"device":_deviceID,@"deviceName":_deviceName,@"created":@(NSDate.date.timeIntervalSince1970)};
}
- (BOOL)saveState:(NSDictionary *)state error:(NSError **)error {
    if (!writeJSON(state,[_localRoot URLByAppendingPathComponent:@"state.json"],error)) return NO;
    _state = [state mutableCopy]; return YES;
}
- (BOOL)addRecord:(NSDictionary *)r error:(NSError **)error {
    if (!validateRecord(r,error)) return NO;
    NSURL *url = [[_localRoot URLByAppendingPathComponent:@"records"] URLByAppendingPathComponent:[r[@"id"] stringByAppendingString:@".json"]];
    if (_records[r[@"id"]]) {
        id existing = readJSON(url,error);
        return (existing && [existing isEqual:r]) || fail(error,@(TOTK_UI("同一版本标识出现不同内容，拒绝覆盖。", "Different content has the same version identifier. It will not be overwritten.")));
    }
    if (!writeJSON(r,url,error)) return NO;
    _records[r[@"id"]] = metadata(r);
    if ([@[@"save",@"choice",@"restore"] containsObject:r[@"kind"]]) [_completeCache removeAllObjects];
    return YES;
}
- (NSDictionary *)versionRecord:(NSString *)version error:(NSError **)error {
    if (!validID(version) || !self.versions[version]) { fail(error,@(TOTK_UI("此版本尚未到达。", "This version has not arrived yet."))); return nil; }
    NSDictionary *r = readJSON([[_localRoot URLByAppendingPathComponent:@"records"] URLByAppendingPathComponent:[version stringByAppendingString:@".json"]],error);
    return r && validateRecord(r,error) ? r : nil;
}
- (BOOL)ingestRecordAtURL:(NSURL *)file error:(NSError **)error {
    id r = readJSON(file,error);
    if (![r isKindOfClass:NSDictionary.class] || !validID(r[@"id"]) || ![[r[@"id"] stringByAppendingString:@".json"] isEqual:file.lastPathComponent])
        return fail(error,@(TOTK_UI("收到的记录名称或格式不匹配。", "The received record name or format does not match.")));
    return [self addRecord:r error:error];
}
- (BOOL)acknowledgeDownloads:(NSError **)error {
    NSDictionary *versions=self.versions;NSMutableSet *acknowledged=[NSMutableSet new];
    for(NSDictionary *receipt in self.receipts)if([receipt[@"device"] isEqual:_deviceID])[acknowledged addObject:receipt[@"target"]];
    for (NSString *key in versions) {
        if ([versions[key][@"device"] isEqual:_deviceID] || ![self completeLineage:key visiting:[NSMutableSet new]]) continue;
        if (![acknowledged containsObject:key]) {
            NSMutableDictionary *receipt = [[self recordWithKind:@"receipt"] mutableCopy]; receipt[@"target"] = key;
            if (![self addRecord:receipt error:error]) return NO;
        }
    }
    return YES;
}
- (NSDictionary *)snapshotFilesAtProfile:(NSURL *)profile error:(NSError **)error {
    NSURL *accountURL = [profile URLByAppendingPathComponent:@"account.uid"];
    struct stat accountStat;
    struct stat parentStat;
    if (lstat(profile.fileSystemRepresentation,&parentStat) || !S_ISDIR(parentStat.st_mode) ||
        lstat([[profile URLByAppendingPathComponent:@"save"] fileSystemRepresentation],&parentStat) || !S_ISDIR(parentStat.st_mode) ||
        lstat(accountURL.fileSystemRepresentation,&accountStat) || !S_ISREG(accountStat.st_mode)) { fail(error,@(TOTK_UI("没有可用的本机账号存档，或存档路径含链接。", "No usable local account save is available, or the save path contains a link."))); return nil; }
    NSData *account = [NSData dataWithContentsOfURL:accountURL options:0 error:error];
    NSString *uid = identity(account);
    if (!uid) { fail(error,@(TOTK_UI("账号身份不完整，未创建版本。", "Account identity is incomplete. No version was created."))); return nil; }
    NSString *relative = [@"save/" stringByAppendingString:uid];
    NSURL *save = [profile URLByAppendingPathComponent:relative isDirectory:YES];
    struct stat rootStat;
    if (lstat(save.fileSystemRepresentation,&rootStat) || !S_ISDIR(rootStat.st_mode)) { fail(error,@(TOTK_UI("没有已保存的游戏进度。", "There is no saved game progress."))); return nil; }
    save = save.URLByResolvingSymlinksInPath;
    NSMutableDictionary *files = [@{@"account.uid":fileEntry(account)} mutableCopy];
    __block NSError *enumerationError = nil;
    NSDirectoryEnumerator *enumerator = [NSFileManager.defaultManager enumeratorAtURL:save includingPropertiesForKeys:nil options:0 errorHandler:^BOOL(NSURL *url,NSError *problem) {
        (void)url; enumerationError = problem; return NO;
    }];
    NSUInteger total = account.length;
    for (NSURL *url in enumerator) {
        struct stat st;
        if (lstat(url.fileSystemRepresentation,&st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) { fail(error,@(TOTK_UI("存档含链接或特殊文件，拒绝打包。", "The save contains a link or special file and cannot be packaged."))); return nil; }
        if (S_ISDIR(st.st_mode)) continue;
        if (st.st_size < 0 || (uint64_t)st.st_size > MaxSnapshot || total > MaxSnapshot - (NSUInteger)st.st_size || files.count >= 10000) { fail(error,@(TOTK_UI("存档超过大小限制。", "The save exceeds the size limit."))); return nil; }
        NSDictionary *entry=nil;NSError *readError=nil;
        @autoreleasepool {
            NSData *data=[NSData dataWithContentsOfURL:url options:0 error:&readError];
            if(data)entry=fileEntry(data);
        }
        if(!entry){if(error)*error=readError;return nil;}
        total += [entry[@"size"] unsignedIntegerValue];
        NSString *actual = url.URLByResolvingSymlinksInPath.path;
        if (![actual hasPrefix:[save.path stringByAppendingString:@"/"]]) { fail(error,@(TOTK_UI("存档文件不在预期目录内。", "A save file is outside the expected directory."))); return nil; }
        NSString *path = [relative stringByAppendingPathComponent:[actual substringFromIndex:save.path.length + 1]];
        files[path] = entry;
    }
    if (enumerationError) { if (error) *error = enumerationError; return nil; }
    if (files.count == 1) { fail(error,@(TOTK_UI("空存档不会覆盖主进度。", "An empty save will not overwrite the main progress."))); return nil; }
    return files;
}
- (BOOL)workingProfileMatchesBase:(NSError **)error {
    if (!self.workingProfile) return YES;
    NSDictionary *files=[self snapshotFilesAtProfile:self.workingProfile error:error];
    return files && [progressDigest(files) isEqual:self.versions[self.baseVersion][@"progressTree"]];
}
- (NSString *)captureProfile:(NSURL *)profile error:(NSError **)error {
    NSDictionary *files=[self snapshotFilesAtProfile:profile error:error];if(!files)return nil;
    NSString *tree = treeDigest(files);
    if (self.baseVersion && [self.versions[self.baseVersion][@"progressTree"] isEqual:progressDigest(files)]) return self.baseVersion;
    NSMutableDictionary *r = [[self recordWithKind:@"save"] mutableCopy];
    [r addEntriesFromDictionary:@{@"version":_gameVersion,@"parents":self.baseVersion ? @[self.baseVersion] : @[],@"files":files,@"tree":tree}];
    if (![self addRecord:r error:error]) return nil;
    NSMutableDictionary *state = [_state mutableCopy]; state[@"base"] = r[@"id"];
    return [self saveState:state error:error] ? r[@"id"] : nil;
}
- (BOOL)exchangeWithFolder:(NSURL *)folder error:(NSError **)error {
    @autoreleasepool {
    NSURL *cloud = [folder URLByAppendingPathComponent:@"totk-progress-v1" isDirectory:YES];
    [_problems removeAllObjects];
    __block NSError *operationError = nil; NSError *coordinationError = nil;
    NSFileCoordinator *coordinator = [NSFileCoordinator new];
    [coordinator coordinateWritingItemAtURL:cloud options:NSFileCoordinatorWritingForMerging error:&coordinationError byAccessor:^(NSURL *url) {
        makeDir(url,&operationError);
    }];
    if (coordinationError || operationError) { if (error) *error = coordinationError ?: operationError; return NO; }
    __block NSArray *urls = nil;
    [coordinator coordinateReadingItemAtURL:cloud options:0 error:&coordinationError byAccessor:^(NSURL *url) {
        urls = [NSFileManager.defaultManager contentsOfDirectoryAtURL:url includingPropertiesForKeys:nil options:0 error:&operationError];
    }];
    if (coordinationError || operationError) { if (error) *error = coordinationError ?: operationError; return NO; }
    for (NSURL *url in urls) {
        @autoreleasepool {
        if (![url.pathExtension isEqual:@"json"] || !validID(url.lastPathComponent.stringByDeletingPathExtension)) continue;
        __block NSDictionary *r = nil; __block NSError *readError = nil; NSError *ce = nil;
        [[NSFileCoordinator new] coordinateReadingItemAtURL:url options:0 error:&ce byAccessor:^(NSURL *readURL) { r = readJSON(readURL,&readError); }];
        if (ce || readError || ![r isKindOfClass:NSDictionary.class] || ![r[@"id"] isEqual:url.lastPathComponent.stringByDeletingPathExtension] || ![self addRecord:r error:&readError]) {
            [_problems addObject:(ce ?: readError).localizedDescription ?: @(TOTK_UI("云端有待下载或损坏的版本。", "A cloud version is still downloading or is damaged."))];
        }
        }
    }
    NSError *receiptError = nil;
    if (![self acknowledgeDownloads:&receiptError]) [_problems addObject:receiptError.localizedDescription];
    /* Every record is immutable and independent; publishing a child before its parent is safe. */
    for (NSString *key in [_records.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        @autoreleasepool {
        NSError *localError = nil;
        NSDictionary *r = readJSON([[_localRoot URLByAppendingPathComponent:@"records"] URLByAppendingPathComponent:[key stringByAppendingString:@".json"]],&localError);
        if (!r || !validateRecord(r,&localError)) { [_problems addObject:localError.localizedDescription ?: @(TOTK_UI("本地记录损坏，未上传。", "A local record is damaged and was not uploaded."))]; continue; }
        __block NSError *writeError = nil; NSError *ce = nil;
        NSURL *target = [cloud URLByAppendingPathComponent:[key stringByAppendingString:@".json"]];
        [[NSFileCoordinator new] coordinateWritingItemAtURL:target options:NSFileCoordinatorWritingForReplacing error:&ce byAccessor:^(NSURL *url) {
            if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) {
                id remote = readJSON(url,&writeError);
                if (remote && ![remote isEqual:r]) fail(&writeError,@(TOTK_UI("云端版本标识冲突，未覆盖。", "Cloud version identifiers conflict. No version was overwritten.")));
            } else writeJSON(r,url,&writeError);
        }];
        if (ce || writeError) [_problems addObject:(ce ?: writeError).localizedDescription];
        }
    }
    for (NSString *key in self.versions) if (![self completeLineage:key visiting:[NSMutableSet new]]) [_problems addObject:@(TOTK_UI("某版本的父版本尚未到达，暂不应用。", "A parent version has not arrived yet. This version will not be applied."))];
    return YES;
    }
}
- (BOOL)checkout:(NSString *)version resolving:(BOOL)resolve error:(NSError **)error {
    NSDictionary *r = [self versionRecord:version error:error];
    if (!r || ![self completeLineage:version visiting:[NSMutableSet new]] || !validateRecord(r,error)) return fail(error,@(TOTK_UI("此版本尚未完整到达。", "This version has not fully arrived yet.")));
    if (![r[@"version"] isEqual:_gameVersion]) return fail(error,@(TOTK_UI("游戏版本不一致，不能自动载入此存档。", "The game version differs. This save cannot be loaded automatically.")));
    if (resolve) {
        NSMutableDictionary *choice = [[self recordWithKind:@"choice"] mutableCopy];
        [choice addEntriesFromDictionary:@{@"version":_gameVersion,@"parents":self.heads,@"files":r[@"files"],@"tree":r[@"tree"],@"selected":version}];
        if (![self addRecord:choice error:error]) return NO;
        r = choice; version = r[@"id"];
    }
    NSString *workID = uuid(); NSURL *work = [[_localRoot URLByAppendingPathComponent:@"work"] URLByAppendingPathComponent:workID isDirectory:YES];
    if (!makeDir(work,error)) return NO;
    for (NSString *path in r[@"files"]) {
        NSURL *url = [work URLByAppendingPathComponent:path];
        if (!makeDir(url.URLByDeletingLastPathComponent,error)) return NO;
        NSData *data = [[NSData alloc] initWithBase64EncodedString:r[@"files"][path][@"data"] options:0];
        if (![data writeToURL:url options:NSDataWritingAtomic error:error]) return NO;
        int fd = open(url.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);
        if (fd < 0) return fail(error,@(TOTK_UI("不能确认恢复文件已落盘。", "Could not confirm that restored files were durably saved.")));
        int result = fsync(fd); close(fd); if (result) return fail(error,@(TOTK_UI("不能确认恢复文件已落盘。", "Could not confirm that restored files were durably saved.")));
    }
    /* One atomic state change selects a whole new generation. Old generations are retained. */
#if defined(TOTK_PROGRESS_TESTING)
    if (getenv("TOTK_PROGRESS_TEST_FAIL") && !strcmp(getenv("TOTK_PROGRESS_TEST_FAIL"),"before-state")) _exit(77);
#endif
    NSMutableDictionary *state = [_state mutableCopy]; state[@"base"] = version; state[@"work"] = workID;
    BOOL saved = [self saveState:state error:error];
#if defined(TOTK_PROGRESS_TESTING)
    if (getenv("TOTK_PROGRESS_TEST_FAIL") && !strcmp(getenv("TOTK_PROGRESS_TEST_FAIL"),"after-state")) _exit(77);
#endif
    return saved;
}
- (BOOL)canLoadVersion:(NSString *)version error:(NSError **)error {
    NSDictionary *record=[self versionRecord:version error:error];
    if(!record)return NO;
    if(![self completeLineage:version visiting:[NSMutableSet new]])return fail(error,@(TOTK_UI("此版本的必要记录尚未到齐，请稍后重试。", "Required records for this version have not all arrived. Try again later.")));
    return [record[@"version"] isEqual:_gameVersion]||fail(error,@(TOTK_UI("游戏版本不一致，不能载入此存档。", "The game version differs. This save cannot be loaded.")));
}
- (NSString *)nameBackup:(NSString *)name forVersion:(NSString *)version replacing:(NSString *)backupID error:(NSError **)error {
    if (!self.versions[version] || ![self completeLineage:version visiting:[NSMutableSet new]] || (backupID && ![_records[backupID][@"kind"] isEqual:@"backup"])) { fail(error,@(TOTK_UI("备份目标不可用。", "The backup target is unavailable."))); return nil; }
    NSMutableDictionary *r = [[self recordWithKind:@"backup"] mutableCopy];
    r[@"name"] = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]; r[@"target"] = version;
    if (backupID) r[@"replaces"] = backupID;
    return [self addRecord:r error:error] ? r[@"id"] : nil;
}
- (NSString *)restoreVersion:(NSString *)version error:(NSError **)error {
    NSDictionary *source = [self versionRecord:version error:error];
    if (!source || ![source[@"version"] isEqual:_gameVersion] || ![self completeLineage:version visiting:[NSMutableSet new]]) { fail(error,@(TOTK_UI("历史版本不完整或不兼容。", "This historical version is incomplete or incompatible."))); return nil; }
    NSMutableDictionary *r = [[self recordWithKind:@"restore"] mutableCopy];
    [r addEntriesFromDictionary:@{@"version":_gameVersion,@"parents":self.heads,@"files":source[@"files"],@"tree":source[@"tree"],@"selected":version}];
    if (![self addRecord:r error:error] || ![self checkout:r[@"id"] resolving:NO error:error]) return nil;
    return r[@"id"];
}
@end

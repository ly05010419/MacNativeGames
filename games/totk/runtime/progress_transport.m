#import "progress_transport.h"
#import "progress_errors.h"
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

static NSString *transportHash(NSData *data) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256(data.bytes,(CC_LONG)data.length,hash);
#pragma clang diagnostic pop
    NSMutableString *s = [NSMutableString new];
    for (NSUInteger i=0;i<sizeof hash;i++) [s appendFormat:@"%02x",hash[i]];
    return s;
}
static BOOL recordName(NSURL *url) {
    NSString *name=url.lastPathComponent;
    NSString *key=name.stringByDeletingPathExtension;
    return [url.pathExtension isEqual:@"json"] && [key isEqual:key.lowercaseString] && [[NSUUID alloc] initWithUUIDString:key];
}
static NSData *readTransfer(NSURL *url, NSError **error) {
    struct stat st;
    if (lstat(url.fileSystemRepresentation,&st) || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>512ll*1024*1024) {
        if (error) *error=[NSError errorWithDomain:@"TOTKProgress" code:2 userInfo:@{NSLocalizedDescriptionKey:@(TOTK_UI("云端文件尚不可读、不是普通文件或过大。", "The cloud file is not readable yet, is not a regular file, or is too large."))}];
        return nil;
    }
    return [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
}
@implementation TOTKProgressTransport {
    NSURL *_root;
    dispatch_queue_t _queue;
    NSMutableDictionary *_remoteSignatures;
}
- (instancetype)initWithLocalRoot:(NSURL *)root {
    if ((self=[super init])) { _root=root; _queue=dispatch_queue_create("totk.progress.file-provider",DISPATCH_QUEUE_SERIAL); _remoteSignatures=[NSMutableDictionary new]; }
    return self;
}
- (void)exchangeWithFolder:(NSURL *)folder completion:(void (^)(NSArray<NSURL *> *,NSArray<NSString *> *))completion {
    dispatch_async(_queue, ^{ @autoreleasepool {
        NSMutableArray *incoming=[NSMutableArray new],*problems=[NSMutableArray new];
        NSURL *cloud=[folder URLByAppendingPathComponent:@"totk-progress-v1" isDirectory:YES];
        NSURL *inbox=[self->_root URLByAppendingPathComponent:@"incoming" isDirectory:YES];
        NSError *error=nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:inbox withIntermediateDirectories:YES attributes:nil error:&error]) { completion(@[],@[TOTKProgressErrorMessage(error)]);return; }
        __block NSError *operationError=nil; NSError *coordinationError=nil;
        if (![NSFileManager.defaultManager fileExistsAtPath:cloud.path]) {
            [[NSFileCoordinator new] coordinateWritingItemAtURL:cloud options:NSFileCoordinatorWritingForMerging error:&coordinationError byAccessor:^(NSURL *url) {
                [NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:&operationError];
            }];
        }
        if (operationError||coordinationError) { completion(@[],@[TOTKProgressErrorMessage(operationError?:coordinationError)]);return; }
        __block NSArray *remoteFiles=nil;
        [[NSFileCoordinator new] coordinateReadingItemAtURL:cloud options:0 error:&coordinationError byAccessor:^(NSURL *url) {
            remoteFiles=[NSFileManager.defaultManager contentsOfDirectoryAtURL:url includingPropertiesForKeys:nil options:0 error:&operationError];
        }];
        if (operationError||coordinationError) { completion(@[],@[TOTKProgressErrorMessage(operationError?:coordinationError)]);return; }
        NSURL *records=[self->_root URLByAppendingPathComponent:@"records"];
        NSMutableSet *remoteNames=[NSMutableSet new];
        for (NSURL *remote in remoteFiles) { @autoreleasepool {
            if (!recordName(remote)) continue;
            [remoteNames addObject:remote.lastPathComponent];
            __block NSError *readError=nil; NSError *ce=nil;
            [[NSFileCoordinator new] coordinateReadingItemAtURL:remote options:0 error:&ce byAccessor:^(NSURL *url) {
                NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:url.path error:&readError];
                NSString *signature=attrs?[NSString stringWithFormat:@"%@/%@",attrs[NSFileModificationDate],attrs[NSFileSize]]:nil;
                if (signature && [signature isEqual:self->_remoteSignatures[remote.lastPathComponent]]) return;
                NSData *data=readTransfer(url,&readError); if (!data) return;
                NSURL *local=[records URLByAppendingPathComponent:remote.lastPathComponent];
                if ([NSFileManager.defaultManager fileExistsAtPath:local.path]) {
                    NSData *existing=readTransfer(local,&readError);
                    if (!existing) return;
                    if (![transportHash(existing) isEqual:transportHash(data)]) {
                        readError=[NSError errorWithDomain:@"TOTKProgress" code:3 userInfo:@{NSLocalizedDescriptionKey:@(TOTK_UI("同一云版本出现不同内容；本机副本已保留，未覆盖。", "The same cloud version has different content. The local copy is retained and was not overwritten."))}]; return;
                    }
                } else {
                    NSURL *target=[inbox URLByAppendingPathComponent:remote.lastPathComponent];
                    if (![data writeToURL:target options:NSDataWritingAtomic error:&readError]) return;
                    [incoming addObject:target];
                    return; // Retry until the store has verified and accepted this inbox record.
                }
                if (signature) self->_remoteSignatures[remote.lastPathComponent]=signature;
            }];
            if (ce||readError) [problems addObject:TOTKProgressErrorMessage(ce?:readError)];
        }}
        NSArray *localFiles=[NSFileManager.defaultManager contentsOfDirectoryAtURL:records includingPropertiesForKeys:nil options:0 error:&error];
        if (!localFiles) [problems addObject:TOTKProgressErrorMessage(error)];
        for (NSURL *local in localFiles) { @autoreleasepool {
            if (!recordName(local) || [remoteNames containsObject:local.lastPathComponent]) continue;
            NSURL *target=[cloud URLByAppendingPathComponent:local.lastPathComponent];
            __block NSError *writeError=nil; NSError *ce=nil;
            [[NSFileCoordinator new] coordinateWritingItemAtURL:target options:NSFileCoordinatorWritingForReplacing error:&ce byAccessor:^(NSURL *url) {
                // A concurrent sender may have published this immutable ID after enumeration.
                NSData *data=readTransfer(local,&writeError); if (!data) return;
                if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) {
                    NSData *existing=readTransfer(url,&writeError);
                    if (existing && ![transportHash(existing) isEqual:transportHash(data)])
                        writeError=[NSError errorWithDomain:@"TOTKProgress" code:3 userInfo:@{NSLocalizedDescriptionKey:@(TOTK_UI("云版本标识冲突，拒绝覆盖。", "Cloud version identifiers conflict. Existing content will not be overwritten."))}];
                } else [data writeToURL:url options:NSDataWritingAtomic error:&writeError];
            }];
            if (ce||writeError) [problems addObject:TOTKProgressErrorMessage(ce?:writeError)];
        }}
        completion(incoming,problems);
    }});
}
@end

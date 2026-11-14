/* MIT. Descriptor-checked caches for the pipelines used by the Metal 4 encoder.
 * Pipeline descriptors remain the backend's actual descriptors. An archive hit
 * is tested by Metal, never by a runtime or source-tree hash. Only the offline
 * builder writes; each worker owns its capture archive and serializes it once.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>

@interface MNGPipelineCapture : NSObject
@property(nonatomic, strong) id<MTLBinaryArchive> archive;
@end
@implementation MNGPipelineCapture
@end
static NSArray<id<MTLBinaryArchive>> *mng_archives;
static NSMutableArray<MNGPipelineCapture *> *mng_captures;
static NSString *mng_archive_folder;
static _Atomic unsigned long mng_archive_hits, mng_archive_misses;
static BOOL mng_archive_build, mng_archive_strict;

static void mng_archive_init(id<MTLDevice> device) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *folder = getenv("DM_PSO_ARCHIVE");
        mng_archive_folder = folder && *folder && strcmp(folder, "off") ? @(folder) : nil;
        mng_archive_build = getenv("DM_PSO_ARCHIVE_BUILD") && atoi(getenv("DM_PSO_ARCHIVE_BUILD"));
        mng_archive_strict = getenv("DM_PSO_REQUIRE_ARCHIVE") && atoi(getenv("DM_PSO_REQUIRE_ARCHIVE"));
        NSMutableArray *loaded = [NSMutableArray array];
        mng_captures = [NSMutableArray array];
        if (mng_archive_folder) {
            for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:mng_archive_folder error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
                if (![name hasSuffix:@".mngarc"]) continue;
                MTLBinaryArchiveDescriptor *descriptor = [MTLBinaryArchiveDescriptor new];
                descriptor.url = [NSURL fileURLWithPath:[mng_archive_folder stringByAppendingPathComponent:name]];
                NSError *error = nil;
                id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
                if (archive) [loaded addObject:archive];
                else fprintf(stderr, "[MNG] archive unavailable: %s: %s\n", name.UTF8String, error.localizedDescription.UTF8String);
            }
        }
        mng_archives = loaded;
    });
}

static id<MTLBinaryArchive> mng_capture(id<MTLDevice> device, NSError **error) {
    if (!mng_archive_build || !mng_archive_folder) return nil;
    NSMutableDictionary *local = NSThread.currentThread.threadDictionary;
    MNGPipelineCapture *capture = local[@"MNGPipelineCapture"];
    if (!capture) {
        capture = [MNGPipelineCapture new];
        capture.archive = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:error];
        if (!capture.archive) return nil;
        local[@"MNGPipelineCapture"] = capture;
        @synchronized(mng_captures) { [mng_captures addObject:capture]; }
    }
    return capture.archive;
}

static NSError *mng_archive_missing(void) {
    return [NSError errorWithDomain:@"MacNativeGames.PipelineArchive" code:1
                          userInfo:@{NSLocalizedDescriptionKey: @"The actual pipeline descriptor is absent from the archive"}];
}

static id<MTLRenderPipelineState> mng_render_pipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor *source, NSError **error) {
    mng_archive_init(device);
    MTLRenderPipelineDescriptor *descriptor = [source copy];
    descriptor.binaryArchives = mng_archives;
    id<MTLRenderPipelineState> state = nil;
    if (mng_archives.count) state = [device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
    if (state) { atomic_fetch_add(&mng_archive_hits, 1); return state; }
    atomic_fetch_add(&mng_archive_misses, 1);
    if (mng_archive_strict) { if (error) *error = mng_archive_missing(); return nil; }
    id<MTLBinaryArchive> capture = mng_capture(device, error);
    if (mng_archive_build) {
        if (!capture || ![capture addRenderPipelineFunctionsWithDescriptor:descriptor error:error]) return nil;
        descriptor.binaryArchives = [mng_archives arrayByAddingObject:capture];
    }
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

static id<MTLComputePipelineState> mng_compute_pipeline(id<MTLDevice> device, id<MTLFunction> function, NSError **error) {
    mng_archive_init(device);
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function;
    descriptor.binaryArchives = mng_archives;
    id<MTLComputePipelineState> state = nil;
    if (mng_archives.count) state = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
    if (state) { atomic_fetch_add(&mng_archive_hits, 1); return state; }
    atomic_fetch_add(&mng_archive_misses, 1);
    if (mng_archive_strict) { if (error) *error = mng_archive_missing(); return nil; }
    id<MTLBinaryArchive> capture = mng_capture(device, error);
    if (mng_archive_build) {
        if (!capture || ![capture addComputePipelineFunctionsWithDescriptor:descriptor error:error]) return nil;
        descriptor.binaryArchives = [mng_archives arrayByAddingObject:capture];
    }
    return [device newComputePipelineStateWithDescriptor:descriptor options:0 reflection:nil error:error];
}

/* Call after joining every compile worker. A cache-only update writes nothing. */
static BOOL mng_archive_flush(NSError **error) {
    if (!mng_captures.count) return YES;
    if (![NSFileManager.defaultManager createDirectoryAtPath:mng_archive_folder withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    for (MNGPipelineCapture *capture in mng_captures) {
        NSString *path = [mng_archive_folder stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@".mngarc"]];
        NSString *temporary = [path stringByAppendingString:@".partial"];
        if (![capture.archive serializeToURL:[NSURL fileURLWithPath:temporary] error:error] ||
            ![NSFileManager.defaultManager moveItemAtPath:temporary toPath:path error:error]) return NO;
    }
    [mng_captures removeAllObjects];
    return YES;
}

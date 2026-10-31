// Metal 4 → Metal 3 兼容层实现（DM_NVN_M3=1）
// Implementation of the Metal 4 -> Metal 3 compatibility layer.
#import "m3_compat.h"
#if DM_NVN_M3

#import <os/lock.h>
#import <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// GPU 地址 → MTLBuffer 反查表
// ---------------------------------------------------------------------------
typedef struct { uint64_t gpu, len; __unsafe_unretained id<MTLBuffer> buf; } M3BufferEntry;
static M3BufferEntry *g_m3_bufs;
static NSUInteger g_m3_nbufs, g_m3_capacity;
static os_unfair_lock g_m3_lk = OS_UNFAIR_LOCK_INIT;

void m3_buf_add(id<MTLBuffer> b) {
    if (!b) return;
    os_unfair_lock_lock(&g_m3_lk);
    for (NSUInteger i = 0; i < g_m3_nbufs; i++) {
        if (g_m3_bufs[i].buf == b) {
            os_unfair_lock_unlock(&g_m3_lk);
            return;
        }
    }
    // Map/UI streaming can exceed 1024 live buffers. Never silently drop a registration.
    if (g_m3_nbufs == g_m3_capacity) {
        if (g_m3_capacity > SIZE_MAX / (2 * sizeof(M3BufferEntry))) abort();
        NSUInteger capacity = g_m3_capacity ? g_m3_capacity * 2 : 1024;
        M3BufferEntry *entries = realloc(g_m3_bufs, capacity * sizeof(*entries));
        if (!entries) { fprintf(stderr, "[dm] [m3] GPU buffer registry allocation failed\n"); abort(); }
        g_m3_bufs = entries;
        if (g_m3_capacity) fprintf(stderr, "[dm] [m3] GPU buffer registry grew: %lu -> %lu\n",
                                  (unsigned long)g_m3_capacity, (unsigned long)capacity);
        g_m3_capacity = capacity;
    }
    g_m3_bufs[g_m3_nbufs++] = (M3BufferEntry){b.gpuAddress, b.length, b};
    os_unfair_lock_unlock(&g_m3_lk);
}

void m3_buf_del(id<MTLBuffer> b) {
    if (!b) return;
    os_unfair_lock_lock(&g_m3_lk);
    for (NSUInteger i = 0; i < g_m3_nbufs; i++)
        if (g_m3_bufs[i].buf == b) { g_m3_bufs[i] = g_m3_bufs[--g_m3_nbufs]; break; }
    os_unfair_lock_unlock(&g_m3_lk);
}

// 最近命中的那一项先试：同一通道里的绑定几乎都落在同一两个缓冲上
// Try the most recently hit entry first: bindings within one pass almost always land on the same one or two buffers.
id<MTLBuffer> m3_buf(uint64_t gpu, NSUInteger *off) {
    static _Thread_local NSUInteger last;
    id<MTLBuffer> r = nil;
    *off = 0;
    if (!gpu) return nil;
    os_unfair_lock_lock(&g_m3_lk);
    NSUInteger i = (last < g_m3_nbufs && gpu >= g_m3_bufs[last].gpu && gpu - g_m3_bufs[last].gpu < g_m3_bufs[last].len) ? last : NSNotFound;
    for (NSUInteger k = 0; i == NSNotFound && k < g_m3_nbufs; k++)
        if (gpu >= g_m3_bufs[k].gpu && gpu - g_m3_bufs[k].gpu < g_m3_bufs[k].len) i = k;
    if (i != NSNotFound) { last = i; r = g_m3_bufs[i].buf; *off = (NSUInteger)(gpu - g_m3_bufs[i].gpu); }
    os_unfair_lock_unlock(&g_m3_lk);
    return r;
}

// ---------------------------------------------------------------------------
// 当前编码器（供时间戳采样用；Metal 3 的计数采样挂在编码器上而不是命令缓冲上）
// Current encoder (for counter sampling: Metal 3 samples on an encoder, not on a command buffer).
// ---------------------------------------------------------------------------
static __unsafe_unretained id g_m3_cur_enc;

void m3_set_cur_enc(id enc) { g_m3_cur_enc = enc; }

// ---------------------------------------------------------------------------
// 参数表
// ---------------------------------------------------------------------------
@implementation M3ArgTableDescriptor
@end

static int g_m3_red_bound;   // 未登记地址只报一次红，避免刷屏 | report an unregistered address once

@implementation M3ArgTable

- (void)setAddress:(uint64_t)gpu atIndex:(NSUInteger)i {
    NSUInteger off = 0;
    id<MTLBuffer> b = m3_buf(gpu, &off);
    if (gpu && !b) {
        if (!g_m3_red_bound++) fprintf(stderr, "[dm] [m3] 【红】绑定：显卡地址 %#llx 不在任何登记过的缓冲里（槽 %lu）\n",
                                       (unsigned long long)gpu, (unsigned long)i);
        return;
    }
    switch (_stage) {
        case M3_STAGE_VERTEX:   [(id<MTLRenderCommandEncoder>)_enc setVertexBuffer:b offset:off atIndex:i]; break;
        case M3_STAGE_FRAGMENT: [(id<MTLRenderCommandEncoder>)_enc setFragmentBuffer:b offset:off atIndex:i]; break;
        default:                [(id<MTLComputeCommandEncoder>)_enc setBuffer:b offset:off atIndex:i]; break;
    }
}

- (void)setTexture:(id<MTLTexture>)tex atIndex:(NSUInteger)i {
    switch (_stage) {
        case M3_STAGE_VERTEX:   [(id<MTLRenderCommandEncoder>)_enc setVertexTexture:tex atIndex:i]; break;
        case M3_STAGE_FRAGMENT: [(id<MTLRenderCommandEncoder>)_enc setFragmentTexture:tex atIndex:i]; break;
        default:                [(id<MTLComputeCommandEncoder>)_enc setTexture:tex atIndex:i]; break;
    }
}

- (void)setSamplerState:(id<MTLSamplerState>)smp atIndex:(NSUInteger)i {
    switch (_stage) {
        case M3_STAGE_VERTEX:   [(id<MTLRenderCommandEncoder>)_enc setVertexSamplerState:smp atIndex:i]; break;
        case M3_STAGE_FRAGMENT: [(id<MTLRenderCommandEncoder>)_enc setFragmentSamplerState:smp atIndex:i]; break;
        default:                [(id<MTLComputeCommandEncoder>)_enc setSamplerState:smp atIndex:i]; break;
    }
}

@end

void m3_attach(M3ArgTable *t, id enc, int stage) {
    t.enc = enc;
    t.stage = stage;
}

// ---------------------------------------------------------------------------
// GPU 时间戳
// ---------------------------------------------------------------------------
@implementation M3CounterHeapDescriptor
@end

@implementation M3CounterHeap {
    id<MTLCounterSampleBuffer> _sb;
}

- (instancetype)initWithDevice:(id<MTLDevice>)dev count:(NSUInteger)count {
    if ((self = [super init])) {
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> cs in [dev counterSets])
            if ([[cs name] isEqualToString:MTLCommonCounterSetTimestamp]) { set = cs; break; }
        if (!set) { fprintf(stderr, "[dm] [m3] 【红】设备没有 timestamp 计数器集\n"); return self; }
        MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
        d.counterSet = set;
        d.sampleCount = count;
        d.storageMode = MTLStorageModeShared;
        NSError *e = nil;
        _sb = [dev newCounterSampleBufferWithDescriptor:d error:&e];
        if (!_sb) fprintf(stderr, "[dm] [m3] 【红】建立计数器采样缓冲失败：%s\n", e.localizedDescription.UTF8String);
    }
    return self;
}

- (id<MTLCounterSampleBuffer>)sampleBuffer { return _sb; }

- (NSData *)resolveCounterRange:(NSRange)r { return [_sb resolveCounterRange:r]; }

@end

// 在「当前编码器」上采一个时间戳；Metal 3 没有命令缓冲级别的写法
// Sample one timestamp on the current encoder; Metal 3 has no command-buffer-level form.
void m3_timestamp(M3CounterHeap *h, NSUInteger i) {
    if (!h || !g_m3_cur_enc) return;
    id<MTLCounterSampleBuffer> sb = [h sampleBuffer];
    if (!sb) return;
    id e = g_m3_cur_enc;
    if ([e conformsToProtocol:@protocol(MTLRenderCommandEncoder)])
        [(id<MTLRenderCommandEncoder>)e sampleCountersInBuffer:sb atSampleIndex:i withBarrier:NO];
    else if ([e conformsToProtocol:@protocol(MTLComputeCommandEncoder)])
        [(id<MTLComputeCommandEncoder>)e sampleCountersInBuffer:sb atSampleIndex:i withBarrier:NO];
    else if ([e conformsToProtocol:@protocol(MTLBlitCommandEncoder)])
        [(id<MTLBlitCommandEncoder>)e sampleCountersInBuffer:sb atSampleIndex:i withBarrier:NO];
}

// ---------------------------------------------------------------------------
// 提交回调
// ---------------------------------------------------------------------------
@implementation M3CommitFeedback
+ (instancetype)withCommandBuffer:(id<MTLCommandBuffer>)cb {
    M3CommitFeedback *f = [M3CommitFeedback new];
    f->_GPUStartTime = cb.GPUStartTime;
    f->_GPUEndTime = cb.GPUEndTime;
    f->_error = cb.error;
    return f;
}
@end

void m3_commit(id<MTLCommandBuffer> cb, M3CommitOptions *o) {
    if (!cb) return;
    if (o.handlers.count) {
        NSArray *hs = [o.handlers copy];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            M3CommitFeedback *fb = [M3CommitFeedback withCommandBuffer:c];
            for (M3CommitFeedbackHandler h in hs) h(fb);
        }];
    }
    [cb commit];
    /* 判定用：TOTK_M3_SYNC_PRESENT=1 时每次提交后等 GPU 完成，排除 drawable 重叠使用时序 */
    /* Diagnostic: with TOTK_M3_SYNC_PRESENT=1 wait for the GPU after every commit, to rule out drawable overlap. */
    static int syncp = -1;
    if (syncp < 0) syncp = getenv("TOTK_M3_SYNC_PRESENT") != NULL;
    if (syncp) [cb waitUntilCompleted];
}

@implementation M3CommitOptions

- (void)addFeedbackHandler:(M3CommitFeedbackHandler)block {
    if (!_handlers) _handlers = [NSMutableArray array];
    if (block) [_handlers addObject:[block copy]];
}

@end

// ---------------------------------------------------------------------------
// 命令分配器 / 队列描述符（Metal 3 无对应概念，只承接属性赋值）
// ---------------------------------------------------------------------------
@implementation M3CommandAllocatorDescriptor
@end
@implementation M3CommandAllocator
@end
@implementation M3CommandQueueDescriptor
@end

// ---------------------------------------------------------------------------
// 着色器编译
// ---------------------------------------------------------------------------
@implementation M3CompilerDescriptor
@end

@implementation M3Compiler {
    id<MTLDevice> _dev;
}

- (instancetype)initWithDevice:(id<MTLDevice>)dev {
    if ((self = [super init])) _dev = dev;
    return self;
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)src options:(MTLCompileOptions *)opts error:(NSError **)err {
    return [_dev newLibraryWithSource:src options:opts error:err];
}

@end

void m3_draw_indexed(id enc, MTLPrimitiveType p, NSUInteger n, MTLIndexType t,
                     uint64_t gpu, NSUInteger len, NSUInteger inst, NSInteger baseV, NSUInteger baseI) {
    (void)len;
    if (!n) return;
    NSUInteger off = 0;
    id<MTLBuffer> b = m3_buf(gpu, &off);
    if (!b) return;
    [(id<MTLRenderCommandEncoder>)enc drawIndexedPrimitives:p indexCount:n indexType:t
                                                indexBuffer:b indexBufferOffset:off
                                              instanceCount:inst baseVertex:baseV baseInstance:baseI];
}

#endif /* DM_NVN_M3 */

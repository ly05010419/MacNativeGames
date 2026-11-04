// Metal 4 → Metal 3 兼容层（DM_NVN_M3=1）
// Metal 4 -> Metal 3 compatibility layer (DM_NVN_M3=1)
//
// 只能在所有苹果头文件（Metal.framework 等）之后引入：这里的宏会改写 MTL4 类型名，
// 若在解析苹果头文件时生效，会连 Metal 自己的 @interface 一起改名（重复定义）。
// Must be imported after every Apple header (Metal.framework etc.): the macros below rename
// MTL4 type names, and if they are active while Apple's headers are parsed they rename Metal's
// own @interface declarations too (duplicate definition).
//
// 设计取舍：不包装编码器（那要改 391 个站点的类型与所有权），而是
//   1) 把 MTL4 类型名宏映射到 Metal 3 的对应物；
//   2) 参数表做成真实对象，自己记住「挂到哪个编码器 + 哪个阶段」，绑定时直接落到底层编码器；
//   3) 屏障为空操作（Metal 3 靠自动 hazard tracking）；
//   4) 时间戳/提交回调/分配器各自给一个薄壳。
// Design: do not wrap encoders (that would touch the type and ownership of 391 sites). Instead
//   1) macro-map the MTL4 type names onto their Metal 3 counterparts;
//   2) make the argument table a real object that remembers which encoder and stage it is attached to
//      and binds straight onto the underlying encoder;
//   3) make barriers no-ops (Metal 3 relies on automatic hazard tracking);
//   4) give timestamps, commit feedback and allocators a thin shell each.
#pragma once
#if DM_NVN_M3

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

// ---------------------------------------------------------------------------
// 类型映射
// ---------------------------------------------------------------------------
// Metal 4 用一个 compute 编码器兼做 blit，Metal 3 分成 MTLComputeCommandEncoder 与
// MTLBlitCommandEncoder。这里的协议同时继承两者，使声明处无需区分；实际对象仍由
// blit() 与 enc_c_begin() 各自给出，用错会在运行期暴露而不是被类型系统挡住。
// Metal 4 does blits on a compute encoder; Metal 3 splits them into MTLComputeCommandEncoder and
// MTLBlitCommandEncoder. This protocol inherits both so declarations need not distinguish; the actual
// objects still come from blit() and enc_c_begin(), so misuse shows at run time rather than being caught.
@protocol M3ComputeBlit <MTLComputeCommandEncoder, MTLBlitCommandEncoder>
@end

#define MTL4RenderCommandEncoder    MTLRenderCommandEncoder
#define MTL4ComputeCommandEncoder   M3ComputeBlit
#define MTL4CommandEncoder          MTLCommandEncoder
#define MTL4RenderPassDescriptor    MTLRenderPassDescriptor
#define MTL4CommandBuffer           MTLCommandBuffer
#define MTL4CommandQueue            MTLCommandQueue

#define MTL4ArgumentTable           M3ArgTable
#define MTL4ArgumentTableDescriptor M3ArgTableDescriptor
#define MTL4CounterHeap             M3CounterHeap
#define MTL4CounterHeapDescriptor   M3CounterHeapDescriptor
#define MTL4CommitOptions           M3CommitOptions
#define MTL4CommitFeedback          M3CommitFeedback
#define MTL4CommitFeedbackHandler   M3CommitFeedbackHandler
#define MTL4CommitOptionsNoop       M3CommitOptionsNoop
#define MTL4CommandAllocator        M3CommandAllocator
#define MTL4CommandAllocatorDescriptor M3CommandAllocatorDescriptor
#define MTL4CommandQueueDescriptor  M3CommandQueueDescriptor
#define MTL4Compiler                M3Compiler
#define MTL4CompilerDescriptor      M3CompilerDescriptor
#define MTL4TimestampHeapEntry      M3TimestampHeapEntry
#define MTL4VisibilityOptions       M3VisibilityOptions
#define MTL4CounterHeapTypeTimestamp 0
#define MTL4VisibilityOptionDevice  0
#define MTL4VisibilityOptionNone    0
#define MTL4TimestampGranularityRelaxed 0

// MetalFX 帧生成在 macOS 15 上不存在（连经典版都是 macos(26.0)）：整块在调用点用 #if 排除。
// MetalFX frame interpolation does not exist on macOS 15 (even the classic class is macos(26.0)):
// the call sites are excluded with #if.
/* 帧生成不映射：MTLFXFrameInterpolator 连经典版都是 macos(26.0)，调用点必须用 #if 排除 */
/* Frame interpolation is not mapped: even the classic MTLFXFrameInterpolator is macos(26.0),
   so the call sites must be excluded with #if. */
#define MTL4FXSpatialScaler         MTLFXSpatialScaler

typedef NSUInteger M3VisibilityOptions;

// ---------------------------------------------------------------------------
// 屏障：Metal 3 没有显式屏障，依赖关系由自动 hazard tracking 保证
// Barriers: Metal 3 has no explicit barrier; dependencies come from automatic hazard tracking
// ---------------------------------------------------------------------------
#define m3_barrier(...) ((void)0)

// 把 `[e barrierAfterQueueStages:... beforeStages:... visibilityOptions:...]`
// 整句换成空操作（供 sed 化改写使用）
// Turn a whole `[e barrierAfter... beforeStages:... visibilityOptions:...]` sentence into a no-op.
#define M3_BARRIER_DROP(...) ((void)0)

// ---------------------------------------------------------------------------
// GPU 地址 → MTLBuffer 反查表
// 回放层手里是显卡地址，老接口按「缓冲对象 + 偏移」绑定，所以要有个登记表。
// GPU address -> MTLBuffer registry: the replay layer holds GPU addresses, while the older API
// binds by "buffer object + offset", so registered buffers are looked up by address range.
// ---------------------------------------------------------------------------
void m3_buf_add(id<MTLBuffer> b);
void m3_buf_del(id<MTLBuffer> b);
id<MTLBuffer> m3_buf(uint64_t gpu, NSUInteger *off);

// ---------------------------------------------------------------------------
// 参数表
// ---------------------------------------------------------------------------
enum { M3_STAGE_VERTEX = 0, M3_STAGE_FRAGMENT = 1, M3_STAGE_COMPUTE = 2 };

@interface M3ArgTableDescriptor : NSObject
@property (nonatomic) NSUInteger maxBufferBindCount;
@property (nonatomic) NSUInteger maxTextureBindCount;
@property (nonatomic) NSUInteger maxSamplerStateBindCount;
@end

// 真实对象：记住挂到哪个编码器、哪个阶段；绑定直接落到该编码器
// A real object: remembers the encoder and stage it is attached to; binds straight onto it.
@protocol M3ArgTable <NSObject>
- (void)setAddress:(uint64_t)gpu atIndex:(NSUInteger)i;
- (void)setTexture:(id<MTLTexture>)tex atIndex:(NSUInteger)i;
- (void)setSamplerState:(id<MTLSamplerState>)smp atIndex:(NSUInteger)i;
@end

@interface M3ArgTable : NSObject <M3ArgTable>
@property (nonatomic, strong) id enc;        // MTLRenderCommandEncoder / MTLComputeCommandEncoder
@property (nonatomic) int stage;
@end

// 挂载：替代 `[enc setArgumentTable:t]` 与 `[enc setArgumentTable:t atStages:s]`
// Attach: replaces `[enc setArgumentTable:t]` and `[enc setArgumentTable:t atStages:s]`.
void m3_attach(M3ArgTable *t, id enc, int stage);

// ---------------------------------------------------------------------------
// GPU 时间戳：MTL4CounterHeap → MTLCounterSampleBuffer
// ---------------------------------------------------------------------------
@interface M3CounterHeapDescriptor : NSObject
@property (nonatomic) NSUInteger type;
@property (nonatomic) NSUInteger count;
@end

@protocol M3CounterHeap <NSObject>
- (NSData *)resolveCounterRange:(NSRange)r;
@end

@interface M3CounterHeap : NSObject <M3CounterHeap>
- (instancetype)initWithDevice:(id<MTLDevice>)dev count:(NSUInteger)count;
- (id<MTLCounterSampleBuffer>)sampleBuffer;
@end

typedef struct { uint64_t timestamp; } M3TimestampHeapEntry;

// 当前编码器 + 时间戳采样（Metal 3 的计数采样挂在编码器上，不在命令缓冲上）
// Current encoder + timestamp sampling (Metal 3 samples on an encoder, not on a command buffer).
void m3_set_cur_enc(id enc);
void m3_timestamp(M3CounterHeap *h, NSUInteger i);

// Metal 4 支持按显卡地址 + 长度绘制索引；Metal 3 只接受 MTLBuffer + 偏移
// Metal 4 draws indexed by GPU address + length; Metal 3 only takes an MTLBuffer + offset.
void m3_draw_indexed(id enc, MTLPrimitiveType p, NSUInteger n, MTLIndexType t,
                     uint64_t gpu, NSUInteger len, NSUInteger inst, NSInteger baseV, NSUInteger baseI);

// ---------------------------------------------------------------------------
// 提交回调：MTL4CommitOptions/CommitFeedback → addCompletedHandler:
// ---------------------------------------------------------------------------
typedef void (^M3CommitFeedbackHandler)(id feedback);

@class M3CommitOptions;
@protocol M3CommitFeedback <NSObject>
@property (nonatomic, readonly) double GPUStartTime;
@property (nonatomic, readonly) double GPUEndTime;
@property (nonatomic, readonly) NSError *error;
@end
@interface M3CommitFeedback : NSObject <M3CommitFeedback>
// 协议里是 readonly；类里声明为读写以便合成实例变量
// Read-only in the protocol; declared read-write here so the ivars are synthesised.
@property (nonatomic) double GPUStartTime;
@property (nonatomic) double GPUEndTime;
@property (nonatomic, strong) NSError *error;
+ (instancetype)withCommandBuffer:(id<MTLCommandBuffer>)cb;
@end

// 提交：替代 `[queue commit:buffers count:1 options:o]`
// Commit: replaces `[queue commit:buffers count:1 options:o]`.
void m3_commit(id<MTLCommandBuffer> cb, M3CommitOptions *o);

@interface M3CommitOptions : NSObject
@property (nonatomic, strong) NSMutableArray *handlers;   // .handlers 供 q4_commit 的退化路径遍历
- (void)addFeedbackHandler:(M3CommitFeedbackHandler)block;
@end

// ---------------------------------------------------------------------------
// 命令分配器：Metal 3 没有这个概念，命令缓冲从队列取
// Command allocator: no such concept in Metal 3; command buffers come from the queue.
// ---------------------------------------------------------------------------
@interface M3CommandAllocatorDescriptor : NSObject
@property (nonatomic, copy) NSString *label;
@end

@protocol M3CommandAllocator <NSObject> @end
@interface M3CommandAllocator : NSObject <M3CommandAllocator> @end

@interface M3CommandQueueDescriptor : NSObject
@property (nonatomic, strong) dispatch_queue_t feedbackQueue;
@end

// ---------------------------------------------------------------------------
// 着色器编译：MTL4Compiler → newLibraryWithSource:
// ---------------------------------------------------------------------------
@interface M3CompilerDescriptor : NSObject @end

@protocol M3Compiler <NSObject>
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)src options:(MTLCompileOptions *)opts error:(NSError **)err;
@end

@interface M3Compiler : NSObject <M3Compiler>
- (instancetype)initWithDevice:(id<MTLDevice>)dev;
@end

#endif /* DM_NVN_M3 */

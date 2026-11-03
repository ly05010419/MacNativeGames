// TOTK: runtime shader translation. A shader is identified by SHA-256(control section + code area) (nvn_mtl.m ctl_key_len: 0x880 of control, or its own smaller size); each
// translation ("variant") also records the translator answers it depended on (texture slot types, vertex
// input and colour output component types). A variant is reused only when those answers match the current
// bound state; otherwise a new one is produced by the translation service and cached on disk: the native program
// build/shader-service/totk_xlate --service when it is there (tools/totk_xlate/Service.cs, NativeAOT: no Python, no .NET
// runtime; 2026-10-17), else scripts/shader_service.py with the .NET translator. Same requests, replies and cache files.
#import <Foundation/Foundation.h>
#include "totk_ui_lang.h"
#include <TargetConditionals.h>
#include "totk_shader_rt.h"
#include "dmsh_chain.h"
#include "../../host/hle.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdatomic.h>
#include "totk_shader_seed.h"
#include "totk_shader_contrib.h"
extern void runtime_boot_status(const char *,const char *) __attribute__((weak_import));
extern void runtime_boot_count(const char *,uint64_t,uint64_t) __attribute__((weak_import));
static void seed_progress(int pass, NSUInteger done, NSUInteger total) {   // the bar under "Checking the shader cache"
    if (runtime_boot_count) runtime_boot_count(pass ? TOTK_UI("导入随 App 准备的着色器","Importing the app's shaders")
                                                    : TOTK_UI("校验随 App 准备的着色器","Verifying the app's shaders"), done, total);
}
int totk_depth_near_minus_w(void);   // hle_nvn.c: 1 when the game selected NVN_DEPTH_MODE_NEAR_IS_MINUS_W
uint64_t dm_nvn_frame_count(void);

extern char **environ;
static pthread_mutex_t g_rt_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSString *, NSMutableArray<NSDictionary *> *> *g_variants;
static NSMutableSet<NSString *> *g_msl_checked;
static FILE *g_svc_in, *g_svc_out;
static NSString *g_root, *g_dir;
static uint64_t g_translated;
/* The bottom-left box (nvn_mtl.m shader_overlay_prepare): variants new to the cache this run, when the last one came,
   and the variants in the cache (counted once in the background, plus the new ones since). */
static _Atomic uint64_t g_new_variants, g_new_variant_ns, g_new_at_count;
static _Atomic int64_t g_cached_variants = -1;

static NSString *root_dir(void) {   // native-port/ (the executable lives in native-port/build/)
    if (g_root) return g_root;
    char path[4096]; uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n)) die("executable path too long");
    g_root = [[[@(path) stringByResolvingSymlinksInPath] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    return g_root;
}
NSString *totk_rt_dir(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_SHADER_CACHE");
#if TARGET_OS_IPHONE
        g_dir = e ? @(e) : [NSString stringWithFormat:@"shader-cache/rt-%s", DMSH_CHAIN];   // in Documents (the app's working directory)
#else
        g_dir = e ? @(e) : [root_dir() stringByAppendingFormat:@"/build/shader-cache/rt-%s", DMSH_CHAIN];
#endif
        [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
        const char *seedMode = getenv("TOTK_SHADER_SEED");
        if (!(seedMode && !strcmp(seedMode, "off"))) {
            NSError *error = nil;
            if (runtime_boot_status) runtime_boot_status(TOTK_UI("检查着色器缓存","Checking the shader cache"),TOTK_UI("校验 App 附带的着色器，并按需导入本机缓存","Verifying the shaders bundled with the app and importing them into the local cache if needed"));
            const char *seedPath = getenv("TOTK_SHADER_SEED_DIR");
            NSString *source = seedPath ? @(seedPath) : [root_dir() stringByAppendingPathComponent:@"build/shader-seed"];
            totk_seed_progress = seed_progress;
            NSDictionary *seed = totk_seed_import(source, g_dir, &error);
            totk_seed_progress = NULL;
            if (!seed) {   // 2026-10-18: a damaged bundle or a full disk used to end the game here with nothing on screen. The
                           // cache stays usable (totk_seed_import writes shader files before the indexes that name them),
                           // so play on and translate shaders as they first appear, as before the prepared shaders existed.
                LOG("[shader-seed] 【红】导入 App 附带的着色器失败：%s；改为边玩边翻译",
                    error ? error.localizedDescription.UTF8String : "unknown error");
                if (runtime_boot_status) runtime_boot_status(TOTK_UI("着色器缓存未就绪","Shader cache not ready"),TOTK_UI("App 附带的着色器导入失败，改为边玩边翻译","Could not import the shaders bundled with the app; shaders are translated during play"));
                seed = @{@"available": @NO, @"failed": @YES};
            }
            if ([seed[@"available"] boolValue]) {
                unsigned long long imported=[seed[@"imported_files"] unsignedLongLongValue];
                LOG("[shader-seed] 发现随 App 准备的着色器 %llu 个，导入 %llu 个文件；首次启动即可预热",
                    [seed[@"shaders"] unsignedLongLongValue], imported);
                if (runtime_boot_status) runtime_boot_status(TOTK_UI("着色器缓存已就绪","Shader cache ready"),imported
                    ? [NSString stringWithFormat:@(TOTK_UI("已导入 %llu 个预置文件，开始预热","Imported %llu prepared files; starting warm-up")),imported].UTF8String
                    : TOTK_UI("本机缓存已存在，无需重新导入；开始预热","Local cache already present, nothing to import; starting warm-up"));
            } else if (runtime_boot_status && ![seed[@"failed"] boolValue]) runtime_boot_status(TOTK_UI("着色器缓存已就绪","Shader cache ready"),TOTK_UI("没有内置着色器种子，继续初始化图形","No bundled shader seed; continuing graphics setup"));
        }
#if TARGET_OS_IPHONE
        // Variants that ios/tools/fill_shaders.py translated on the Mac for this device's earlier misses (shader-requests/).
        NSDictionary *fill = totk_fill_import(@"shader-fill", g_dir);
        if ([fill[@"packs"] unsignedIntegerValue]) LOG("[shader-fill] 合并 Mac 补译的着色器包 %lu 个，导入 %lu 个文件",
            (unsigned long)[fill[@"packs"] unsignedIntegerValue], (unsigned long)[fill[@"imported_files"] unsignedIntegerValue]);
        for (NSString *why in fill[@"rejected"]) LOG("【红】[shader-fill] 丢弃不完整的着色器包 %s（请在 Mac 上重新运行 fill_shaders.py）", why.UTF8String);
#endif
    });
    return g_dir;
}

static void service_start(void) {
    if (g_svc_in) return;
    NSString *r = root_dir();
    int to[2], from[2];
    if (pipe(to) || pipe(from)) die("shader service pipes");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0); posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addclose(&fa, to[1]); posix_spawn_file_actions_addclose(&fa, from[0]);
    NSString *sc = [r stringByAppendingString:@"/build/spirv-cross/spirv-cross"];
    // TOTK_SHADER_XLATE=<totk_xlate.dll> runs a trial translator build (e.g. build/totk_xlate-opt) without replacing the
    // shared one (through shader_service.py); TOTK_SHADER_DEBUGMODE=0|1 reaches either service through the environment.
    // TOTK_SHADER_SERVICE=python | native picks the service (default: native when build/shader-service/totk_xlate is there).
    const char *xe = getenv("TOTK_SHADER_XLATE"), *want = getenv("TOTK_SHADER_SERVICE"), *dm = getenv("TOTK_SHADER_DEBUGMODE");
    NSString *native = [r stringByAppendingString:@"/build/shader-service/totk_xlate"];
    int have_native = access(native.fileSystemRepresentation, X_OK) == 0;
    int use_native = have_native && !(xe && *xe) && !(want && !strcmp(want, "python"));
    if (want && !strcmp(want, "native") && !use_native) die("TOTK_SHADER_SERVICE=native but %s is not there (make shader-service)", native.UTF8String);
    pid_t pid;
    if (use_native) {
        const char *argv[] = {native.UTF8String, "--service", "--spirv-cross", sc.UTF8String, "--out", totk_rt_dir().UTF8String,
                              "--chain", DMSH_CHAIN, NULL};
        if (posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, environ)) die("start shader service %s", argv[0]);
        if (dm) LOG("[shader] native translator, DebugMode %s", dm);
    } else {
        NSString *script = [r stringByAppendingString:@"/scripts/shader_service.py"], *dotnet = [r stringByAppendingString:@"/tooling/dotnet/dotnet"];
        NSString *xlate = xe && *xe ? @(xe) : [r stringByAppendingString:@"/build/totk_xlate/totk_xlate.dll"];
        const char *argv[] = {"python3", "-I", script.UTF8String, "--dotnet", dotnet.UTF8String, "--xlate", xlate.UTF8String,
                              "--spirv-cross", sc.UTF8String, "--out", totk_rt_dir().UTF8String, "--chain", DMSH_CHAIN, NULL};
        if (posix_spawnp(&pid, "python3", &fa, NULL, (char *const *)argv, environ)) die("start shader service");
        if (xe || dm) LOG("[shader] translator %s, DebugMode %s", xlate.UTF8String, dm ? dm : "default");
    }
    posix_spawn_file_actions_destroy(&fa);
    close(to[0]); close(from[1]);
    g_svc_in = fdopen(to[1], "w"); g_svc_out = fdopen(from[0], "r");
    if (!g_svc_in || !g_svc_out) die("shader service streams");
    LOG("[shader] runtime translation service started (pid %d, %s), cache %s", pid,
        use_native ? "native build/shader-service/totk_xlate" : "python3 scripts/shader_service.py + .NET", totk_rt_dir().UTF8String);
}

static NSString *hex(const uint8_t *p, size_t n) {
    NSMutableString *s = [NSMutableString stringWithCapacity:n * 2];
    for (size_t i = 0; i < n; i++) [s appendFormat:@"%02x", p[i]];
    return s;
}
static NSString *json_string(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingSortedKeys error:nil];
    if (!d) die("shader request JSON");
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}
static NSString *index_path(NSString *shader) { return [totk_rt_dir() stringByAppendingFormat:@"/%@.variants", shader]; }
static NSMutableArray *variants_of(NSString *shader) {
    if (!g_variants) g_variants = [NSMutableDictionary dictionary];
    NSMutableArray *v = g_variants[shader];
    if (v) return v;
    NSData *d = [NSData dataWithContentsOfFile:index_path(shader)];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    v = [NSMutableArray arrayWithArray:[a isKindOfClass:[NSArray class]] ? a : @[]];
    g_variants[shader] = v;
    return v;
}
static int deps_match(NSDictionary *deps, NSDictionary *ans) {
    { id w = deps[@"tfb"], h = ans[@"tfb"]; if ((w || h) && ![w isEqual:h]) return 0; }   // transform feedback layout: exact, or neither has one
    NSDictionary *defaults = @{@"tex": @"2d", @"img": @{}, @"attr": @"float", @"fout": @"float"};
    for (NSString *kind in defaults) {
        NSDictionary *want = deps[kind], *have = ans[kind];
        for (NSString *slot in want) {
            id w = want[slot], h = have[slot];
            if (!h && [kind isEqualToString:@"attr"]) continue;
            if (!h) h = defaults[kind];
            if ([w isEqual:@"*"] || [w isEqual:@"*_depth"]) {
                if (![h isKindOfClass:NSString.class] || [h hasSuffix:@"_depth"] != [w isEqual:@"*_depth"]) return 0;
                continue;
            }
            if (![h isEqual:w]) return 0;
        }
    }
    return 1;
}

/* No translator: the iPhone / iPad, or TOTK_SHADER_NO_TRANSLATE=1 on the Mac (tests: what a device with only the prepared
   shaders would skip, with the same miss records and skipped draws). */
static int no_translator(void) {
#if TARGET_OS_IPHONE
    return 1;
#else
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_SHADER_NO_TRANSLATE"); v = e && !strcmp(e, "1"); }
    return v;
#endif
}
int totk_rt_fix_bindless(NSString *base) {
    NSString *mp = [base stringByAppendingString:@".metal"], *jp = [base stringByAppendingString:@".json"];
    NSString *msl = [NSString stringWithContentsOfFile:mp encoding:NSUTF8StringEncoding error:nil];
    if (!msl || ![msl containsString:@"_cb0_"] || ![msl containsString:@"array<"]) return 0;
    NSData *jd = [NSData dataWithContentsOfFile:jp];
    NSMutableDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:NSJSONReadingMutableContainers error:nil] : nil;
    if (![j isKindOfClass:[NSMutableDictionary class]] || ![j[@"bindings"] isKindOfClass:[NSArray class]]) return 0;
    if (j[@"bindless_slots"]) {
        int changed = 0;
        for (NSMutableDictionary *b in j[@"bindings"]) {
            NSString *name = b[@"msl_name"], *type = b[@"msl_type"];
            if (![name containsString:@"_cb0_"] || ![name containsString:@"["]) continue;
            NSString *kind = [type isEqualToString:@"sampler"] ? @"samp" : [type hasPrefix:@"texture_buffer"] ? @"tbuf" : @"tex";
            if (![b[@"kind"] isEqual:kind]) { b[@"kind"] = kind; changed = 1; }
        }
        if (!changed) return 0;
        NSData *jo = [NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:nil];
        if (!jo || ![jo writeToFile:jp atomically:YES]) die("bindless fix: cannot write %s", jp.UTF8String);
        return 1;
    }
    NSRegularExpression *decl = [NSRegularExpression regularExpressionWithPattern:@"array<((?:texture\\w*<[^>]*>)|sampler), (\\d+)> ([vfc]p_(?:t|tb|s)_cb0_[0-9A-Fa-f]+) \\[\\[id\\((\\d+)\\)\\]\\];" options:0 error:nil];
    NSArray<NSTextCheckingResult *> *decls = [decl matchesInString:msl options:0 range:NSMakeRange(0, msl.length)];
    if (!decls.count) return 0;
    NSRegularExpression *rd = [NSRegularExpression regularExpressionWithPattern:@"[vfc]p_c2\\)\\._m0\\[(\\d+)\\]" options:0 error:nil];
    for (NSTextCheckingResult *r in [rd matchesInString:msl options:0 range:NSMakeRange(0, msl.length)]) {
        unsigned off = (unsigned)[[msl substringWithRange:[r rangeAtIndex:1]] intValue] * 16;
        if (off < 0x160) return 0;
    }
    NSMutableDictionary<NSString *, NSTextCheckingResult *> *arrays = [NSMutableDictionary dictionary];
    for (NSTextCheckingResult *r in decls) arrays[[msl substringWithRange:[r rangeAtIndex:3]]] = r;
    NSMutableArray *bindings = [NSMutableArray array]; int expanded = 0;
    for (NSMutableDictionary *b in j[@"bindings"]) {
        NSTextCheckingResult *r = arrays[b[@"msl_name"]];
        if (!r) { [bindings addObject:b]; continue; }
        NSString *type = [msl substringWithRange:[r rangeAtIndex:1]];
        int n = [[msl substringWithRange:[r rangeAtIndex:2]] intValue], id0 = [[msl substringWithRange:[r rangeAtIndex:4]] intValue];
        if (n <= 0 || n > 32 || [b[@"arg_id"] intValue] != id0) return 0;
        for (int i = 0; i < n; i++) {
            NSMutableDictionary *e = [b mutableCopy];
            e[@"msl_name"] = [NSString stringWithFormat:@"%@[%d]", b[@"msl_name"], i]; e[@"msl_type"] = type; e[@"arg_id"] = @(id0 + i); e[@"reg"] = @(i);
            e[@"kind"] = [type isEqualToString:@"sampler"] ? @"samp" : [type hasPrefix:@"texture_buffer"] ? @"tbuf" : @"tex";
            [bindings addObject:e];
        }
        expanded++;
    }
    if (expanded != (int)decls.count) return 0;
    j[@"bindings"] = bindings; j[@"bindless_slots"] = @YES;
    NSData *jo = [NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:nil];
    if (!jo || ![jo writeToFile:jp atomically:YES]) die("bindless fix: cannot write %s", jp.UTF8String);
    LOG("[shader] bindless arrays bound by slot (handles in c2 are slot numbers for this shader): %s", base.lastPathComponent.UTF8String);
    return 1;
}
static pthread_mutex_t g_svc_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t rt_queue(void) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("shader-translation", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0)); });
    return queue;
}
static dispatch_semaphore_t rt_slots(void) {
    static dispatch_semaphore_t slots; static dispatch_once_t once;
    dispatch_once(&once, ^{
        long n = (long)NSProcessInfo.processInfo.activeProcessorCount / 2;
        const char *e = getenv("XC3_SHADER_THREADS"); if (e && atoi(e) > 0) n = atoi(e);
        if (n < 2) n = 2;
        slots = dispatch_semaphore_create(n);
        LOG("[shader] background shader builds run %ld at a time", n);
    });
    return slots;
}
static NSMutableSet<NSString *> *g_rt_spec;
static pthread_cond_t g_rt_cond = PTHREAD_COND_INITIALIZER;
static int g_rt_async_allowed = 1;
void totk_rt_set_async_allowed(int allowed) { g_rt_async_allowed = allowed; }
static NSMutableSet<NSString *> *g_rt_pending;
static _Atomic uint64_t g_rt_ready_gen = 1;
static void (*g_rt_ready_hook)(NSString *variant, int stage);
void totk_rt_set_ready_hook(void (*hook)(NSString *variant, int stage)) { g_rt_ready_hook = hook; }
uint64_t totk_rt_ready_gen(void) { return atomic_load(&g_rt_ready_gen); }
int totk_rt_async_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_ASYNC_SHADERS");
        v = !(e && !strcmp(e, "0")) && !no_translator();
        LOG("[shader] new graphics shaders are %s", v ? "translated and compiled in the background; their draws are skipped until ready (TOTK_ASYNC_SHADERS=0 waits instead)" : "translated and compiled while the frame waits");
    }
    return v;
}
static NSString *rt_translate(NSDictionary *rq, NSString *shader, NSString *variant, NSString *stage_name, uint64_t started, int fatal) {
    BOOL existed = [NSFileManager.defaultManager fileExistsAtPath:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", variant]];
    pthread_mutex_lock(&g_svc_lock);
    service_start();
    fprintf(g_svc_in, "%s\n", json_string(rq).UTF8String); fflush(g_svc_in);
    char line[2048];
    if (!fgets(line, sizeof line, g_svc_out)) die("shader service exited while translating %s", variant.UTF8String);
    pthread_mutex_unlock(&g_svc_lock);
    NSDictionary *reply = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line length:strlen(line)] options:0 error:nil];
    if (![reply[@"ok"] boolValue] && !fatal) {
        LOG("[shader] early build of %s %s failed; it is translated when first used instead", stage_name.UTF8String, variant.UTF8String);
        return nil;
    }
    if (![reply[@"ok"] boolValue]) {
        LOG("[survey-shader] frame %llu translation FAILED %s %s after %.2f ms: %s",
            (unsigned long long)dm_nvn_frame_count(), stage_name.UTF8String, variant.UTF8String,
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6, [[reply[@"error"] description] UTF8String]);
        die("shader %s (%s) translation failed: %s", variant.UTF8String, stage_name.UTF8String, [[reply[@"error"] description] UTF8String]);
    }
    totk_rt_fix_bindless([totk_rt_dir() stringByAppendingFormat:@"/%@_b", variant]);
    NSData *jd = [NSData dataWithContentsOfFile:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.json", variant]];
    NSDictionary *refl = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![refl isKindOfClass:[NSDictionary class]] && !fatal) return nil;
    if (![refl isKindOfClass:[NSDictionary class]]) die("shader %s: reflection missing", variant.UTF8String);
    pthread_mutex_lock(&g_rt_lock);
    NSMutableArray *vs = variants_of(shader);
    [vs addObject:@{@"variant": variant, @"deps": refl[@"deps"] ?: @{}}];
    if (!g_msl_checked) g_msl_checked = [NSMutableSet set];
    [g_msl_checked addObject:variant];
    [[json_string(vs) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:index_path(shader) atomically:YES];
    g_translated++;
    uint64_t translated = g_translated;
    if (!existed) totk_contrib_add(shader, refl[@"deps"] ?: @{});
    pthread_mutex_unlock(&g_rt_lock);
    if (!existed) { atomic_fetch_add(&g_new_variants, 1); atomic_store(&g_new_variant_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }
    if (getenv("TOTK_SURVEY_SHADER_LOG")) LOG("[survey-shader] frame %llu translated %s %s in %.2f ms%s",
        (unsigned long long)dm_nvn_frame_count(), stage_name.UTF8String, variant.UTF8String,
        (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6,
        [refl[@"softfloat64"] boolValue] ? " (FP64 fallback)" : "");
    if (translated <= 8 || translated % 100 == 0) LOG("[shader] translated %llu: %s %s", (unsigned long long)translated, stage_name.UTF8String, variant.UTF8String);
    return variant;
}
static NSMutableDictionary *rt_request(NSString *shader, NSString *variant, NSString *stage_name, int stage, const uint8_t *control, const uint8_t *code, uint32_t c1o, uint32_t c1t, NSDictionary *answers, NSArray *local) {
    NSString *code_path = [totk_rt_dir() stringByAppendingFormat:@"/%@.code", variant];
    size_t skip = stage == 5 ? 0 : 0x30;
    if (![[NSData dataWithBytes:code + skip length:c1t - skip] writeToFile:code_path atomically:YES]) die("write shader code %s", code_path.UTF8String);
    NSMutableDictionary *rq = [NSMutableDictionary dictionaryWithDictionary:@{@"variant": variant, @"stage": stage_name, @"code": code_path, @"c1": hex(code + c1o, c1t - c1o)}];
    [rq addEntriesFromDictionary:answers];
    if (local) rq[@"local"] = local;
    if (stage == 5) { uint32_t shared; memcpy(&shared, control + 0x754, 4); if (shared) rq[@"shared"] = @(shared); }
    { uint32_t size; memcpy(&size, control + 0x20, 4); if (stage == 1 && !(size >= 0x7cc && size < 0x880) && (control[0x718] & 1)) rq[@"early_fragment_tests"] = @YES; }
    rq[@"depth_minus_w"] = totk_depth_near_minus_w() ? @YES : @NO;
    return rq;
}
static void rt_background(NSDictionary *job, NSString *shader, NSString *variant, NSString *stage_name, int stage, uint64_t started, int spec) {
    dispatch_async(rt_queue(), ^{ @autoreleasepool {
        dispatch_semaphore_wait(rt_slots(), DISPATCH_TIME_FOREVER);
        NSString *done = rt_translate(job, shader, variant, stage_name, started, !spec);
        if (done && g_rt_ready_hook) g_rt_ready_hook(variant, stage);
        dispatch_semaphore_signal(rt_slots());
        pthread_mutex_lock(&g_rt_lock);
        [g_rt_pending removeObject:variant];
        if (spec) [g_rt_spec removeObject:shader];
        pthread_cond_broadcast(&g_rt_cond);
        pthread_mutex_unlock(&g_rt_lock);
        atomic_fetch_add(&g_rt_ready_gen, 1);
    } });
}
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local) {
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_rt_lock);
    NSMutableArray *vs = variants_of(shader);
    for (NSUInteger i = 0; i < vs.count; i++) {
        NSDictionary *v = vs[i];
        if (!deps_match(v[@"deps"], answers)) continue;
        NSString *found = v[@"variant"];
        if ([g_rt_pending containsObject:found]) {
            if (g_rt_async_allowed) { pthread_mutex_unlock(&g_rt_lock); return nil; }
            while ([g_rt_pending containsObject:found]) pthread_cond_wait(&g_rt_cond, &g_rt_lock);
            pthread_mutex_unlock(&g_rt_lock);
            return totk_rt_variant(shader, stage, control, code, answers, local);
        }
        if (!g_msl_checked) g_msl_checked = [NSMutableSet set];
        BOOL checked = [g_msl_checked containsObject:found];
        NSString *msl = checked ? nil :
            [NSString stringWithContentsOfFile:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", found]
                                             encoding:NSUTF8StringEncoding error:nil];
        if (!checked && (!msl || [msl containsString:@"double "] || [msl containsString:@"unsupported_GLSLstd450"])) {
            [vs removeObjectAtIndex:i];
            break;
        }
        [g_msl_checked addObject:found];
        pthread_mutex_unlock(&g_rt_lock);
        return found;
    }
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) die("shader %s: code area %#x / c1 %#x", shader.UTF8String, c1t, c1o);
    NSString *stage_name = stage == 0 ? @"vs" : stage == 1 ? @"ps" : stage == 5 ? @"cs" : nil;
    if (!stage_name) die("shader %s: NVN stage %d has no translation chain (tessellation/geometry)", shader.UTF8String, stage);
    uint8_t md[32]; NSData *ad = [json_string(answers) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@", shader, hex(md, 6)];
    if ([g_rt_pending containsObject:variant]) {
        if (g_rt_async_allowed) { pthread_mutex_unlock(&g_rt_lock); return nil; }
        while ([g_rt_pending containsObject:variant]) pthread_cond_wait(&g_rt_cond, &g_rt_lock);
        pthread_mutex_unlock(&g_rt_lock);
        return totk_rt_variant(shader, stage, control, code, answers, local);
    }
    if ([g_rt_spec containsObject:shader]) {
        if (g_rt_async_allowed) { pthread_mutex_unlock(&g_rt_lock); return nil; }
        while ([g_rt_spec containsObject:shader]) pthread_cond_wait(&g_rt_cond, &g_rt_lock);
        pthread_mutex_unlock(&g_rt_lock);
        return totk_rt_variant(shader, stage, control, code, answers, local);
    }
    size_t skip = stage == 5 ? 0 : 0x30;
    NSMutableDictionary *rq = rt_request(shader, variant, stage_name, stage, control, code, c1o, c1t, answers, local);
    if (no_translator()) {
        rq[@"code_hex"] = hex(code + skip, c1t - skip);
        [NSFileManager.defaultManager createDirectoryAtPath:@"shader-requests" withIntermediateDirectories:YES attributes:nil error:nil];
        static NSMutableSet<NSString *> *missed; if (!missed) missed = [NSMutableSet set];
        if (![missed containsObject:variant]) {
            [missed addObject:variant];
            [[json_string(rq) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[NSString stringWithFormat:@"shader-requests/%@.json", variant] atomically:YES];
            totk_contrib_add(shader, answers);
            LOG("[shader-miss] %s %s is not in this device's translation cache: draws that need it are skipped; request saved in shader-requests/ (%lu so far)",
                stage_name.UTF8String, variant.UTF8String, (unsigned long)missed.count);
        }
        pthread_mutex_unlock(&g_rt_lock);
        return nil;
    }
    if (stage != 5 && g_rt_async_allowed && totk_rt_async_on()) {
        if (!g_rt_pending) g_rt_pending = [NSMutableSet set];
        [g_rt_pending addObject:variant];
        pthread_mutex_unlock(&g_rt_lock);
        rt_background([rq copy], shader, variant, stage_name, stage, started, 0);
        return nil;
    }
    pthread_mutex_unlock(&g_rt_lock);
    return rt_translate(rq, shader, variant, stage_name, started, 1);
}
/* Geometry shaders as compute (Xenoblade Chronicles 3; games/xc3/docs/reference/nvn-geometry-shaders.md): one of the three
   translations of a program with a geometry stage, "vsc" (its vertex shader as a kernel), "gsc" (its geometry shader as a
   kernel) or "vpt" (the vertex shader drawing the geometry output), for one input topology. Translated synchronously the
   first time; the variant name hashes the part, the topology and the answers, so an existing file is the right one. */
NSString *totk_rt_vtg(NSString *gs_shader, NSString *part, const uint8_t *vs_ctl, const uint8_t *vs_code, const uint8_t *gs_ctl, const uint8_t *gs_code,
                      NSString *topology, NSDictionary *answers) {
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint8_t md[32]; NSData *ad = [json_string(@{@"a": answers ?: @{}, @"part": part, @"topology": topology}) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@-%@", gs_shader, part, hex(md, 6)], *dir = totk_rt_dir();
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:[dir stringByAppendingFormat:@"/%@_b.json", variant]] && [fm fileExistsAtPath:[dir stringByAppendingFormat:@"/%@_b.metal", variant]]) return variant;
    if (no_translator()) { LOG("[shader-miss] geometry part %s %s is not in the translation cache", part.UTF8String, variant.UTF8String); return nil; }
    int vs = [part isEqualToString:@"vsc"];
    const uint8_t *ctl = vs ? vs_ctl : gs_ctl, *code = vs ? vs_code : gs_code;
    uint32_t c1o, c1t; memcpy(&c1o, ctl + 0x700, 4); memcpy(&c1t, ctl + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || c1o < 0x30) die("geometry part %s: code area %#x / c1 %#x", variant.UTF8String, c1t, c1o);
    NSString *code_path = [dir stringByAppendingFormat:@"/%@.code", variant];
    if (![[NSData dataWithBytes:code + 0x30 length:c1t - 0x30] writeToFile:code_path atomically:YES]) die("write shader code %s", code_path.UTF8String);
    NSMutableDictionary *rq = [NSMutableDictionary dictionaryWithDictionary:@{@"variant": variant, @"stage": part, @"code": code_path, @"c1": hex(code + c1o, c1t - c1o), @"topology": topology}];
    [rq addEntriesFromDictionary:answers ?: @{}];
    if (!vs) {
        uint32_t v1o, v1t; memcpy(&v1o, vs_ctl + 0x700, 4); memcpy(&v1t, vs_ctl + 0x704, 4);
        if (!v1t || v1t > 16u << 20 || v1o > v1t || v1o < 0x30) die("geometry part %s: vertex code area %#x / c1 %#x", variant.UTF8String, v1t, v1o);
        NSString *vs_path = [dir stringByAppendingFormat:@"/%@.vs.code", variant];
        if (![[NSData dataWithBytes:vs_code + 0x30 length:v1t - 0x30] writeToFile:vs_path atomically:YES]) die("write shader code %s", vs_path.UTF8String);
        rq[@"vs_code"] = vs_path;
        rq[@"vs_c1"] = hex(vs_code + v1o, v1t - v1o);
    }
    return rt_translate(rq, gs_shader, variant, part, started, 1);
}
void totk_rt_prefetch_answers(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local, NSDictionary *answers) {
    if (no_translator() || !totk_rt_async_on()) return;
    NSString *stage_name = stage == 0 ? @"vs" : stage == 1 ? @"ps" : stage == 5 ? @"cs" : nil;
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!stage_name || !c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) return;
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_rt_lock);
    if ([g_rt_spec containsObject:shader]) { pthread_mutex_unlock(&g_rt_lock); return; }
    for (NSDictionary *v in variants_of(shader)) if (deps_match(v[@"deps"], answers)) { pthread_mutex_unlock(&g_rt_lock); return; }
    uint8_t md[32]; NSData *ad = [json_string(answers) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@", shader, hex(md, 6)];
    if ([g_rt_pending containsObject:variant]) { pthread_mutex_unlock(&g_rt_lock); return; }
    NSMutableDictionary *rq = rt_request(shader, variant, stage_name, stage, control, code, c1o, c1t, answers, local);
    if (!g_rt_pending) g_rt_pending = [NSMutableSet set];
    if (!g_rt_spec) g_rt_spec = [NSMutableSet set];
    [g_rt_pending addObject:variant];
    [g_rt_spec addObject:shader];
    pthread_mutex_unlock(&g_rt_lock);
    rt_background([rq copy], shader, variant, stage_name, stage, started, 1);
}
void totk_rt_prefetch(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSArray *local) {
    if (no_translator() || !totk_rt_async_on()) return;
    NSString *stage_name = stage == 0 ? @"vs" : stage == 1 ? @"ps" : stage == 5 ? @"cs" : nil;
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!stage_name || !c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) return;
    static NSDictionary *answers;
    if (!answers) {
        NSMutableDictionary *tex = [NSMutableDictionary dictionary];
        for (int r = 0; r < 32; r++) tex[@(r).stringValue] = @"buffer";
        answers = @{@"tex": tex, @"img": @{}, @"attr": @{}, @"fout": @{}};
    }
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_rt_lock);
    if (variants_of(shader).count || [g_rt_spec containsObject:shader]) { pthread_mutex_unlock(&g_rt_lock); return; }
    uint8_t md[32]; NSData *ad = [json_string(answers) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@", shader, hex(md, 6)];
    if ([g_rt_pending containsObject:variant]) { pthread_mutex_unlock(&g_rt_lock); return; }
    NSMutableDictionary *rq = rt_request(shader, variant, stage_name, stage, control, code, c1o, c1t, answers, local);
    if (!g_rt_pending) g_rt_pending = [NSMutableSet set];
    if (!g_rt_spec) g_rt_spec = [NSMutableSet set];
    [g_rt_pending addObject:variant];
    [g_rt_spec addObject:shader];
    pthread_mutex_unlock(&g_rt_lock);
    rt_background([rq copy], shader, variant, stage_name, stage, started, 1);
}

void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *dir = totk_rt_dir();
        atomic_store(&g_new_at_count, atomic_load(&g_new_variants));
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{   // ~45,000 directory entries: not on the present thread
            int64_t n = 0; DIR *d = opendir(dir.fileSystemRepresentation); struct dirent *e;
            if (d) { while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 8 && !strcmp(e->d_name + l - 8, "_b.metal")) n++; } closedir(d); }
            atomic_store(&g_cached_variants, n);
        });
    });
    uint64_t f = atomic_load(&g_new_variants); int64_t t = atomic_load(&g_cached_variants);
    if (fresh) *fresh = f;
    if (total) *total = t < 0 ? -1 : t + (int64_t)(f - atomic_load(&g_new_at_count));
    if (last_new_ns) *last_new_ns = atomic_load(&g_new_variant_ns);
}

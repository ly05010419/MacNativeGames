// TOTK: runtime shader translation. A shader is identified by SHA-256(control section + code area) (nvn_mtl.m ctl_key_len: 0x880 of control, or its own smaller size); each
// translation ("variant") also records the translator answers it depended on (texture slot types, vertex
// input and colour output component types). A variant is reused only when those answers match the current
// bound state; otherwise a new one is produced by the translation service and cached on disk: the native program
// build/shader-service/totk_xlate --service when it is there (tools/totk_xlate/Service.cs, NativeAOT: no Python, no .NET
// runtime; 2026-10-17), else scripts/shader_service.py with the .NET translator. Same requests, replies and cache files.
#import <Foundation/Foundation.h>
#include "compute_shared.h"
#include "totk_ui_lang.h"
#include <TargetConditionals.h>
#include "totk_shader_rt.h"
#include "dmsh_chain.h"
#include "../../host/hle.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdatomic.h>
#include "totk_shader_seed.h"
#include "totk_shader_observed.h"
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
static NSMutableSet<NSString *> *g_failed_variants; // process-local, retry on next launch
typedef struct { FILE *in, *out; } rt_svc;   // one translator service process
static NSString *g_root, *g_dir;
/* iOS: the seed inside the signed app is read where it is instead of being copied into the cache (totk_shader_seed.h
   totk_seed_sealed). Its files are taken from there; the cache holds the rest (fill packs, pso_collected.tsv), and a
   shader's .variants lists of both places are joined. nil on the Mac, where the seed is cloned into the cache. */
static NSString *g_seed_dir;
static NSSet<NSString *> *g_seed_names;
static NSMutableSet<NSString *> *g_cache_names;
static pthread_mutex_t g_names_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL g_seed_all_default;   // seed-manifest.json all_default: every shader has its default variant (contrib_wanted)
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
static NSString *seed_dir(void) {   // the prepared startup shaders that come with the app (build_app.py)
    const char *seedPath = getenv("TOTK_SHADER_SEED_DIR");
    return seedPath ? @(seedPath) : [root_dir() stringByAppendingPathComponent:@"build/shader-seed"];
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
            NSString *source = seed_dir();
            totk_seed_progress = seed_progress;
            NSDictionary *seed = totk_seed_import(source, g_dir, &error);
            totk_seed_progress = NULL;
            if ([seed[@"overlay"] isKindOfClass:NSArray.class]) { g_seed_dir = source; g_seed_names = [NSSet setWithArray:seed[@"overlay"]]; }
            if ([seed[@"available"] boolValue]) {
                NSData *md = [NSData dataWithContentsOfFile:[source stringByAppendingPathComponent:@"seed-manifest.json"]];
                NSDictionary *m = md ? [NSJSONSerialization JSONObjectWithData:md options:0 error:nil] : nil;
                g_seed_all_default = [m isKindOfClass:NSDictionary.class] && [m[@"all_default"] isEqual:@YES];
            }
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
                LOG("[shader-seed] 发现随 App 准备的着色器 %llu 个，导入 %llu 个文件%s%s；首次启动即可预热",
                    [seed[@"shaders"] unsignedLongLongValue], imported,
                    [seed[@"cloned_folder"] boolValue] ? "（整个文件夹一次克隆）" : "",
                    g_seed_dir ? "（App 包已签名，原地读取，不校验也不复制）" : "");
                if ([seed[@"folder_skipped"] length]) LOG("[shader-seed] 缓存里已有文件（例如 %s），逐个导入", [seed[@"folder_skipped"] UTF8String]);
                if ([seed[@"clone_errno"] intValue]) LOG("[shader-seed] 【红】克隆失败（%s），改为复制文件", strerror([seed[@"clone_errno"] intValue]));
                if (runtime_boot_status) runtime_boot_status(TOTK_UI("着色器缓存已就绪","Shader cache ready"),imported
                    ? [NSString stringWithFormat:@(TOTK_UI("已导入 %llu 个预置文件，开始预热","Imported %llu prepared files; starting warm-up")),imported].UTF8String
                    : g_seed_dir ? TOTK_UI("直接读取 App 附带的着色器，无需导入；开始预热","Reading the shaders bundled with the app in place, nothing to import; starting warm-up")
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
        NSArray<NSString *> *init_cached = [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_dir error:nil] ?: @[];
        pthread_mutex_lock(&g_names_lock);
        g_cache_names = [NSMutableSet setWithArray:init_cached];
        pthread_mutex_unlock(&g_names_lock);
    });
    return g_dir;
}

/* PR #84 (part A): the seed's shader-keys.json names every shader of this game version; the background warm-up only
   warms cache entries of those shaders (and of shaders this Mac has seen the game use), not other versions' leftovers in
   the same cache. Read once from the seed folder, checked against seed-manifest.json; not in use on iOS (its warm-up and
   memory were not measured with it). nil = no valid scope: warm everything, as before. */
static NSDictionary *shader_scope(void) {
#if TARGET_OS_IPHONE
    return nil;
#else
    static NSDictionary *scope; static dispatch_once_t once;
    dispatch_once(&once, ^{ scope = totk_shader_scope_metadata(seed_dir(), @DMSH_CHAIN); });
    return scope;
#endif
}
static NSMutableDictionary *observed_keys_state(void) {
    static NSMutableDictionary *state; static dispatch_once_t once;
    // Finish cache/seed initialization before entering this once token. Keeping
    // this outside totk_rt_dir's token prevents recursive initialization.
    NSString *cache = totk_rt_dir();
    dispatch_once(&once, ^{ state = totk_shader_observed_open(shader_scope(), cache, @DMSH_CHAIN); });
    return state;
}

NSSet<NSString *> *totk_rt_observed_shader_keys(void) {
    return totk_shader_observed_keys(observed_keys_state());
}

NSSet<NSString *> *totk_rt_shader_scope(void) {
    const char *all = getenv("TOTK_WARM_ALL");
    NSDictionary *scope = shader_scope();
    if (!scope || (all && atoi(all))) return nil;
    return [[NSSet setWithArray:scope[@"keys"]] setByAddingObjectsFromSet:totk_rt_observed_shader_keys()];
}

/* Several services can start at once (async shaders): one at a time, and each child keeps only its own stdin / stdout and
   the shared stderr (POSIX_SPAWN_CLOEXEC_DEFAULT). A child holding another's pipe ends would keep that service alive after
   the game exits and hide its crash from the reader (fgets would wait instead of seeing EOF). */
static pthread_mutex_t g_svc_start_lock = PTHREAD_MUTEX_INITIALIZER;
static void service_start(rt_svc *svc) {
    pthread_mutex_lock(&g_svc_start_lock);
    /* Prestart and the first real job may meet. Publish/check the streams under
       the same lock; never spawn twice or read a partially initialized svc. */
    if (svc->in) { pthread_mutex_unlock(&g_svc_start_lock); return; }
    NSString *r = root_dir();
    int to[2], from[2];
    if (pipe(to) || pipe(from)) die("shader service pipes");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0); posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_adddup2(&fa, 2, 2);   // the same descriptor: only clears close-on-exec
    /* An empty signal mask: a child inherits the spawning thread's, and a GCD worker (bg_translate) blocks SIGCHLD among
       others. The .NET translator then never learns that its spirv-cross child exited and waits forever (2026-10-21). */
    posix_spawnattr_t sa; posix_spawnattr_init(&sa); sigset_t none; sigemptyset(&none); posix_spawnattr_setsigmask(&sa, &none);
    posix_spawnattr_setflags(&sa, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK);
    NSString *sc = [r stringByAppendingString:@"/build/spirv-cross/spirv-cross"];
    const char *xe = getenv("TOTK_SHADER_XLATE"), *want = getenv("TOTK_SHADER_SERVICE"), *dm = getenv("TOTK_SHADER_DEBUGMODE");
    if (want && *want && strcmp(want, "native")) die("TOTK_SHADER_SERVICE=%s: only the native translator exists (the Python service was removed); unset it", want);
    NSString *native = xe && *xe ? @(xe) : [r stringByAppendingString:@"/build/shader-service/totk_xlate"];
    if (access(native.fileSystemRepresentation, X_OK)) die("shader translator %s is missing (make shader-service, or build the app again)", native.UTF8String);
    pid_t pid;
    const char *argv[] = {native.UTF8String, "--service", "--spirv-cross", sc.UTF8String, "--out", totk_rt_dir().UTF8String,
                          "--chain", DMSH_CHAIN, NULL};
    if (posix_spawn(&pid, argv[0], &fa, &sa, (char *const *)argv, environ)) die("start shader service %s", argv[0]);
    if (dm) LOG("[shader] native translator, DebugMode %s", dm);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&sa);
    close(to[0]); close(from[1]);
    svc->in = fdopen(to[1], "w"); svc->out = fdopen(from[0], "r");
    if (!svc->in || !svc->out) die("shader service streams");
    LOG("[shader] runtime translation service started (pid %d, native), cache %s", pid, totk_rt_dir().UTF8String);
    pthread_mutex_unlock(&g_svc_start_lock);
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
NSString *totk_rt_file(NSString *name) {
    NSString *dir = totk_rt_dir();
    return g_seed_dir && [g_seed_names containsObject:name] ? [g_seed_dir stringByAppendingPathComponent:name]
                                                             : [dir stringByAppendingPathComponent:name];
}
int totk_rt_exists(NSString *name) {
    if (!name.length) return 0;
    totk_rt_dir();
    if (g_seed_dir && [g_seed_names containsObject:name]) return 1;
    pthread_mutex_lock(&g_names_lock);
    int has = g_cache_names && [g_cache_names containsObject:name];
    pthread_mutex_unlock(&g_names_lock);
    return has;
}
NSArray<NSString *> *totk_rt_list(void) {
    totk_rt_dir();
    pthread_mutex_lock(&g_names_lock);
    NSArray<NSString *> *cached = g_cache_names ? [g_cache_names allObjects] : @[];
    pthread_mutex_unlock(&g_names_lock);
    if (!g_seed_dir) return [cached sortedArrayUsingSelector:@selector(compare:)];
    NSMutableSet<NSString *> *all = [g_seed_names mutableCopy]; [all addObjectsFromArray:cached];
    return [all.allObjects sortedArrayUsingSelector:@selector(compare:)];
}
static NSString *index_path(NSString *shader) { return [totk_rt_dir() stringByAppendingFormat:@"/%@.variants", shader]; }
static NSMutableArray *variants_of(NSString *shader);
static NSArray *variants_file(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    return [a isKindOfClass:[NSArray class]] ? a : @[];
}
/* Legacy pool arrays were clamped to 31 and had no length dependencies. Never
   promote them to current variants just because their scalar dependencies match.
   This checks reflection provenance, not a rewrite of cached MSL or metadata. */
static int pool_array_cache_valid(NSDictionary *refl) {
    if (![refl isKindOfClass:NSDictionary.class] || ![refl[@"bindings"] isKindOfClass:NSArray.class]) return 0;
    id deps = refl[@"deps"];
    if (deps && ![deps isKindOfClass:NSDictionary.class]) return 0;
    id lengths = deps[@"array_lengths"];
    for (id row in refl[@"bindings"]) {
        if (![row isKindOfClass:NSDictionary.class]) return 0;
        NSString *type = [row[@"msl_type"] isKindOfClass:NSString.class] ? row[@"msl_type"] : @"";
        NSString *kind = [row[@"kind"] isKindOfClass:NSString.class] ? row[@"kind"] : @"";
        if (![type hasPrefix:@"array<"] && !row[@"array_count"] && ![kind hasPrefix:@"pool_"]) continue;
        NSString *domain = ([kind isEqual:@"pool_tbuf"] || [kind isEqual:@"pool_tex"]) ? @"texture_pool" : [kind isEqual:@"pool_samp"] ? @"sampler_pool" : nil;
        if (!domain || ![row[@"index_domain"] isEqual:domain] || ![lengths isKindOfClass:NSDictionary.class]) return 0;
        id length = lengths[domain], count = row[@"array_count"];
        long long limit = [domain isEqual:@"sampler_pool"] ? 1LL << 12 : 1LL << 20;
        if (![length isKindOfClass:NSNumber.class] || ![count isKindOfClass:NSNumber.class]) return 0;
        if (CFGetTypeID((__bridge CFTypeRef)length) == CFBooleanGetTypeID() ||
            CFGetTypeID((__bridge CFTypeRef)count) == CFBooleanGetTypeID()) return 0;
        long long n = [length longLongValue], c = [count longLongValue];
        if (n < 1 || n > limit || [length doubleValue] != (double)n ||
            [count doubleValue] != (double)c || c != MAX(2, n)) return 0;
        NSString *pattern = [kind isEqual:@"pool_samp"] ? @"^array<sampler,\\s*([0-9]+)>$" :
            [kind isEqual:@"pool_tex"] ? @"^array<texture2d<(?:float|uint|int)>,\\s*([0-9]+)>$" :
            @"^array<texture_buffer<(?:float|uint|int)>,\\s*([0-9]+)>$";
        // Use a literal backslash in the regular expression, not an ObjC escape.
        NSRegularExpression *rx = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:nil];
        NSTextCheckingResult *match = [rx firstMatchInString:type options:0 range:NSMakeRange(0, type.length)];
        if (!match || [[type substringWithRange:[match rangeAtIndex:1]] longLongValue] != c) return 0;
    }
    return 1;
}
static NSMutableArray *variants_load_unlocked(NSString *shader) {
    NSString *seedIndex = [shader stringByAppendingString:@".variants"];
    NSMutableArray *v = [NSMutableArray array];
    pthread_mutex_lock(&g_names_lock);
    BOOL hasLocal = g_cache_names && [g_cache_names containsObject:seedIndex];
    pthread_mutex_unlock(&g_names_lock);
    if (hasLocal) [v addObjectsFromArray:variants_file(index_path(shader))];
    if (g_seed_dir && [g_seed_names containsObject:seedIndex]) {   // the cache's own rows (fill packs) first, then the seed's
        NSMutableSet *known = [NSMutableSet set];
        for (NSDictionary *row in v) if ([row isKindOfClass:NSDictionary.class] && row[@"variant"]) [known addObject:row[@"variant"]];
        for (NSDictionary *row in variants_file([g_seed_dir stringByAppendingPathComponent:seedIndex]))
            if ([row isKindOfClass:NSDictionary.class] && row[@"variant"] && ![known containsObject:row[@"variant"]]) [v addObject:row];
    }
    // Validate once per shader when loading its index, before authoritative or
    // prediction lookups can use it. Do not mutate files or trust cache filenames.
    NSMutableArray *safe = [NSMutableArray array];
    for (id row in v) {
        if (![row isKindOfClass:NSDictionary.class] || ![row[@"variant"] isKindOfClass:NSString.class]) continue;
        NSString *name = [row[@"variant"] stringByAppendingString:@"_b.json"];
        NSData *data = [NSData dataWithContentsOfFile:totk_rt_file(name)];
        NSDictionary *refl = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (!pool_array_cache_valid(refl)) continue;
        id indexed = row[@"deps"];
        if (![indexed isKindOfClass:NSDictionary.class]) continue;
        id recorded_lengths = refl[@"deps"][@"array_lengths"], indexed_lengths = indexed[@"array_lengths"];
        if ((recorded_lengths || indexed_lengths) && ![recorded_lengths isEqual:indexed_lengths]) continue;
        [safe addObject:row];
    }
    return safe;
}
static NSMutableArray *variants_of(NSString *shader) {
    if (!g_variants) g_variants = [NSMutableDictionary dictionary];
    NSMutableArray *v = g_variants[shader];
    if (v) return v;
    v = variants_load_unlocked(shader);
    g_variants[shader] = v;
    return v;
}
/* Whether a variant translated with deps serves these answers. A texture slot whose kind the translator took from the
   sampling instruction records only whether a depth-format texture sits there: "*" (any other) or "*_depth"
   (Service.cs, docs/shader-variants.md). A vertex input the answers leave out is unbound: the replay feeds it a constant
   of the declared type (nvn_mtl.m pso_desc), so any variant serves it. prepare_shader_seed.py deps_match is the same. */
static int deps_match(NSDictionary *deps, NSDictionary *ans) {
    // Pool lengths specialize the translated array bounds. Only queried domains
    // are dependencies; a different/missing length must never reuse old code.
    if (![deps isKindOfClass:NSDictionary.class] || ![ans isKindOfClass:NSDictionary.class]) return 0;
    // Symmetric check also rejects legacy compute variants lacking this metadata.
    // Graphics answers have no shared-memory field and remain unaffected.
    if (deps[@"shared"] || ans[@"shared"]) {
        id want = deps[@"shared"], have = ans[@"shared"];
        if (![want isKindOfClass:NSNumber.class] || ![have isKindOfClass:NSNumber.class] ||
            ![want isEqual:have]) return 0;
    }
    id lengths = deps[@"array_lengths"], answers = ans[@"array_lengths"];
    if (lengths && ![lengths isKindOfClass:NSDictionary.class]) return 0;
    if ([lengths count] && ![answers isKindOfClass:NSDictionary.class]) return 0;
    for (NSString *domain in lengths) {
        if (![domain isKindOfClass:NSString.class]) return 0;
        id want = lengths[domain], have = answers[domain];
        long long limit = [domain isEqual:@"sampler_pool"] ? 1LL << 12 : 1LL << 20;
        for (id value in @[want ?: NSNull.null, have ?: NSNull.null]) {
            if (![value isKindOfClass:NSNumber.class] ||
                CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return 0;
            long long n = [value longLongValue];
            if (n < 1 || n > limit || [value doubleValue] != (double)n) return 0;
        }
        if (![want isEqual:have]) return 0;
    }
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
int totk_rt_can_translate(void) { return !no_translator(); }

/* The cached variant whose deps match the answers, or nil (g_rt_lock held). A variant whose Metal source is missing or
   could not be compiled before is dropped from the index, so it is translated again. */
static NSString *variant_lookup(NSString *shader, NSDictionary *answers, uint32_t need_shared) {
    NSMutableArray *vs = variants_of(shader);
    NSDictionary *ans_img = [answers[@"img"] isKindOfClass:NSDictionary.class] ? answers[@"img"] : nil;
    for (NSUInteger i = 0; i < vs.count; i++) {
        NSDictionary *v = vs[i];
        if (!deps_match(v[@"deps"], answers)) continue;
        NSString *found = v[@"variant"];
        if (!g_msl_checked) g_msl_checked = [NSMutableSet set];
        NSString *mname = [found stringByAppendingString:@"_b.metal"];
        if (!totk_rt_exists(mname)) {
            [vs removeObjectAtIndex:i];
            return nil;
        }
        BOOL checked = [g_msl_checked containsObject:found] || (!need_shared && ([g_seed_names containsObject:mname] || [g_cache_names containsObject:mname]));
        NSString *msl = checked ? nil :
            [NSString stringWithContentsOfFile:totk_rt_file(mname)
                                             encoding:NSUTF8StringEncoding error:nil];
        BOOL stale_img = NO;
        if (!checked && msl && ans_img.count) {
            NSDictionary *dep_img = [v[@"deps"][@"img"] isKindOfClass:NSDictionary.class] ? v[@"deps"][@"img"] : @{};
            for (NSString *slot in ans_img) {
                NSDictionary *im = [ans_img[slot] isKindOfClass:NSDictionary.class] ? ans_img[slot] : nil;
                NSString *fmt = [im[@"fmt"] isKindOfClass:NSString.class] ? im[@"fmt"] : nil;
                int s = slot.intValue;
                if (s >= 0 && s < 8 && fmt && ([fmt hasSuffix:@"ui"] || [fmt hasSuffix:@"i"]) && !dep_img[slot]) {
                    NSString *sfx = [NSString stringWithFormat:@"_i_cb2_%02X_", 0x48 + s * 2];
                    if ([msl containsString:[@"texture_buffer<float, access::write> cp" stringByAppendingString:sfx]] ||
                        [msl containsString:[@"texture2d<float, access::write> cp" stringByAppendingString:sfx]] ||
                        [msl containsString:[@"texture3d<float, access::write> cp" stringByAppendingString:sfx]] ||
                        [msl containsString:[@"texture_buffer<float, access::write> fp" stringByAppendingString:sfx]] ||
                        [msl containsString:[@"texture_buffer<float, access::write> vp" stringByAppendingString:sfx]]) {
                        stale_img = YES;
                        break;
                    }
                }
            }
        }
        if (!checked && (!msl || stale_img || [msl containsString:@"double "] || [msl containsString:@"unsupported_GLSLstd450"] || (need_shared && !totk_rt_msl_shared_ok(msl.UTF8String, need_shared)))) {
            [vs removeObjectAtIndex:i];  /* regenerate missing or previously un-compilable Metal variant */
            return nil;
        }
        [g_msl_checked addObject:found];
        return found;
    }
    return nil;
}
static NSString *stage_name(int stage) { return stage == 0 ? @"vs" : stage == 1 ? @"ps" : stage == 5 ? @"cs" : nil; }
/* The translation request for these answers; the guest code is written to <variant>.code (the translator reads it there).
   code_out: the code is handed back instead, to be written by the job that translates it (rt_job_run): an atomic file
   write on the submit thread took 10-30 ms, 0.42 s of the long frames of a session that met 1,670 new variants. */
static NSMutableDictionary *variant_request(NSString *shader, int stage, const uint8_t *control, const uint8_t *code,
                                            NSDictionary *answers, NSArray *local, NSData **code_out) {
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) die("shader %s: code area %#x / c1 %#x", shader.UTF8String, c1t, c1o);
    if (!stage_name(stage)) die("shader %s: NVN stage %d has no translation chain (tessellation/geometry)", shader.UTF8String, stage);
    uint8_t md[32]; NSMutableDictionary *key = [answers mutableCopy];
    if (stage == 5 && totk_rt_compute_shared_request(control)) key[@"shared"] = @(totk_rt_compute_shared_request(control));
    NSData *ad = [json_string(key) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@", shader, hex(md, 6)];
    NSString *code_path = [totk_rt_dir() stringByAppendingFormat:@"/%@.code", variant];
    // Graphics code drops the 0x30-byte NVN header (the translator wants SPH + instructions); compute keeps it, as in D2R's build.
    size_t skip = stage == 5 ? 0 : 0x30;
    NSData *bytes = [NSData dataWithBytes:code + skip length:c1t - skip];
    if (code_out) *code_out = bytes;
    else if (![bytes writeToFile:code_path atomically:YES]) die("write shader code %s", code_path.UTF8String);
    NSMutableDictionary *rq = [NSMutableDictionary dictionaryWithDictionary:@{@"variant": variant, @"stage": stage_name(stage), @"code": code_path, @"c1": hex(code + c1o, c1t - c1o)}];
    [rq addEntriesFromDictionary:answers];
    if (local) rq[@"local"] = local;
    if (stage == 5 && totk_rt_compute_shared_request(control)) rq[@"shared"] = @(totk_rt_compute_shared_request(control));
    rq[@"depth_minus_w"] = totk_depth_near_minus_w() ? @YES : @NO;   // nvnDeviceSetDepthMode: NEAR_IS_MINUS_W needs the GL -> Metal clip-z conversion in every vertex shader
    if (no_translator()) rq[@"code_hex"] = hex(code + skip, c1t - skip);
    return rq;
}
/* Whether a new variant goes into the contribution file (totk_shader_contrib.h). With a seed that translated every shader
   for the default bind state (all_default, prepare_shader_seed.py), the shared list keeps only the states those variants
   do not serve (user 2026-10-22): the shader's default variant is there and this is another one. A shader the build did
   not find or could not translate is left out too: the list cannot make a build find it. Older seeds (Breath of the
   Wild's): every new variant, as before. g_rt_lock held. */
static NSString *default_variant(NSString *shader) {   // the default bind state's name, as prepare_shader_seed.py SUFFIX (no answers at all)
    static NSString *suffix;
    if (!suffix) {
        uint8_t md[32]; NSData *ad = [json_string(@{@"tex": @{}, @"img": @{}, @"attr": @{}, @"fout": @{}}) dataUsingEncoding:NSUTF8StringEncoding];
        CC_SHA256(ad.bytes, (CC_LONG)ad.length, md); suffix = [@"-" stringByAppendingString:hex(md, 6)];
    }
    return [shader stringByAppendingString:suffix];
}
static BOOL contrib_wanted(NSString *shader, NSString *variant) {
    if (!g_seed_all_default) return YES;
    NSString *builtin = default_variant(shader);
    if ([variant isEqualToString:builtin]) return NO;
    for (NSDictionary *v in variants_of(shader)) if ([v[@"variant"] isEqual:builtin]) return YES;
    return NO;
}
/* nvn_mtl.m pso_contribute: the name a pipeline row gives "<variant>_b" so that another player's build has that variant.
   A variant the seed makes from the code alone keeps its name (the default bind state; the depth-bound one of a shadow-map
   shader, named after its depth slots only); any other is named as a build names a list row with its deps
   (totk_contrib_listed_name), which the row the same variant added to the contribution file makes. */
NSString *totk_rt_portable_variant(NSString *name) {
    if (name.length < 66 || ![name hasSuffix:@"_b"]) return name;
    NSString *variant = [name substringToIndex:name.length - 2], *shader = [variant substringToIndex:64];
    if ([variant isEqualToString:default_variant(shader)]) return name;
    pthread_mutex_lock(&g_rt_lock);
    NSDictionary *deps = nil;
    for (NSDictionary *v in variants_of(shader)) if ([v[@"variant"] isEqual:variant]) { deps = v[@"deps"]; break; }
    pthread_mutex_unlock(&g_rt_lock);
    if (![deps isKindOfClass:NSDictionary.class]) return name;
    NSMutableDictionary *depth = [NSMutableDictionary dictionary];
    NSDictionary *tex = [deps[@"tex"] isKindOfClass:NSDictionary.class] ? deps[@"tex"] : @{};
    for (NSString *slot in tex) if ([tex[slot] isEqual:@"*_depth"]) depth[slot] = @"*_depth";
    if (depth.count && [variant isEqualToString:totk_contrib_listed_name(shader, @{@"tex": depth})]) return name;
    return [totk_contrib_listed_name(shader, deps) stringByAppendingString:@"_b"];
}
int totk_rt_contrib_on(void) { return totk_contrib_path() != nil; }
void totk_rt_contrib_line(NSString *line) {   // totk_contrib_add_line under the lock its other writers hold
    pthread_mutex_lock(&g_rt_lock);
    totk_contrib_add_line(line);
    pthread_mutex_unlock(&g_rt_lock);
}
/* No translator on the phone (Python + .NET + SPIRV-Cross): the variant cache is translated on the Mac and copied over.
   A miss records the complete request (with the guest code bytes) under shader-requests/ for ios/tools/fill_shaders.py.
   A miss no longer stops the game: the caller skips the draws / dispatches that need this variant (draw-skip reason
   "shader-missing"). The request is written once per variant; after fill_shaders.py pushed the translation, the next
   start compiles it in the background (totk_rt_warm_filled) and moves the request to shader-requests/done/. */
static void record_miss(NSString *shader, NSDictionary *rq, NSDictionary *answers) {   // g_rt_lock held
    NSString *variant = rq[@"variant"];
    [NSFileManager.defaultManager createDirectoryAtPath:@"shader-requests" withIntermediateDirectories:YES attributes:nil error:nil];
    static NSMutableSet<NSString *> *missed; if (!missed) missed = [NSMutableSet set];
    if ([missed containsObject:variant]) return;
    [missed addObject:variant];
    [[json_string(rq) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[NSString stringWithFormat:@"shader-requests/%@.json", variant] atomically:YES];
    if (contrib_wanted(shader, variant)) totk_contrib_add(shader, answers);   // not in the prepared seed, so not in the shared shader list yet
    LOG("[shader-miss] %s %s 不在手机的翻译缓存里：跳过用到它的绘制，请求已存 shader-requests/（第 %lu 个；在 Mac 上 ios/tools/fill_shaders.py 补上后下次启动预热）",
        [rq[@"stage"] UTF8String], variant.UTF8String, (unsigned long)missed.count);
}
static NSString *patch_img_writes(NSString *msl, NSString *handle, NSString *vecType) {
    NSString *needle = [NSString stringWithFormat:@".%@.write(float4(", handle];
    NSRange search = NSMakeRange(0, msl.length);
    NSMutableString *out = nil;
    NSUInteger copied = 0;
    while (1) {
        NSRange r = [msl rangeOfString:needle options:0 range:search];
        if (r.location == NSNotFound) break;
        if (!out) out = [NSMutableString stringWithCapacity:msl.length + 256];
        NSUInteger argStart = r.location + r.length;   // right after "float4("
        int depth = 1;
        NSUInteger p = argStart;
        while (p < msl.length && depth > 0) {
            unichar c = [msl characterAtIndex:p++];
            if (c == '(') depth++;
            else if (c == ')') depth--;
        }
        if (depth != 0) break;
        [out appendString:[msl substringWithRange:NSMakeRange(copied, r.location - copied)]];
        [out appendFormat:@".%@.write(as_type<%@>(float4(%@)", handle, vecType, [msl substringWithRange:NSMakeRange(argStart, p - argStart)]];
        copied = p;
        search = NSMakeRange(p, msl.length - p);
    }
    if (!out) return msl;
    [out appendString:[msl substringFromIndex:copied]];
    return out;
}

/* One request through a translator service; the reflection of the new variant. */
static NSDictionary *translate(rt_svc *svc, NSDictionary *rq, uint64_t started) {
    NSString *variant = rq[@"variant"]; const char *stage = [rq[@"stage"] UTF8String];
    service_start(svc);
    fprintf(svc->in, "%s\n", json_string(rq).UTF8String); fflush(svc->in);
    char line[2048];
    if (!fgets(line, sizeof line, svc->out)) die("shader service exited while translating %s", variant.UTF8String);
    NSDictionary *reply = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line length:strlen(line)] options:0 error:nil];
    if (![reply[@"ok"] boolValue]) {
        LOG("[survey-shader] frame %llu translation FAILED %s %s after %.2f ms: %s",
            (unsigned long long)dm_nvn_frame_count(), stage, variant.UTF8String,
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6, [[reply[@"error"] description] UTF8String]);
        return nil; // Keep the error visible; never publish an invalid reflection.
    }
    NSString *json_path = [totk_rt_dir() stringByAppendingFormat:@"/%@_b.json", variant];
    NSData *jd = [NSData dataWithContentsOfFile:json_path];
    NSDictionary *refl = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![refl isKindOfClass:[NSDictionary class]]) { LOG("shader %s: reflection missing", variant.UTF8String); return nil; }
    if (rq[@"shared"] && ![refl[@"deps"][@"shared"] isEqual:rq[@"shared"]]) {
        LOG("shader %s: translator did not preserve compute shared-memory metadata; rebuild the MCD2 shader service", variant.UTF8String);
        return nil;
    }
    NSDictionary *rq_img = [rq[@"img"] isKindOfClass:NSDictionary.class] ? rq[@"img"] : nil;
    if (rq_img.count) {
        NSString *metal_path = [totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", variant];
        NSString *msl = [NSString stringWithContentsOfFile:metal_path encoding:NSUTF8StringEncoding error:nil];
        if (msl) {
            NSString *orig_msl = msl;
            NSMutableDictionary *mrefl = [refl mutableCopy];
            NSMutableDictionary *mdeps = [refl[@"deps"] isKindOfClass:NSDictionary.class] ? [refl[@"deps"] mutableCopy] : [NSMutableDictionary dictionary];
            NSMutableDictionary *mimg = [mdeps[@"img"] isKindOfClass:NSDictionary.class] ? [mdeps[@"img"] mutableCopy] : [NSMutableDictionary dictionary];
            BOOL deps_changed = NO;
            const char *prefixes[] = {"cp", "fp", "vp"};
            const char *tex_kinds[] = {"texture_buffer", "texture2d", "texture3d", "texture1d"};
            for (NSString *slot in rq_img) {
                int s = slot.intValue;
                if (s < 0 || s >= 8) continue;
                NSDictionary *im = [rq_img[slot] isKindOfClass:NSDictionary.class] ? rq_img[slot] : nil;
                NSString *fmt = [im[@"fmt"] isKindOfClass:NSString.class] ? im[@"fmt"] : nil;
                if (!fmt) continue;
                NSString *sfx = [NSString stringWithFormat:@"_i_cb2_%02X_", 0x48 + s * 2];
                BOOL used = NO;
                for (int p = 0; p < 3; p++) {
                    NSString *h = [NSString stringWithFormat:@"%s%@", prefixes[p], sfx];
                    if ([msl containsString:h]) { used = YES; break; }
                }
                if (!used) continue;
                if (![mimg[slot] isEqual:im]) { mimg[slot] = im; deps_changed = YES; }
                BOOL is_uint = [fmt hasSuffix:@"ui"];
                BOOL is_int = !is_uint && [fmt hasSuffix:@"i"];
                if (!is_uint && !is_int) continue;
                NSString *elemType = is_uint ? @"uint" : @"int";
                NSString *vecType = is_uint ? @"uint4" : @"int4";
                for (int p = 0; p < 3; p++) {
                    NSString *h = [NSString stringWithFormat:@"%s%@", prefixes[p], sfx];
                    BOOL patched_decl = NO;
                    for (int k = 0; k < 4; k++) {
                        NSString *oldDecl = [NSString stringWithFormat:@"%s<float, access::write> %@ ", tex_kinds[k], h];
                        if ([msl containsString:oldDecl]) {
                            NSString *newDecl = [NSString stringWithFormat:@"%s<%@, access::write> %@ ", tex_kinds[k], elemType, h];
                            msl = [msl stringByReplacingOccurrencesOfString:oldDecl withString:newDecl];
                            patched_decl = YES;
                        }
                    }
                    if (patched_decl) msl = patch_img_writes(msl, h, vecType);
                }
            }
            if (![msl isEqualToString:orig_msl]) {
                [msl writeToFile:metal_path atomically:YES encoding:NSUTF8StringEncoding error:nil];
            }
            if (deps_changed) {
                mdeps[@"img"] = mimg;
                mrefl[@"deps"] = mdeps;
                refl = mrefl;
                [[json_string(refl) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:json_path atomically:YES];
            }
        }
    }
    pthread_mutex_lock(&g_names_lock);
    if (g_cache_names) {
        [g_cache_names addObject:[variant stringByAppendingString:@"_b.metal"]];
        [g_cache_names addObject:[variant stringByAppendingString:@"_b.json"]];
    }
    pthread_mutex_unlock(&g_names_lock);
    return refl;
}
/* A translated variant into the shader's index, the counters and the contribution file (g_rt_lock held). */
static void variant_record(NSString *shader, NSDictionary *rq, NSDictionary *refl, BOOL existed, uint64_t started, uint64_t queued) {   // queued: 0, or when a background job was queued
    NSString *variant = rq[@"variant"];
    NSMutableArray *vs = variants_of(shader);
    [vs addObject:@{@"variant": variant, @"deps": refl[@"deps"] ?: @{}}];
    if (!g_msl_checked) g_msl_checked = [NSMutableSet set];
    [g_msl_checked addObject:variant];
    [[json_string(vs) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:index_path(shader) atomically:YES];
    pthread_mutex_lock(&g_names_lock);
    if (g_cache_names) {
        [g_cache_names addObject:[variant stringByAppendingString:@"_b.metal"]];
        [g_cache_names addObject:[variant stringByAppendingString:@"_b.json"]];
        [g_cache_names addObject:[shader stringByAppendingString:@".variants"]];
    }
    pthread_mutex_unlock(&g_names_lock);
    g_translated++;
    if (!existed) { atomic_fetch_add(&g_new_variants, 1); atomic_store(&g_new_variant_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }   // discovered, translated, in the cache
    if (!existed && contrib_wanted(shader, variant)) totk_contrib_add(shader, refl[@"deps"] ?: @{});   // the seed would have had it if the shared list named it
    if (getenv("TOTK_SURVEY_SHADER_LOG")) LOG("[survey-shader] frame %llu translated %s %s in %.2f ms%s%s",
        (unsigned long long)dm_nvn_frame_count(), [rq[@"stage"] UTF8String], variant.UTF8String,
        (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6,
        [refl[@"softfloat64"] boolValue] ? " (FP64 fallback)" : "",
        queued ? [NSString stringWithFormat:@" (background, with its Metal library; queued %.2f ms before)", (started - queued) / 1e6].UTF8String : "");
    if (g_translated <= 8 || g_translated % 100 == 0) LOG("[shader] translated %llu: %s %s", (unsigned long long)g_translated, [rq[@"stage"] UTF8String], variant.UTF8String);
}

/* Background translation (Mac, user 2026-10-21: new shaders are prepared the way games stream their content): a variant
   the caller does not wait for (may_defer) goes to one of rt_bg_workers() translator services of their own, and nil is
   returned; nvn_mtl.m skips the draws that need it. One job per shader at a time: while it is queued or running, a
   request for the same shader defers again. A required request (may_defer 0) promotes a queued job to urgent or starts
   an urgent job, then waits with the cache mutex released. It rechecks dependency compatibility after other jobs finish.
   No translation runs under the global cache mutex. The worker also compiles the Metal library (g_rt_ready_hook), then records
   the variant and moves g_rt_gen.
   Urgent jobs (may_defer 2, nvn_mtl.m shader_predict_rest): the variants that draws further on in this submission cannot
   go without. They run at once on rt_urgent_workers() services of their own (4; the background line waits meanwhile,
   TOTK_RT_URGENT_FIRST=0 lets it run on), a queued job of the same shader taken out of
   its line, so those draws wait in parallel instead of translating and compiling one after another on the submit
   thread behind the background line (entering an area: 157 libraries, 6.3 s on the submit thread, 2026-10-22). */
#define RT_BG_MAX 8
#define RT_BG_DEFAULT 4   // 2 left draws waiting to be shown for 13-15 s after flying into an area; 4 halved the skipped draws (2026-10-22)
/* Background translator services (each its own translator process and serial queue): TOTK_RT_BG_WORKERS, 1-8. */
static int rt_bg_workers(void) {
    static int n;
    if (!n) { const char *e = getenv("TOTK_RT_BG_WORKERS"); n = e ? atoi(e) : RT_BG_DEFAULT; n = n < 1 ? 1 : n > RT_BG_MAX ? RT_BG_MAX : n; }
    return n;
}
#define RT_URGENT_MAX 8
/* Urgent translator services: TOTK_RT_URGENT_WORKERS, 1-8. */
static int rt_urgent_workers(void) {
    static int n; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_RT_URGENT_WORKERS"); n = e ? atoi(e) : 4;
        n = n < 1 ? 1 : n > RT_URGENT_MAX ? RT_URGENT_MAX : n;
    });
    return n;
}
static rt_svc g_urgent_svc[RT_URGENT_MAX];
/* Experimental, opt-in: start processes during startup, without translating
   guessed variants, publishing cache rows or occupying shader job slots. */
void totk_rt_prestart(void) {
    const char *e = getenv("MCD2_SHADER_PRESTART");
    if ((e && !strcmp(e, "0")) || !totk_rt_can_translate()) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
            for (int i = 0; i < rt_urgent_workers(); i++) service_start(&g_urgent_svc[i]);
            LOG("[shader-prestart] urgent translator processes started (no shader requests)");
        } });
    });
}
static _Atomic int g_rt_urgent_busy;   // urgent jobs queued or running: the background line does not start new jobs meanwhile
enum { RT_JOB_QUEUED, RT_JOB_RUNNING, RT_JOB_TAKEN };
static NSMutableDictionary<NSString *, NSMutableArray *> *g_rt_pending;   // shader -> its job: @[@RT_JOB_*, request, @stage, @existed, code] (g_rt_lock)
static _Atomic uint64_t g_rt_urgent_n, g_rt_urgent_ns;
static pthread_cond_t g_rt_done = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_rt_gen;
static void (*g_rt_ready_hook)(NSString *variant, int stage);
void totk_rt_set_ready_hook(void (*hook)(NSString *variant, int stage)) { g_rt_ready_hook = hook; }
uint64_t totk_rt_ready_gen(void) { return atomic_load(&g_rt_gen); }
/* One job: translate on svc, compile its Metal library (the ready hook), record it and wake the waiters. */
static void rt_job_run(NSString *shader, NSMutableArray *job, rt_svc *svc, uint64_t queued) {
    NSDictionary *rq = job[1]; int stage = [job[2] intValue]; BOOL existed = [job[3] boolValue]; NSData *code = job[4];
    if (![code writeToFile:rq[@"code"] atomically:YES]) die("write shader code %s", [rq[@"code"] UTF8String]);
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // Optional exact requests for offline profiling; never reconstruct inputs from
    // reflection dependencies (which deliberately omit unqueried answers).
    const char *capture = getenv("TOTK_SHADER_CAPTURE_REQUESTS");
    if (capture && !strcmp(capture, "1"))
        [[json_string(rq) dataUsingEncoding:NSUTF8StringEncoding]
            writeToFile:[totk_rt_dir() stringByAppendingFormat:@"/%@.request.json", rq[@"variant"]] atomically:YES];
    NSDictionary *refl = translate(svc, rq, started);
    uint64_t translated = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (refl && g_rt_ready_hook) g_rt_ready_hook(rq[@"variant"], stage);   // the Metal library too, off the submit thread
    uint64_t compiled = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_rt_lock);
    if (getenv("TOTK_SURVEY_SHADER_LOG"))
        LOG("[shader-phases] %s queue %.2f translate %.2f metal %.2f publish-lock %.2f ms",
            [rq[@"variant"] UTF8String], (started - queued) / 1e6,
            (translated - started) / 1e6, (compiled - translated) / 1e6,
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - compiled) / 1e6);
    if (refl) variant_record(shader, rq, refl, existed, started, queued);
    else { if (!g_failed_variants) g_failed_variants = [NSMutableSet set]; [g_failed_variants addObject:rq[@"variant"]]; }
    if (g_rt_pending[shader] == job) [g_rt_pending removeObjectForKey:shader];
    atomic_fetch_add(&g_rt_gen, 1);
    pthread_cond_broadcast(&g_rt_done);
    pthread_mutex_unlock(&g_rt_lock);
}
static NSMutableArray *rt_job_new(NSString *shader, int stage, NSDictionary *rq, BOOL existed, NSData *code) {   // g_rt_lock held
    if (!g_rt_pending) g_rt_pending = [NSMutableDictionary dictionary];
    NSMutableArray *job = [NSMutableArray arrayWithObjects:@(RT_JOB_QUEUED), rq, @(stage), @(existed), code, nil];
    g_rt_pending[shader] = job;
    return job;
}
static void bg_translate(NSString *shader, int stage, NSDictionary *rq, BOOL existed, NSData *code) {   // g_rt_lock held
    static rt_svc svc[RT_BG_MAX]; static dispatch_queue_t queue[RT_BG_MAX]; static unsigned next; int workers = rt_bg_workers();
    if (!queue[0]) for (int i = 0; i < workers; i++)
        queue[i] = dispatch_queue_create("totk.shader.background", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    NSMutableArray *job = rt_job_new(shader, stage, rq, existed, code);
    rt_svc *own = &svc[next % workers]; dispatch_queue_t q = queue[next++ % workers];
    uint64_t queued = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dispatch_async(q, ^{ @autoreleasepool {
        static int yield = -1; if (yield < 0) { const char *e = getenv("TOTK_RT_URGENT_FIRST"); yield = !(e && !strcmp(e, "0")); }
        while (yield && atomic_load(&g_rt_urgent_busy) > 0) usleep(2000);   // the CPU to what draws are waiting for
        pthread_mutex_lock(&g_rt_lock);
        int mine = [job[0] intValue] == RT_JOB_QUEUED;   // not taken back by a draw, nor made urgent
        if (mine) job[0] = @(RT_JOB_RUNNING);
        pthread_mutex_unlock(&g_rt_lock);
        if (mine) rt_job_run(shader, job, own, queued);
    } });
}
/* Run a job now on a free urgent service (g_rt_lock held; the job is marked running, so waiters wait for it). */
static void urgent_translate(NSString *shader, NSMutableArray *job) {
    static int busy[RT_URGENT_MAX]; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static dispatch_semaphore_t free_svc; static dispatch_queue_t q;
    if (!q) { free_svc = dispatch_semaphore_create(rt_urgent_workers());
              // At the game's own level (user interactive, threads.c): one below, the jobs draws were waiting for took 30-49 ms
              // instead of 26 ms and the long frames did not get shorter (2026-10-22).
              q = dispatch_queue_create("totk.shader.urgent", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INTERACTIVE, 0)); }
    job[0] = @(RT_JOB_RUNNING);
    uint64_t queued = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_fetch_add(&g_rt_urgent_busy, 1);
    dispatch_async(q, ^{ @autoreleasepool {
        dispatch_semaphore_wait(free_svc, DISPATCH_TIME_FOREVER);
        pthread_mutex_lock(&mu); int i = 0; while (busy[i]) i++; busy[i] = 1; pthread_mutex_unlock(&mu);
        rt_job_run(shader, job, &g_urgent_svc[i], queued);
        pthread_mutex_lock(&mu); busy[i] = 0; pthread_mutex_unlock(&mu);
        dispatch_semaphore_signal(free_svc);
        atomic_fetch_sub(&g_rt_urgent_busy, 1);
        atomic_fetch_add(&g_rt_urgent_n, 1); atomic_fetch_add(&g_rt_urgent_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - queued);
    } });
}
void totk_rt_urgent_stats(uint64_t *n, uint64_t *ns) { *n = atomic_load(&g_rt_urgent_n); *ns = atomic_load(&g_rt_urgent_ns); }

static NSString *rt_variant_impl(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local, int may_defer) {
    if (stage == 5) {
        // NVN compute control: local size at +0x748, shared bytes at +0x754.
        // Include this in both lookup dependencies and the request hash so old
        // 4 KB fallback translations cannot satisfy or overwrite this variant.
        uint32_t shared; memcpy(&shared, control + 0x754, sizeof shared);
        if ((shared & 3) || shared > INT_MAX) die("shader %s: invalid shared-memory bytes %u", shader.UTF8String, shared);
        NSMutableDictionary *compute = [answers mutableCopy];
        compute[@"shared"] = @(shared); answers = compute;
    }
    pthread_mutex_lock(&g_rt_lock);
    // Called for the game's actual shader bytes, including when a prepared
    // variant already exists. Old cache files alone never become observed keys.
    totk_shader_observed_note(observed_keys_state(), shader);
    NSString *found = variant_lookup(shader, answers, stage == 5 ? totk_rt_compute_shared_request(control) : 0);
    for (NSMutableArray *job; !found && !may_defer && (job = g_rt_pending[shader]); found = variant_lookup(shader, answers, stage == 5 ? totk_rt_compute_shared_request(control) : 0)) {
        if ([job[0] intValue] == RT_JOB_QUEUED) urgent_translate(shader, job);
        pthread_cond_wait(&g_rt_done, &g_rt_lock);
    }
    if (!found && may_defer == 2 && g_rt_pending[shader]) {   // urgent: a job of this shader still in line runs now
        NSMutableArray *job = g_rt_pending[shader];
        if ([job[0] intValue] == RT_JOB_QUEUED) urgent_translate(shader, job);
        pthread_mutex_unlock(&g_rt_lock); return nil;
    }
    if (found || g_rt_pending[shader]) { pthread_mutex_unlock(&g_rt_lock); return found; }
    NSData *code_bytes = nil;   // written by the job when it is not translated here
    NSMutableDictionary *rq = variant_request(shader, stage, control, code, answers, local, !no_translator() ? &code_bytes : NULL);
    if ([g_failed_variants containsObject:rq[@"variant"]]) { pthread_mutex_unlock(&g_rt_lock); return nil; }
    if (no_translator()) { record_miss(shader, rq, answers); pthread_mutex_unlock(&g_rt_lock); return nil; }
    BOOL existed = totk_rt_exists([NSString stringWithFormat:@"%@_b.metal", rq[@"variant"]]);   // a re-translation of a broken variant is not a new one
    if (may_defer == 2) { urgent_translate(shader, rt_job_new(shader, stage, rq, existed, code_bytes)); pthread_mutex_unlock(&g_rt_lock); return nil; }
    if (may_defer) { bg_translate(shader, stage, rq, existed, code_bytes); pthread_mutex_unlock(&g_rt_lock); return nil; }
    /* Required work still waits, but never runs the translator while holding
       the cache mutex. Other stages can translate/compile and publish while
       this caller sleeps. Share the same single-flight job with async callers. */
    NSMutableArray *job = rt_job_new(shader, stage, rq, existed, code_bytes);
    urgent_translate(shader, job);
    while (g_rt_pending[shader] == job) pthread_cond_wait(&g_rt_done, &g_rt_lock);
    found = variant_lookup(shader, answers, stage == 5 ? totk_rt_compute_shared_request(control) : 0);
    pthread_mutex_unlock(&g_rt_lock);
    return found;
}


/* Include translator-lock contention and waiting for background jobs, neither of
   which is counted by the Metal library / PSO timers in the long-frame report. */
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local, int may_defer) {
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NSString *variant = rt_variant_impl(shader, stage, control, code, answers, local, may_defer);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if (elapsed >= 50000000ull) LOG("[shader-stall] frame %llu stage %d defer %d lookup/translate/wait %.2f ms shader %s ready %d",
        (unsigned long long)dm_nvn_frame_count(), stage, may_defer, elapsed / 1e6, shader.UTF8String, variant != nil);
    return variant;
}

NSArray<NSString *> *totk_rt_known_variants(NSString *shader) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    pthread_mutex_lock(&g_rt_lock);
    for (NSDictionary *v in variants_of(shader))
        if ([v isKindOfClass:NSDictionary.class] && [v[@"variant"] isKindOfClass:NSString.class]) [out addObject:v[@"variant"]];
    pthread_mutex_unlock(&g_rt_lock);
    return out;
}

/* Pipeline prediction (nvn_mtl.m prog_prediction_unseen): the variant variant_lookup (totk_rt_variant) would return for
   these answers when the cache index already names one, else nil. Nothing is translated, read, written or noted; a lock
   held elsewhere answers nil instead of waiting. variant_lookup also reads a variant's MSL the first time and drops the
   variant when it is missing or cannot compile; such a variant is never loaded, and the caller takes loaded shaders only,
   so the answer is the same without that read. */
NSString *totk_rt_variant_cached(NSString *shader, NSDictionary *answers) {
    if (pthread_mutex_trylock(&g_rt_lock)) return nil;
    if (!g_variants || !g_variants[shader]) { pthread_mutex_unlock(&g_rt_lock); return nil; }
    NSString *found = nil;
    for (NSDictionary *v in variants_of(shader)) if (deps_match(v[@"deps"], answers)) { found = v[@"variant"]; break; }
    pthread_mutex_unlock(&g_rt_lock);
    return found;
}
void totk_rt_probe_index(NSString *shader, NSArray<NSDictionary *> *rows) {   // probe-gfx: a variant index without cache files
    pthread_mutex_lock(&g_rt_lock);
    if (!g_variants) g_variants = [NSMutableDictionary dictionary];
    g_variants[shader] = rows ? [rows mutableCopy] : nil;
    pthread_mutex_unlock(&g_rt_lock);
}

void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *dir = totk_rt_dir();
        atomic_store(&g_new_at_count, atomic_load(&g_new_variants));
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{   // ~45,000 directory entries: not on the present thread
            int64_t n = 0;
            if (g_seed_dir) { for (NSString *name in totk_rt_list()) if ([name hasSuffix:@"_b.metal"]) n++; }
            else { DIR *d = opendir(dir.fileSystemRepresentation); struct dirent *e;
                   if (d) { while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 8 && !strcmp(e->d_name + l - 8, "_b.metal")) n++; } closedir(d); } }
            atomic_store(&g_cached_variants, n);
        });
    });
    uint64_t f = atomic_load(&g_new_variants); int64_t t = atomic_load(&g_cached_variants);
    if (fresh) *fresh = f;
    if (total) *total = t < 0 ? -1 : t + (int64_t)(f - atomic_load(&g_new_at_count));
    if (last_new_ns) *last_new_ns = atomic_load(&g_new_variant_ns);
}

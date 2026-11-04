// TOTK: the game's states for Apple's performance tools (macOS 27 StateReporting; not in D2R).
// TOTK_STATE_REPORTING=1 (off by default; `flag state_reporting 0|1` in the input file switches it while playing). macOS
// records frame timing of every Metal layer all the time (metalperftrace(1)); with these reports Instruments and
// `metalperftrace overview --aggregate` (games/totk/scripts/metal_perf_report.py) split it by what the game was doing.
//  - org.macnativegames.Lifecycle: starting, shaderWarmup (archive reused / rebuilt) until the start-up screen goes,
//    focused / unfocused (the window has the keyboard or not: the Mac never pauses the game), settings (the panel is
//    open: the game gets no input), occluded (covered or on another Space: macOS stops presenting the window, so that
//    stretch is no measure of the game), closing. Title, menus, world and loading are not reported: nothing in the port
//    knows them reliably.
//  - org.macnativegames.Graphics: the configuration the renderer runs with, e.g. "1920x1080 | MetalFX+output | TAA |
//    FI 50+50 | held 50 | fullscreen@120Hz": picture size, aspect ratio, MetalFX upscaling, edge smoothing, frame
//    interpolation, the frame-rate cap and why, the present path, full screen or window and the screen's refresh rate.
//    Whether a stage actually ran is content-dependent (a menu has no depth to anti-alias, a scene as large as the canvas
//    needs no upscale), so it is not a state: the counters below say how many game frames it ran on.
//  - metalperftrace keys states by their label only (a change of the stable metadata alone does not show there), so each
//    configuration has its own label; the stable metadata repeats it as fields and adds the window's drawable size.
//  - Volatile metadata goes with every change: the game's own counters (presents, interpolated frames shown, frames the
//    display merged away, frames with a MetalFX upscale or edge smoothing, frames per pacing step, shaders and pipelines
//    made, memory). The report takes their change over a state: game frames apart from what the window showed.
//  - Event driven: Lifecycle at the events; Graphics from a comparison of small integers once per game frame on the game
//    thread (totk_sr_frame). A change is reported at once unless one was reported in the last SETTLE_NS; then the
//    changes are merged until the configuration has held that long, or MAXWAIT_NS passed. All StateReporting calls run
//    on one serial queue; the game's threads only store and enqueue. Off: the hooks read one atomic int.
//  - StateReporting is opened at run time (dlopen, NSClassFromString): no header or -framework, so the file builds with
//    any SDK and an app built for macOS 26 does not link it; there the switch says so once and the game runs as without.
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include "totk_state_report.h"
#include "totk_mfx_mode.h"
#include "dmsh_chain.h"

#define SLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [state] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#ifndef TOTK_SR_FRAMEWORK   // tests point these elsewhere (tests/state_report_probe.m)
#define TOTK_SR_FRAMEWORK "/System/Library/Frameworks/StateReporting.framework/StateReporting"
#endif
#ifndef TOTK_SR_CLASS
#define TOTK_SR_CLASS @"SRStateReporter"
#endif
#define SETTLE_NS 500000000ull     // a graphics change within this time of the last report waits until it has held this long
#define MAXWAIT_NS 2000000000ull   // ... but not longer than this (a configuration that keeps changing is still reported)

/* SRStateReporter (StateReporting.framework, macOS 27), declared here instead of imported (see above). */
@protocol TotkSRClass
+ (id)reporterForDomain:(NSString *)domain;
@end
@protocol TotkSR
- (void)reportTransitionToStateLabel:(NSString *)label stableMetadata:(NSDictionary<NSString *, NSObject *> *)stable
                    volatileMetadata:(NSDictionary<NSString *, NSObject *> *)vol;
@end

_Atomic int g_totk_sr_on;
static dispatch_queue_t g_q;                          // every report, in order
static id<TotkSR> g_life, g_gfx;                      // queue
static NSDictionary *g_ident;                         // set once at start: game, version, build, host, settings
static _Atomic int g_started;
/* Lifecycle facts, kept also while off (rare events), so that switching on later reports the right state. */
static _Atomic int g_archive = -1, g_picture, g_key, g_visible = 1, g_settings, g_closing;
static NSString *g_life_label;                        // queue: the last Lifecycle label reported
/* Graphics: the game thread compares with its own last snapshot; g_mu guards the newest one and the pending wait. */
static _Atomic int g_view_w, g_view_h, g_display_hz, g_fullscreen;
static _Atomic unsigned g_epoch;                      // switching on makes the next frame count as a change
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static totk_sr_gfx g_latest; static uint64_t g_latest_ns, g_pending_ns; static int g_have_latest, g_armed;
static totk_sr_gfx g_reported; static int g_have_reported; static uint64_t g_reported_ns;   // queue
/* Activity counted on the game thread while on: game frames a MetalFX upscale / edge smoothing ran on, frames per step. */
static _Atomic uint64_t g_upscaled, g_antialiased, g_ticks[4];

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int sr_load(void) {   // queue, once: the framework and the two reporters (macOS 26 has no such framework)
    static int ok = -1;
    if (ok >= 0) return ok;
    ok = 0;
    void *h = dlopen(TOTK_SR_FRAMEWORK, RTLD_LAZY | RTLD_LOCAL);
    Class c = h ? NSClassFromString(TOTK_SR_CLASS) : Nil;
    if (c && [c respondsToSelector:@selector(reporterForDomain:)]) {
        g_life = [(Class<TotkSRClass>)c reporterForDomain:@"org.macnativegames.Lifecycle"];
        g_gfx = [(Class<TotkSRClass>)c reporterForDomain:@"org.macnativegames.Graphics"];
        SEL s = @selector(reportTransitionToStateLabel:stableMetadata:volatileMetadata:);
        ok = [(id)g_life respondsToSelector:s] && [(id)g_gfx respondsToSelector:s];
    }
    if (!ok) SLOG("TOTK_STATE_REPORTING: StateReporting is not there (it needs macOS 27); the game runs without state reports");
    return ok;
}

static NSString *runtime_uuid(void) {   // LC_UUID of the game binary: ties a trace to a build (metal_perf_report.py --app)
    // the executable's own header: image 0 is a library when one is inserted (DYLD_INSERT_LIBRARIES, Xcode's checkers)
    const struct mach_header_64 *mh = _NSGetMachExecuteHeader();
    if (!mh || mh->magic != MH_MAGIC_64) return nil;
    const uint8_t *p = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmd == LC_UUID) return [[[NSUUID alloc] initWithUUIDBytes:((const struct uuid_command *)lc)->uuid] UUIDString];
        p += lc->cmdsize;
    }
    return nil;
}

static NSString *sysctl_string(const char *name) {
    char buf[256]; size_t n = sizeof buf;
    return sysctlbyname(name, buf, &n, NULL, 0) == 0 && n > 1 ? [NSString stringWithUTF8String:buf] : nil;
}

/* The switches that change what is measured, as set for this run (environment; numbers and words only, never paths). */
static NSString *settings_in_force(void) {
    static const char *const names[] = {"TOTK_UNCAPPED", "TOTK_UNCAPPED_HZ", "TOTK_FPS60", "TOTK_REALTIME", "TOTK_MFX",
        "TOTK_MFX_UPSCALE", "TOTK_MFX_OUTPUT", "TOTK_MFX_TEMPORAL", "TOTK_MFX_TEMPORAL_SHARPEN", "TOTK_MFX_TEMPORAL_MIPBIAS",
        "TOTK_MFX_TEMPORAL_INPUT", "TOTK_MFX_TEMPORAL_REWARM", "TOTK_TAA", "TOTK_RENDER_RES", "TOTK_OUTPUT_RES", "TOTK_FIXED_RES",
        "TOTK_ASPECT", "TOTK_GPU_KEEPALIVE", "TOTK_WARM_BEFORE_TITLE", "TOTK_ASSUME_FOCUS", "TOTK_ASYNC_REPLAY",
        "TOTK_UNCAPPED_INTERVAL", "TOTK_WINDOW_SIZE", "TOTK_DRAWABLE_SCALE", "DM_PRESENT_LINK", "DM_PRESENT_HZ",
        "DM_PRESENT_INTERVAL", "DM_NVN_TSV", "MTL_HUD_ENABLED", "METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS",
        "METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS"};
    NSMutableArray *out = [NSMutableArray array];
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const char *v = getenv(names[i]);
        if (v && *v && strlen(v) < 32 && !strchr(v, '/')) [out addObject:[NSString stringWithFormat:@"%s=%s", names[i], v]];
    }
    return out.count ? [out componentsJoinedByString:@" "] : @"defaults";
}

static NSDictionary *counters(void) {   // queue: the volatile metadata of every report
    totk_sr_counters c = {0};
    mtl_sr_counters(&c);
    task_vm_info_data_t vm; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) == KERN_SUCCESS ? vm.phys_footprint : 0;
    return @{@"gameFrames": @(c.game_frames), @"generatedFrames": @(c.generated), @"mergedFrames": @(c.merged),
             @"upscaledFrames": @(atomic_load(&g_upscaled)), @"antiAliasedFrames": @(atomic_load(&g_antialiased)),
             @"paceTicks1": @(atomic_load(&g_ticks[1])), @"paceTicks2": @(atomic_load(&g_ticks[2])), @"paceTicks3": @(atomic_load(&g_ticks[3])),
             @"shaderTranslations": @(c.translations), @"librariesCreated": @(c.libraries), @"pipelinesCreated": @(c.pipelines),
             @"creationMs": @(llround(c.create_ns / 1e6)), @"footprintMiB": @(foot >> 20), @"metalAllocatedMiB": @(c.metal_bytes >> 20),
             @"scene": [NSString stringWithFormat:@"%dx%d", c.scene_w, c.scene_h],
             @"sampledAt": [NSDate date]};   // when these were read: the system's heartbeat repeats a state later with the same values
}

static void report(id<TotkSR> r, NSString *label, NSDictionary *stable) {   // queue
    @try { [r reportTransitionToStateLabel:label stableMetadata:stable volatileMetadata:counters()]; }
    @catch (NSException *e) { SLOG("StateReporting refused a report (%s); reports stop", e.reason.UTF8String); atomic_store(&g_totk_sr_on, 0); }
}

static NSString *life_label(void) {
    if (atomic_load(&g_closing)) return @"closing";
    if (!atomic_load(&g_picture)) {
        int archive = atomic_load(&g_archive);
        return archive < 0 ? @"starting" : archive ? @"shaderWarmup (archive reused)" : @"shaderWarmup (archive rebuilt)";
    }
    return !atomic_load(&g_visible) ? @"occluded" : atomic_load(&g_settings) ? @"settings" : atomic_load(&g_key) ? @"focused" : @"unfocused";
}
static void life_report(NSString *label) {   // queue
    if (!totk_sr_on() || [label isEqualToString:g_life_label]) return;
    g_life_label = label;
    report(g_life, label, g_ident);
    SLOG("Lifecycle: %s", label.UTF8String);
}
/* The label is taken where the event happens, in event order (events come from the main thread and the warm-up). */
static void life_event(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (!totk_sr_on()) return;
    pthread_mutex_lock(&mu);
    NSString *label = life_label();
    dispatch_async(g_q, ^{ life_report(label); });
    pthread_mutex_unlock(&mu);
}

static NSString *fi_name(int m) { return m == MFX_30 ? @"30+30" : m == MFX_40 ? @"40+40" : m == MFX_50 ? @"50+50" : m == MFX_MAX ? @"max" : @"off"; }
static NSString *aspect_name(int milli) {
    static const struct { int milli; const char *name; } known[] = {{1600, "16:10"}, {2333, "21:9"}, {2370, "21:9"}, {2389, "43:18"}, {3556, "32:9"}};
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (abs(milli - known[i].milli) <= 8) return @(known[i].name);
    return [NSString stringWithFormat:@"%.2f:1", milli / 1000.0];
}

/* The label holds every field of the stable metadata except the drawable size (see the top). */
static void gfx_report(const totk_sr_gfx *s) {   // queue
    if (!totk_sr_on()) return;
    static NSString *const srcs[] = {@"game interval", @"uncapped", @"uncapped, game interval", @"lock", @"interpolation",
                                     @"movie", @"phone", @"none", @"display interval"};
    int src = s->cap_src >= 0 && s->cap_src <= SR_CAP_DISPLAY ? s->cap_src : SR_CAP_GAME;
    int held = src == SR_CAP_LOCK || src == SR_CAP_INTERP || src == SR_CAP_MOVIE || src == SR_CAP_PHONE;
    NSMutableString *l = [NSMutableString stringWithFormat:@"%dx%d", s->out_w, s->out_h];
    if (s->aspect_milli) [l appendFormat:@" | %@", aspect_name(s->aspect_milli)];
    if (s->upscale) [l appendString:s->upscale > 1 ? @" | MetalFX+output" : @" | MetalFX"];
    if (s->temporal) [l appendString:@" | MetalFX temporal"];
    if (s->taa) [l appendString:@" | TAA"];
    if (s->fi_mode) [l appendFormat:@" | FI %@", fi_name(s->fi_mode)];
    switch (src) {
    case SR_CAP_UNCAPPED: [l appendFormat:@" | cap %d", s->cap]; break;
    case SR_CAP_UNCAPPED_INTERVAL: [l appendFormat:@" | cap %d/game interval", s->cap]; break;
    case SR_CAP_NONE: [l appendString:@" | no cap"]; break;
    case SR_CAP_DISPLAY: [l appendString:@" | display interval"]; break;
    case SR_CAP_GAME: [l appendString:@" | game interval"]; break;
    default: [l appendFormat:@" | %@ %d%@", src == SR_CAP_LOCK ? @"lock" : src == SR_CAP_INTERP ? @"held" : srcs[src], s->cap, s->own_clock ? @" own clock" : @""];
    }
    if (s->present_path) [l appendString:s->present_path == 2 ? @" | present thread (display link stalled)" : @" | present thread"];
    [l appendFormat:@" | %@@%dHz", s->fullscreen ? @"fullscreen" : @"window", s->display_hz];
    int paced = (held || src == SR_CAP_GAME || src == SR_CAP_DISPLAY) && s->paced_hz > 0 && s->paced_hz != s->display_hz;
    if (paced) [l appendFormat:@" (paced %dHz)", s->paced_hz];
    NSDictionary *st = @{@"output": [NSString stringWithFormat:@"%dx%d", s->out_w, s->out_h],
                         @"aspect": s->aspect_milli ? aspect_name(s->aspect_milli) : @"16:9",
                         @"upscaling": s->upscale > 1 ? @"metalfx+output" : s->upscale ? @"metalfx" : @"off",
                         @"temporalUpscaling": s->temporal ? @"on" : @"off",
                         @"taa": s->taa ? @"on" : @"off", @"interpolation": fi_name(s->fi_mode),
                         @"fpsCap": @(s->cap), @"fpsCapSource": srcs[src], @"ownClock": @(s->own_clock),
                         @"pacingHz": @(paced ? s->paced_hz : 0),
                         @"presentPath": s->present_path == 2 ? @"present thread (display link stalled)" : s->present_path ? @"present thread" : @"display link",
                         @"fullscreen": @(s->fullscreen), @"displayHz": @(s->display_hz),
                         @"view": [NSString stringWithFormat:@"%dx%d", s->view_w, s->view_h]};
    report(g_gfx, l, st);
    SLOG("Graphics: %s (%dx%d)", l.UTF8String, s->view_w, s->view_h);
}

static void settle(void) {   // queue
    pthread_mutex_lock(&g_mu);
    totk_sr_gfx s = g_latest; int have = g_have_latest;
    uint64_t now = now_ns(), changed = g_latest_ns, pending = g_pending_ns;
    // leading: the first change after a quiet half second goes at once; later ones wait until quiet, at most MAXWAIT_NS
    int leading = pending == changed && now - g_reported_ns >= SETTLE_NS, quiet = now - changed >= SETTLE_NS, overdue = now - pending >= MAXWAIT_NS;
    if (have && !leading && !quiet && !overdue) {
        uint64_t wait = SETTLE_NS - (now - changed), left = MAXWAIT_NS - (now - pending);
        pthread_mutex_unlock(&g_mu);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait < left ? wait : left)), g_q, ^{ settle(); });
        return;
    }
    g_pending_ns = 0; g_armed = 0;
    pthread_mutex_unlock(&g_mu);
    if (!have || (g_have_reported && !memcmp(&s, &g_reported, sizeof s))) return;   // nothing new, or it changed back
    g_reported = s; g_have_reported = 1; g_reported_ns = now;
    gfx_report(&s);
}

void totk_sr_frame(int cap, int cap_src, int own_clock, int paced_hz, unsigned ticks) {
    static __thread totk_sr_gfx seen; static __thread unsigned seen_epoch; static __thread uint64_t up_seen, aa_seen;   // per thread: no lock on the frame path
    totk_sr_gfx s; memset(&s, 0, sizeof s);
    uint64_t up_at = 0, aa_at = 0;
    mtl_sr_picture(&s, &up_at, &aa_at);
    if (up_at != up_seen) { up_seen = up_at; atomic_fetch_add_explicit(&g_upscaled, 1, memory_order_relaxed); }
    if (aa_at != aa_seen) { aa_seen = aa_at; atomic_fetch_add_explicit(&g_antialiased, 1, memory_order_relaxed); }
    atomic_fetch_add_explicit(&g_ticks[ticks < 3 ? ticks : 3], 1, memory_order_relaxed);
    if (!s.out_w) return;   // no game frame shown yet
    s.cap = cap; s.cap_src = cap_src; s.own_clock = own_clock; s.paced_hz = paced_hz;
    s.view_w = atomic_load_explicit(&g_view_w, memory_order_relaxed); s.view_h = atomic_load_explicit(&g_view_h, memory_order_relaxed);
    s.display_hz = atomic_load_explicit(&g_display_hz, memory_order_relaxed); s.fullscreen = atomic_load_explicit(&g_fullscreen, memory_order_relaxed);
    unsigned epoch = atomic_load_explicit(&g_epoch, memory_order_relaxed);
    if (seen_epoch == epoch && !memcmp(&s, &seen, sizeof s)) return;
    seen = s; seen_epoch = epoch;
    pthread_mutex_lock(&g_mu);
    uint64_t now = now_ns();
    g_latest = s; g_have_latest = 1; g_latest_ns = now;
    if (!g_pending_ns) g_pending_ns = now;
    int arm = !g_armed; g_armed = 1;
    pthread_mutex_unlock(&g_mu);
    if (arm) dispatch_async(g_q, ^{ settle(); });
}

void totk_sr_set(int on) {
    on = on != 0;
    if (!atomic_load(&g_started)) { if (on) SLOG("state_reporting: only a game with its window reports states"); return; }
    static dispatch_once_t once;
    dispatch_once(&once, ^{ g_q = dispatch_queue_create("totk.state-report", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    dispatch_async(g_q, ^{
        if (on && !sr_load()) return;
        if (atomic_load(&g_totk_sr_on) == on) return;
        if (!on) {   // the trace keeps a domain's last label until the next one: say that the reports stopped
            report(g_life, @"reportingOff", g_ident);
            report(g_gfx, @"reportingOff", nil);
            g_life_label = @"reportingOff"; g_have_reported = 0;
        }
        atomic_store(&g_totk_sr_on, on);
        SLOG("state reports %s (domains org.macnativegames.Lifecycle, org.macnativegames.Graphics)", on ? "on" : "off");
        if (on) { g_life_label = nil; g_have_reported = 0; atomic_fetch_add(&g_epoch, 1); life_event(); }
    });
}

void totk_sr_start(const char *game, const char *version) {
    if (atomic_exchange(&g_started, 1)) return;
    NSMutableDictionary *ident = [@{@"game": @(game), @"gameVersion": @(version), @"shaderChain": @DMSH_CHAIN,
                                    @"settings": settings_in_force()} mutableCopy];
    const char *commit = getenv("TOTK_SOURCE_COMMIT");   // the build's commit, as the app recorded it (build_app.py, the launcher)
    if (commit && *commit && strlen(commit) <= 40) ident[@"sourceCommit"] = @(commit);
    NSString *uuid = runtime_uuid(), *model = sysctl_string("hw.model"), *chip = sysctl_string("machdep.cpu.brand_string");
    if (uuid) ident[@"runtimeUUID"] = uuid;
    if (model) ident[@"hostModel"] = model;
    if (chip) ident[@"hostChip"] = chip;
    ident[@"hostOS"] = NSProcessInfo.processInfo.operatingSystemVersionString;
    g_ident = ident;
    const char *e = getenv("TOTK_STATE_REPORTING");
    if (e && !strcmp(e, "1")) totk_sr_set(1);
}

void totk_sr_warmup(int archive_valid) { atomic_store(&g_archive, archive_valid != 0); life_event(); }
void totk_sr_picture(int key, int visible) {
    atomic_store(&g_key, key != 0); atomic_store(&g_visible, visible != 0); atomic_store(&g_picture, 1);
    life_event();
}
void totk_sr_window(int key, int visible) { atomic_store(&g_key, key != 0); atomic_store(&g_visible, visible != 0); life_event(); }
void totk_sr_settings(int open) { atomic_store(&g_settings, open != 0); life_event(); }
void totk_sr_view(int w, int h, int display_hz, int fullscreen) {   // the next game frame picks them up (totk_sr_frame)
    atomic_store(&g_view_w, w); atomic_store(&g_view_h, h); atomic_store(&g_display_hz, display_hz); atomic_store(&g_fullscreen, fullscreen != 0);
}
void totk_sr_closing(void) {
    atomic_store(&g_closing, 1);
    if (!totk_sr_on()) return;
    life_event();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);   // the process ends right after: wait a little, at the waiter's priority
    dispatch_block_t b = dispatch_block_create_with_qos_class(DISPATCH_BLOCK_ENFORCE_QOS_CLASS, QOS_CLASS_USER_INTERACTIVE, 0, ^{ dispatch_semaphore_signal(done); });
    dispatch_async(g_q, b);
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC))) SLOG("closing: the last state report did not finish in 200 ms");
}
#endif

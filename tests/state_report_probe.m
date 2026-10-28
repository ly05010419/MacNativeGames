/* The state reports (games/totk/third_party/d2r_gfx/gfx/nvn/totk_state_report.m) against a fake SRStateReporter that
   records every call, without a game or a window; tests/test_state_report.py builds it three ways (fake, framework
   missing, the real framework on macOS 27) and runs one scenario per argument. */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_state_report.m"
#include <assert.h>

static NSMutableArray<NSArray *> *calls;   // {domain, label or NSNull, stable or NSNull, volatile or NSNull, time}
static int throwing;
@interface FakeSRStateReporter : NSObject
@property NSString *domain;
@end
@implementation FakeSRStateReporter
+ (id)reporterForDomain:(NSString *)domain {
    static NSMutableDictionary *all;
    @synchronized(self) {
        if (!all) all = [NSMutableDictionary dictionary];
        if (!all[domain]) { FakeSRStateReporter *r = [FakeSRStateReporter new]; r.domain = domain; all[domain] = r; }
        return all[domain];
    }
}
static void check_values(NSDictionary *d) {   // what the real reporter accepts (SRStateReporter.h)
    for (id v in d.allValues)
        if (![v isKindOfClass:NSString.class] && ![v isKindOfClass:NSNumber.class] && ![v isKindOfClass:NSDate.class])
            [NSException raise:NSInvalidArgumentException format:@"metadata value of class %@", [v class]];
}
- (void)reportTransitionToStateLabel:(NSString *)label stableMetadata:(NSDictionary *)stable volatileMetadata:(NSDictionary *)vol {
    if (throwing) [NSException raise:NSInvalidArgumentException format:@"fake refusal"];
    assert(!label || label.length);
    check_values(stable); check_values(vol);
    @synchronized(calls) { [calls addObject:@[self.domain, label ?: NSNull.null, stable ?: NSNull.null, vol ?: NSNull.null, @((now_ns() / 1e9))]]; }
}
@end

/* nvn_mtl.m's side: the configuration, the stage marks and the counters. */
static totk_sr_gfx fake_pic;
static uint64_t frames, up_at, aa_at;
static int upscaling, antialiasing;   // a stage ran this frame
void mtl_sr_picture(totk_sr_gfx *s, uint64_t *upscaled_at, uint64_t *antialiased_at) {
    *s = fake_pic;
    if (upscaling) up_at = frames;
    if (antialiasing && frames % 2) aa_at = frames;
    *upscaled_at = up_at; *antialiased_at = aa_at;
}
void mtl_sr_counters(totk_sr_counters *c) { *c = (totk_sr_counters){frames, frames / 2, 7, 3, 4, 5, 6000000, 64u << 20, 1600, 896}; }

static NSArray *of(NSString *domain, int field) {
    NSMutableArray *out = [NSMutableArray array];
    @synchronized(calls) { for (NSArray *c in calls) if ([c[0] isEqualToString:domain]) [out addObject:c[field]]; }
    return out;
}
static NSArray *labels(NSString *domain) { return of(domain, 1); }
static NSArray *last(NSString *domain) {
    @synchronized(calls) { for (NSArray *c in calls.reverseObjectEnumerator) if ([c[0] isEqualToString:domain]) return c; }
    return nil;
}
static void drain(void) { if (g_q) dispatch_sync(g_q, ^{}); }
static void frame(int cap, int src, int own, unsigned ticks) { frames++; if (totk_sr_on()) totk_sr_frame(cap, src, own, 120, ticks); }
static void run_frames(double seconds) {   // the game thread at 120 fps, uncapped at 120
    for (double t = 0; t < seconds; t += 1 / 120.0) { frame(120, SR_CAP_UNCAPPED, 0, 1); usleep(8333); }
    drain();
}
static void expect(NSArray *got, NSArray *want, const char *what) {
    if (![got isEqualToArray:want]) { fprintf(stderr, "FAIL %s: got %s want %s\n", what, got.description.UTF8String, want.description.UTF8String); exit(1); }
}
#define L @"org.macnativegames.Lifecycle"
#define G @"org.macnativegames.Graphics"
#define L1 @"1920x1080 | cap 120 | window@120Hz"

int main(int argc, char **argv) {
    @autoreleasepool {
        calls = [NSMutableArray array];
        const char *scenario = argc > 1 ? argv[1] : "";
        fake_pic = (totk_sr_gfx){.out_w = 1920, .out_h = 1080};
        totk_sr_view(3024, 1700, 120, 0);
        if (!strcmp(scenario, "off")) {   // TOTK_STATE_REPORTING unset: no reports, no queue, the frame hook never runs
            totk_sr_start("totk", "1.4.3");
            totk_sr_warmup(1); totk_sr_picture(1, 1); totk_sr_window(0, 1); totk_sr_settings(1); run_frames(0.3); totk_sr_closing();
            assert(!totk_sr_on() && !g_q && !calls.count && !atomic_load(&g_ticks[1]));
        } else if (!strcmp(scenario, "headless")) {   // no window (probes, TOTK_HEADLESS): the flag cannot switch it on
            totk_sr_set(1); drain();
            assert(!totk_sr_on() && !g_q && !calls.count);
        } else if (!strcmp(scenario, "lifecycle")) {
            totk_sr_start("totk", "1.4.3"); drain();
            totk_sr_warmup(1); drain(); totk_sr_picture(1, 1); drain(); totk_sr_window(0, 1); drain();
            totk_sr_settings(1); drain(); totk_sr_settings(0); drain(); totk_sr_window(0, 0); drain();
            totk_sr_window(1, 1); drain(); totk_sr_window(1, 1); drain();   // no change: no report
            totk_sr_closing();
            expect(labels(L), @[@"starting", @"shaderWarmup (archive reused)", @"focused", @"unfocused", @"settings", @"unfocused",
                                @"occluded", @"focused", @"closing"], "lifecycle");
            NSDictionary *st = calls[1][2], *vol = calls[1][3];
            assert([st[@"game"] isEqualToString:@"totk"] && [st[@"gameVersion"] isEqualToString:@"1.4.3"] && st[@"shaderChain"]);
            assert([st[@"settings"] isKindOfClass:NSString.class] && [st[@"hostOS"] length] && [st[@"hostModel"] length] && st[@"runtimeUUID"]);
            const char *uuid = getenv("PROBE_UUID");   // the probe's own LC_UUID, also with a library inserted before it
            assert(!uuid || [st[@"runtimeUUID"] isEqualToString:@(uuid)]);
            assert([st isEqualToDictionary:calls[0][2]]);   // the same stable metadata for every Lifecycle state
            for (NSString *k in @[@"gameFrames", @"generatedFrames", @"mergedFrames", @"upscaledFrames", @"antiAliasedFrames", @"paceTicks1",
                                  @"paceTicks2", @"paceTicks3", @"shaderTranslations", @"librariesCreated", @"pipelinesCreated", @"creationMs",
                                  @"footprintMiB", @"metalAllocatedMiB"])
                assert([vol[k] isKindOfClass:NSNumber.class]);
            assert([vol[@"scene"] isEqualToString:@"1600x896"] && [vol[@"creationMs"] intValue] == 6 && [vol[@"metalAllocatedMiB"] intValue] == 64);
            assert([vol[@"sampledAt"] isKindOfClass:NSDate.class] && fabs([vol[@"sampledAt"] timeIntervalSinceNow]) < 5);
            assert(!labels(G).count);   // no game frame yet
        } else if (!strcmp(scenario, "graphics")) {
            totk_sr_start("botw", "1.9.0"); totk_sr_picture(1, 1);
            run_frames(0.8);
            expect(labels(G), @[L1], "first graphics state");
            // a window dragged larger: 30 sizes, two frames each, half a second on one 120 Hz schedule. Paced per frame
            // (usleep), a slow runner stretched it past MAXWAIT_NS, and a configuration still changing then is reported.
            uint64_t d0 = now_ns();
            for (int i = 0; i < 30; i++) {
                totk_sr_view(2000 + 10 * i, 1700, 120, 0);
                for (int k = 1; k <= 2; k++) {
                    frame(120, SR_CAP_UNCAPPED, 0, 1);
                    uint64_t next = d0 + (uint64_t)(2 * i + k) * 1000000000ull / 120, now = now_ns();
                    if (next > now) usleep((useconds_t)((next - now) / 1000));
                }
            }
            drain();
            if (now_ns() - d0 >= MAXWAIT_NS) { fprintf(stderr, "FAIL drag merged: the drag took %.2f s, the runner stalled\n", (now_ns() - d0) / 1e9); exit(1); }
            fake_pic = (totk_sr_gfx){.out_w = 2560, .out_h = 1440, .upscale = 2, .taa = 1, .fi_mode = MFX_50};
            totk_sr_view(3840, 2160, 160, 1);   // full screen on another display
            upscaling = antialiasing = 1;
            uint64_t f0 = frames, up0 = atomic_load(&g_upscaled), aa0 = atomic_load(&g_antialiased), t2 = atomic_load(&g_ticks[2]);
            for (double t = 0; t < 0.8; t += 1 / 120.0) { frame(50, SR_CAP_INTERP, 1, 2); usleep(8333); }
            drain();
            NSString *l2 = @"2560x1440 | MetalFX+output | TAA | FI 50+50 | held 50 own clock | fullscreen@160Hz (paced 120Hz)";
            NSArray *g = labels(G);
            if (!(g.count <= 3 && [g.firstObject isEqualToString:L1] && [g.lastObject isEqualToString:l2])) expect(g, @[L1, l2], "drag merged");
            NSDictionary *st = last(G)[2];
            assert([st[@"interpolation"] isEqualToString:@"50+50"] && [st[@"upscaling"] isEqualToString:@"metalfx+output"]);
            assert([st[@"fpsCapSource"] isEqualToString:@"interpolation"] && [st[@"fpsCap"] intValue] == 50 && [st[@"ownClock"] intValue] == 1);
            assert([st[@"pacingHz"] intValue] == 120 && [st[@"fullscreen"] intValue] == 1 && [st[@"view"] isEqualToString:@"3840x2160"]);
            assert(atomic_load(&g_upscaled) - up0 == frames - f0 && labs((long)(atomic_load(&g_antialiased) - aa0) - (long)(frames - f0) / 2) <= 1);
            assert(atomic_load(&g_ticks[2]) - t2 == frames - f0);
            upscaling = antialiasing = 0;   // a menu: the stages stop, the configuration does not change
            for (double t = 0; t < 0.8; t += 1 / 120.0) { frame(50, SR_CAP_INTERP, 1, 2); usleep(8333); }
            drain();
            assert([last(G)[1] isEqualToString:l2] && labels(G).count == g.count);
            fake_pic.fi_mode = MFX_OFF;   // one change after a quiet second: reported at once
            frame(50, SR_CAP_INTERP, 1, 2); usleep(60000); drain();
            expect(@[last(G)[1]], @[@"2560x1440 | MetalFX+output | TAA | held 50 own clock | fullscreen@160Hz (paced 120Hz)"], "leading edge");
            assert((now_ns() / 1e9) - [last(G)[4] doubleValue] < 0.2 + 0.06);
            fake_pic = (totk_sr_gfx){.out_w = 1920, .out_h = 1080, .aspect_milli = 2333, .present_path = 2};
            for (double t = 0; t < 0.8; t += 1 / 120.0) { frame(120, SR_CAP_UNCAPPED_INTERVAL, 0, 1); usleep(8333); }
            drain();
            expect(@[last(G)[1]], @[@"1920x1080 | 21:9 | cap 120/game interval | present thread (display link stalled) | fullscreen@160Hz"], "aspect, path");
            fake_pic = (totk_sr_gfx){.out_w = 2560, .out_h = 1440, .temporal = 1};   // TOTK_MFX_TEMPORAL
            for (double t = 0; t < 0.8; t += 1 / 120.0) { frame(120, SR_CAP_UNCAPPED_INTERVAL, 0, 1); usleep(8333); }
            drain();
            expect(@[last(G)[1]], @[@"2560x1440 | MetalFX temporal | cap 120/game interval | fullscreen@160Hz"], "temporal");
            assert([last(G)[2][@"temporalUpscaling"] isEqualToString:@"on"] && [last(G)[2][@"upscaling"] isEqualToString:@"off"]);
        } else if (!strcmp(scenario, "toggle")) {   // flag state_reporting 0 / 1 within one session
            totk_sr_start("totk", "1.4.3"); drain(); totk_sr_picture(1, 1); run_frames(0.8);
            totk_sr_set(0); drain(); run_frames(0.3);
            totk_sr_window(0, 1); drain();   // while off: remembered, not reported
            totk_sr_set(1); drain(); run_frames(0.8);
            expect(labels(L), @[@"starting", @"focused", @"reportingOff", @"unfocused"], "toggle lifecycle");
            expect(labels(G), @[L1, @"reportingOff", L1], "toggle graphics");
        } else if (!strcmp(scenario, "refusal")) {   // the reporter throws: reports stop, the game goes on
            totk_sr_start("totk", "1.4.3"); drain();
            throwing = 1; totk_sr_picture(1, 1); drain();
            assert(!totk_sr_on());
            run_frames(0.2); totk_sr_closing();
        } else if (!strcmp(scenario, "missing")) {   // built with a framework path that does not exist (macOS 26)
            totk_sr_start("totk", "1.4.3"); drain(); totk_sr_picture(1, 1); run_frames(0.2); totk_sr_closing();
            assert(!totk_sr_on() && !calls.count && !atomic_load(&g_ticks[1]));
        } else if (!strcmp(scenario, "real")) {   // the system's StateReporting (macOS 27): accepted without an exception
            totk_sr_start("totk", "1.4.3"); drain();
            assert(totk_sr_on());
            totk_sr_warmup(0); drain(); totk_sr_picture(1, 1); run_frames(0.8);
            totk_sr_window(0, 1); drain(); totk_sr_closing();
            assert(totk_sr_on());
        } else { fprintf(stderr, "unknown scenario %s\n", scenario); return 2; }
        printf("pass: state reports %s\n", scenario);
    }
    return 0;
}

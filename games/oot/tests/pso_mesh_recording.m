#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The runner includes the current production functions verbatim. Only the
   unrelated renderer/capture dependencies are replaced here; no GPU is used. */
static NSString *g_shmap_path, *fixture_cache;
static FILE *g_pso_user, *g_pso_shared;
static int g_pso_user_checked, g_pso_shared_checked, fixture_warm_shared;
static NSMutableSet<NSNumber *> *g_pso_user_seen, *g_pso_shipped_seen;
static NSSet<NSString *> *g_pso_unretained_mesh_lists;
static _Atomic uint64_t g_collect_new[3], g_collect_last_ns;
static NSMutableArray<NSString *> *captured;
static int warm_shared(void) { return fixture_warm_shared; }
static NSString *totk_rt_dir(void) { return fixture_cache; }
static void pso_capture(const char *line) { [captured addObject:@(line)]; }
#include "pso_mesh_recording_production.inc"

static unsigned checks;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void write_text(NSString *path, NSString *text) {
    NSError *error = nil;
    CHECK([text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&error]);
}
static NSString *read_text(NSString *path) {
    NSError *error = nil;
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    CHECK(text != nil);
    return text;
}
static void close_files(void) {
    if (g_pso_user) fclose(g_pso_user);
    if (g_pso_shared) fclose(g_pso_shared);
    g_pso_user = g_pso_shared = NULL;
    g_pso_user_checked = g_pso_shared_checked = 0;
}

typedef NS_ENUM(int, WarmMode) {
    ValidLowMemory, ValidRetained, NoArchive, ColdTitleOnly, CustomManifest, WarmOff
};

static void exercise(WarmMode mode, int local_copy) {
    @autoreleasepool {
        close_files();
        g_pso_user_seen = g_pso_shipped_seen = nil;
        g_pso_unretained_mesh_lists = nil;
        captured = [NSMutableArray array];
        for (int i = 0; i < 3; i++) atomic_store(&g_collect_new[i], 0);
        atomic_store(&g_collect_last_ns, 0);
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        CHECK([fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:NULL]);
        g_shmap_path = [root stringByAppendingPathComponent:@"shader-map.tsv"];
        fixture_cache = root;
        NSString *manifest = [root stringByAppendingPathComponent:@"pso_manifest.tsv"];
        NSString *list = [root stringByAppendingPathComponent:@"pso_list.tsv"];
        NSString *user = [root stringByAppendingPathComponent:@"user.tsv"];
        NSString *shared = [root stringByAppendingPathComponent:@"pso_collected.tsv"];
        NSString *mesh = @"M\ttask-a\t001122";
        NSString *other_state = @"M\ttask-a\t001133";
        NSString *local = local_copy ? [mesh stringByAppendingString:@"\n"] : @"";
        NSString *header = @"# mesh regression fixture";
        write_text(manifest, [NSString stringWithFormat:@"%@\n\nS\tshader-a\t0\nR\trender-a\nC\tcompute-a\nT\ttile-a\nB\tborder-a\nMAYBE\tordinary\n%@\n%@\n", header, mesh, other_state]);
        write_text(list, @"# other shipped list\nM\ttask-list\t00\nR\trender-list\n");
        write_text(user, local_copy == 1 ? local : @"");
        write_text(shared, local_copy == 2 ? local : @"");
        CHECK(setenv("DM_PSO_USER", user.fileSystemRepresentation, 1) == 0);
        fixture_warm_shared = mode == ValidRetained;

        /* Inputs are the actual retained/shipped path sets after each startup
           policy. The production helper sees overrides as an empty shipped set. */
        NSSet *shipped = [NSSet setWithArray:@[manifest, list]];
        NSArray *retained = @[manifest, list, user, shared];
        if (mode == ValidLowMemory || mode == ColdTitleOnly) retained = @[user, shared];
        if (mode == ColdTitleOnly) retained = @[[root stringByAppendingPathComponent:@"pso_title.tsv"]];
        if (mode == CustomManifest) shipped = [NSSet set];
        if (mode != WarmOff) pso_user_mesh_warm_paths(shipped, retained);
        /* Mutating the caller's sets must not mutate the published snapshot. */
        NSMutableSet *caller_set = [shipped mutableCopy];
        if (mode != WarmOff) pso_user_mesh_warm_paths(caller_set, retained);
        [caller_set removeAllObjects];

        BOOL promote = NO;
#if defined(GAME_OOT)
        promote = mode == ValidLowMemory || mode == ColdTitleOnly;
#endif
        CHECK([g_pso_unretained_mesh_lists containsObject:manifest] == promote);
        CHECK(g_pso_unretained_mesh_lists == nil || ![g_pso_unretained_mesh_lists isKindOfClass:NSMutableSet.class]);
        pso_user_record("%s\n", mesh.UTF8String);
        pso_user_record("%s\n", mesh.UTF8String);
        NSString *mesh_line = [mesh stringByAppendingString:@"\n"];
        NSString *expected_user = local_copy == 1 ? local : @"";
        NSString *expected_shared = local_copy == 2 ? local : @"";
        if (promote && !local_copy) {
            expected_user = [expected_user stringByAppendingString:mesh_line];
            expected_shared = [expected_shared stringByAppendingString:mesh_line];
        }
        CHECK([read_text(user) isEqualToString:expected_user]);
        CHECK([read_text(shared) isEqualToString:expected_shared]);
        CHECK(captured.count == 2); /* capture still sees duplicates before dedup */
        CHECK(![g_pso_user_seen containsObject:seen_key_str(header)]);
        CHECK(![g_pso_user_seen containsObject:seen_key_str(@"")]);

        /* Same shader names, a different complete draw key: preserve the row
           byte for byte instead of deduplicating just the shader pair. */
        pso_user_record("%s\n", other_state.UTF8String);
        pso_user_record("%s\n", other_state.UTF8String);
        if (promote) {
            expected_user = [expected_user stringByAppendingFormat:@"%@\n", other_state];
            expected_shared = [expected_shared stringByAppendingFormat:@"%@\n", other_state];
        }
        CHECK([read_text(user) isEqualToString:expected_user]);
        CHECK([read_text(shared) isEqualToString:expected_shared]);

        /* Every other row kind keeps the prior shipped-manifest behavior. */
        for (NSString *row in @[@"S\tshader-a\t0", @"R\trender-a", @"C\tcompute-a", @"T\ttile-a", @"B\tborder-a", @"MAYBE\tordinary"]) {
            pso_user_record("%s\n", row.UTF8String);
            CHECK([g_pso_user_seen containsObject:seen_key_str(row)]);
        }
        CHECK([read_text(user) isEqualToString:expected_user]);
        CHECK([read_text(shared) isEqualToString:expected_shared]);
        for (int i = 0; i < 3; i++) CHECK(atomic_load(&g_collect_new[i]) == 0);

        /* The other shipped list's existing shared-retention policy stays as
           before: retained rows suppress recording; otherwise draws are saved. */
        NSString *list_mesh = @"M\ttask-list\t00\n";
        pso_user_record("%s", list_mesh.UTF8String);
        pso_user_record("%s", list_mesh.UTF8String);
        if (!fixture_warm_shared) {
            expected_user = [expected_user stringByAppendingString:list_mesh];
            expected_shared = [expected_shared stringByAppendingString:list_mesh];
        }
        CHECK([read_text(user) isEqualToString:expected_user]);
        CHECK([read_text(shared) isEqualToString:expected_shared]);
        close_files();
        CHECK([fm removeItemAtPath:root error:NULL]);
    }
}

int main(void) {
    @autoreleasepool {
        for (WarmMode mode = ValidLowMemory; mode <= WarmOff; mode++)
            for (int local = 0; local < 3; local++) exercise(mode, local);
        printf("pso-mesh-recording: PASS %u checks (%s)\n", checks,
#if defined(GAME_OOT)
               "OoT"
#else
               "other game"
#endif
        );
    }
    return 0;
}

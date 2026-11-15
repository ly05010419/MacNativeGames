#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdint.h>
// The policy only reads shader identity; no Metal device is required by these tests.
typedef struct { char key[80]; int stage; } NvShader;
#include "nvn_pso_skey.h"
static NSString *par_hash(const void *bytes, size_t n) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bytes, (CC_LONG)n, digest);
    NSMutableString *out = [NSMutableString string];
    for (int i = 0; i < 20; ++i) [out appendFormat:@"%02x", digest[i]];
    return out;
}
#include "nvn_warm_policy.h"

static int checks, failures;
static void expect(BOOL ok, const char *why) {
    ++checks;
    if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\n", why); }
}
static NSString *hex(pso_skey *sk) {
    char text[sizeof(*sk) * 2 + 1]; pso_skey_hex(sk, text); return @(text);
}
static NSString *rowkey(NSString *row) { return warm_archive_row_key([row componentsSeparatedByString:@"\t"]); }

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    NSString *mode = @(argv[1]);
    if ([mode isEqualToString:@"lists"]) {
        NSString *title = @"/seed/pso_title.tsv", *user = @"/profile/pso_user.tsv";
        NSArray *paths = @[title, @"/seed/pso_list.tsv", @"/seed/pso_manifest.tsv", user,
                           @"/collected/pso_user.tsv", @"/seed/shader-seed/pso_manifest.tsv", @"/seed/renamed.tsv"];
        NSArray *archive = nil;
        NSArray *keep = warm_resident_lists(paths, title, user, YES, NO, &archive);
        expect([keep isEqual:@[title, user]], "valid iOS archive keeps only explicit title and local user roles");
        expect(archive.count == 5 && [archive containsObject:@"/seed/pso_manifest.tsv"], "duplicate manifest stays archive-only");
        expect([archive containsObject:@"/collected/pso_user.tsv"], "shared user basename is not local identity");
        expect([archive containsObject:@"/seed/renamed.tsv"], "renaming a shared list cannot bypass low-memory policy");
        keep = warm_resident_lists(paths, title, user, NO, YES, &archive);
        expect([keep isEqual:@[title]] && archive.count == 6, "cold low-memory start retains only title, fills every other list");
        keep = warm_resident_lists(paths, title, user, YES, YES, &archive);
        expect([keep isEqual:paths] && archive == nil, "Mac/full warmup retains all lists in original order");
        keep = warm_resident_lists(@[@"/custom/title.tsv"], title, user, YES, NO, &archive);
        expect(!keep.count && archive.count == 1, "custom manifest still fills before title without full residency");
        keep = warm_resident_lists(@[], title, nil, NO, NO, &archive);
        expect(!keep.count && archive == nil, "missing lists are safe");
    } else if ([mode isEqualToString:@"fill"]) {
        pso_skey sk = {0}; strcpy(sk.vs, "vertex_b"); strcpy(sk.fs, "fragment_b"); sk.fs_stage = 1;
        sk.k.cfmt[0] = 80; sk.k.samples = 1; sk.k.chmask = 15;
        NSString *render = [@"R\t" stringByAppendingString:hex(&sk)];
        NSString *border = [NSString stringWithFormat:@"B\t%@\tborder-v\t-", hex(&sk)];
        NSString *tile = [NSString stringWithFormat:@"T\t%@\t0:1", hex(&sk)];
        NSString *compute = @"C\tcompute_b";
        NSString *mesh = [NSString stringWithFormat:@"M\t-\t%@", hex(&sk)];
        // Independently construct the keys the archive writer records, including each variant's suffix.
        expect([rowkey(render) isEqual:par_hash(&sk, sizeof sk)], "render lookup matches archive bytes");
        expect([rowkey(compute) isEqual:[@"c" stringByAppendingString:par_hash("compute_b", 9)]], "compute key includes c prefix");
        pso_skey ak = sk; ak.k.a2c |= PSO_VBORDER;
        NSMutableData *data = [NSMutableData dataWithBytes:&ak length:sizeof ak];
        [data appendData:[@"B:border-v\t-" dataUsingEncoding:NSUTF8StringEncoding]];
        expect([rowkey(border) isEqual:par_hash(data.bytes, data.length)], "border lookup matches variant archive key");
        ak = sk; ak.fz = 1;
        data = [NSMutableData dataWithBytes:&ak length:sizeof ak];
        [data appendData:[@"T:0:1" dataUsingEncoding:NSUTF8StringEncoding]];
        expect([rowkey(tile) isEqual:par_hash(data.bytes, data.length)], "tile lookup matches variant archive key");
        NSArray *rows = @[render, compute, border, tile, mesh, @"S\tunused_b\t0", @"R\tbad", @"C\t", @"#comment", @""];
        NSSet *cached = [NSSet setWithArray:@[rowkey(render), rowkey(compute), rowkey(border), rowkey(tile)]];
        BOOL (^have)(NSString *) = ^BOOL(NSString *key) { return [cached containsObject:key]; };
        uint64_t hits;
        NSArray *pending = warm_fill_pending(rows, NO, NO, have, &hits);
        expect(hits == 4 && !pending.count, "archive hits and unsupported mesh cause ZERO shader preloads");
        // The loader receives only pending: a missing pipeline must still be compiled and then hit on the next run.
        NSString *missing = @"C\tnew_compute_b";
        pending = warm_fill_pending([rows arrayByAddingObject:missing], NO, NO, have, &hits);
        expect(hits == 4 && [pending isEqual:@[missing]], "only a cache miss reaches shader loading");
        cached = [cached setByAddingObject:rowkey(missing)];
        pending = warm_fill_pending(@[missing], NO, NO, ^BOOL(NSString *key) { return [cached containsObject:key]; }, &hits);
        expect(hits == 1 && !pending.count, "second run reuses newly archived pipeline");
        pending = warm_fill_pending(rows, YES, NO, have, &hits);
        expect(!hits && [pending isEqual:@[render, compute, border, tile]], "packaging recheck still rebuilds cached descriptors");
        pending = warm_fill_pending(rows, NO, YES, have, &hits);
        expect(hits == 4 && [pending isEqual:@[mesh]], "supported mesh keeps its own build path");
        sk.k.a2c |= PSO_HOST_RECT;
        expect(rowkey([@"R\t" stringByAppendingString:hex(&sk)]) == nil, "host-only render rows cannot preload shaders");
        sk.k.a2c = 0; sk.fz = 1;
        expect(rowkey([@"R\t" stringByAppendingString:hex(&sk)]) == nil, "unreconstructable fragment variants stay excluded");
    } else return 2;
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
} }

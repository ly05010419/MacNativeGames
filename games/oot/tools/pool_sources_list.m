// build/pool_sources.tsv: the pool proof (nvn_pool_sources) of every seed shader with a pooled read, made once at
// packaging (2026-11-18, user: "a"). The proof is a prover over the whole MSL text: one 200 KB shader takes 40-70 ms,
// 99.6 s over Ocarina of Time's 40,659 translations, and the runtime paid it again at every start's warm-up and at every
// first load in play (80 ms frames on the second save). Built from the runtime's own nvn_pool_sources.m and nvn_refl.m
// (games/oot/Makefile), and the bindings are marked as the runtime marks them first (nvn_refl_mark_stores), so a row is
// what the runtime would prove. The runtime uses a row only for the MSL and reflection it was made from (the digest).
//
// usage: pool-sources-list <seed folder> <out.tsv> <seed manifest sha256> <nvn_pool_sources.m sha256> [--verify]
//   --verify: every row decoded again (nvn_pool_sources_decode) must give the same bindings and the same row.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <time.h>
#include "nvn_pool_sources.h"

static NSString *digest(NSData *msl, NSData *json) {   // as nvn_mtl.m pool_list_digest: the MSL's bytes, then the json's
    CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, msl.bytes, (CC_LONG)msl.length); CC_SHA256_Update(&c, json.bytes, (CC_LONG)json.length);
    unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(md, &c);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", md[i]];
    return hex;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 5 || argc > 6 || (argc == 6 && strcmp(argv[5], "--verify"))) {
        fprintf(stderr, "usage: pool-sources-list <seed folder> <out.tsv> <seed manifest sha256> <nvn_pool_sources.m sha256> [--verify]\n");
        return 2;
    }
    NSString *dir = @(argv[1]); BOOL verify = argc == 6;
    NSArray<NSString *> *names = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]
        filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF ENDSWITH '_b.json'"]] sortedArrayUsingSelector:@selector(compare:)];
    if (!names.count) { fprintf(stderr, "pool-sources-list: no *_b.json in %s\n", argv[1]); return 1; }
    NSUInteger n = names.count;
    __strong NSString **rows = (__strong NSString **)calloc(n, sizeof(NSString *));
    __block _Atomic int bad = 0; __block _Atomic uint64_t proved_ns = 0;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dispatch_apply(n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { @autoreleasepool {
        NSString *name = names[i], *key = [name substringToIndex:name.length - 5];
        NSData *jd = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:name]];
        NSData *md = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]]];
        NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
        NSString *src = md ? [[NSString alloc] initWithData:md encoding:NSUTF8StringEncoding] : nil;
        if (![j isKindOfClass:NSDictionary.class] || !src) return;   // the runtime proves nothing for these either
        nvn_refl rf; char err[160];
        if (nvn_refl_from_json(j, &rf, err, sizeof err)) return;
        NSArray *bindings = j[@"bindings"];
        nvn_refl_mark_stores(&rf, bindings, src);   // as shader_load_mode does before the proof
        if (!nvn_pool_sources_pooled(&rf, bindings, src)) { free(rf.c1); return; }   // nothing to prove: the runtime's check is cheap
        uint64_t a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        NSDictionary *result = nvn_pool_sources(&rf, bindings, src);
        atomic_fetch_add(&proved_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - a);
        NSString *row = nvn_pool_sources_encode(&rf, result);
        if (verify) {
            nvn_refl back = rf; uint64_t bits[NVN_REFL_MAXB];
            for (int b = 0; b < rf.nb; b++) bits[b] = rf.b[b].pool_sources;
            NSDictionary *again = nvn_pool_sources_decode(&back, row);
            int same = again != nil && [nvn_pool_sources_encode(&back, again) isEqualToString:row];
            for (int b = 0; b < rf.nb && same; b++) same = back.b[b].pool_sources == bits[b];
            if (!same) { atomic_fetch_add(&bad, 1); fprintf(stderr, "pool-sources-list: %s does not decode to itself\n", key.UTF8String); }
        }
        rows[i] = [NSString stringWithFormat:@"%@\t%@\t%@", key, digest(md, jd), row];
        free(rf.c1);
    } });
    NSMutableString *out = [NSMutableString stringWithFormat:@"# nvn pool sources v1 seed=%s scan=%s\n", argv[3], argv[4]];
    NSUInteger listed = 0;
    for (NSUInteger i = 0; i < n; i++) if (rows[i]) { [out appendString:rows[i]]; [out appendString:@"\n"]; listed++; rows[i] = nil; }
    free(rows);
    if (bad) { fprintf(stderr, "pool-sources-list: %d rows do not decode to themselves: no list written\n", (int)bad); return 1; }
    NSString *tmp = [@(argv[2]) stringByAppendingString:@".tmp"];
    if (![out writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil] || rename(tmp.fileSystemRepresentation, argv[2])) {
        fprintf(stderr, "pool-sources-list: cannot write %s\n", argv[2]); return 1;
    }
    printf("pool sources: %lu of %lu shaders have a pooled read, proven in %.1f s of proof (%.1f s wall)%s\n", (unsigned long)listed,
           (unsigned long)n, atomic_load(&proved_ns) / 1e9, (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9, verify ? ", every row decoded again" : "");
    return 0;
} }

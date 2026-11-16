#pragma once
#include "nvn_warm_resident.h"   /* which pipelines stay in memory: the modes and the kinds' rows */

/* List identity is its role, not its basename: a seed's manifest can contain exactly the same pipelines as the shared
   list. With a low-memory archive only the title and this device's observed pipelines stay resident. All other lists
   still finish archive filling before the title. Without an archive only the title stays resident. */
static NSArray<NSString *> *warm_resident_lists(NSArray<NSString *> *paths, NSString *title, NSString *user,
                                               BOOL valid, BOOL shared, NSArray<NSString *> **archive) {
    NSMutableArray<NSString *> *keep = [NSMutableArray array], *fill = [NSMutableArray array];
    for (NSString *path in paths) {
        BOOL resident = (valid && shared) || [path isEqualToString:title] || (valid && [path isEqualToString:user]);
        [(resident ? keep : fill) addObject:path];
    }
    *archive = fill.count ? fill : nil;
    return keep;
}

/* The same key as pso_archive_key, without instantiating a shader or pipeline. */
static NSString *warm_variant_archive_key(const pso_skey *sk, int vborder, int fz, NSString *variant) {
    pso_skey ak = *sk;
    memset(&ak.fz_desc, 0, sizeof ak.fz_desc);
    ak.fz = fz != 0;
    if (vborder) ak.k.a2c |= PSO_VBORDER;
    NSMutableData *bytes = [NSMutableData dataWithBytes:&ak length:sizeof ak];
    if (variant) [bytes appendData:[variant dataUsingEncoding:NSUTF8StringEncoding]];
    return par_hash(bytes.bytes, bytes.length);
}

static NSString *warm_archive_row_key(NSArray<NSString *> *c) {
    if (c.count < 2) return nil;
    NSString *type = c[0];
    if ([type isEqualToString:@"C"] && [c[1] length]) {
        const char *key = [c[1] UTF8String];
        return [@"c" stringByAppendingString:par_hash(key, strlen(key))];
    }
    BOOL render = [type isEqualToString:@"R"], border = [type isEqualToString:@"B"] && c.count >= 4;
    BOOL tile = [type isEqualToString:@"T"] && c.count >= 3;
    if (!render && !border && !tile) return nil;
    pso_skey sk;
    if (pso_skey_parse([c[1] UTF8String], &sk) || sk.fz || (!render && sk.merged)) return nil;
    if (render) return warm_variant_archive_key(&sk, 0, 0, nil);
    if (border) return warm_variant_archive_key(&sk, ![c[2] isEqualToString:@"-"], ![c[3] isEqualToString:@"-"],
                                               [NSString stringWithFormat:@"B:%@\t%@", c[2], c[3]]);
    return warm_variant_archive_key(&sk, 0, 1, [@"T:" stringByAppendingString:c[2]]);
}

/* Filter BEFORE warm_load_rows: even a cache hit used to load all of its libraries on the multi-threaded path. Keep
   packaging's recheck: it must rebuild descriptors to validate them against the archive. Mesh has its own key path. */
static NSArray<NSString *> *warm_fill_pending(NSArray<NSString *> *rows, BOOL recheck, BOOL mesh,
                                             BOOL (^have)(NSString *), uint64_t *hits) {
    NSMutableArray<NSString *> *pending = [NSMutableArray array];
    *hits = 0;
    for (NSString *line in rows) {
        NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
        if ([c[0] isEqualToString:@"M"]) {
            if (mesh && c.count >= 3) [pending addObject:line];
            continue;
        }
        NSString *key = warm_archive_row_key(c);
        if (!key) continue;
        if (!recheck && have(key)) ++*hits;
        else [pending addObject:line];
    }
    return pending;
}

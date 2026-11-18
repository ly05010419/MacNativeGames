// A player's shader list contribution (pso_export.h).
#import "pso_export.h"

static BOOL key_ok(const char *s, size_t n) {   // 16 lowercase hex digits and "_b"
    if (n != 18 || s[16] != '_' || s[17] != 'b') return NO;
    for (int i = 0; i < 16; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return NO;
    return YES;
}
static NSString *raw_key(const uint8_t *b, BOOL empty_ok) {   // a key field of the pso_skey: the key, then zero bytes
    size_t n = strnlen((const char *)b, DM_PSO_KEYLEN);
    for (size_t i = n; i < DM_PSO_KEYLEN; i++) if (b[i]) return @"bytes after the key are not zero";
    if (!n && empty_ok) return nil;
    return key_ok((const char *)b, n) ? nil : @"not a shader key in the pipeline key";
}
static NSString *row_problem(NSString *row) {
    NSArray<NSString *> *c = [row componentsSeparatedByString:@"\t"];
    const char *k = c.count > 1 ? c[1].UTF8String : "";
    if (([c[0] isEqualToString:@"S"] && c.count == 3) || ([c[0] isEqualToString:@"C"] && c.count == 2)) {
        if (!key_ok(k, strlen(k))) return @"not a shader key";
        if (c.count == 3 && !(c[2].length == 1 && [c[2] characterAtIndex:0] >= '0' && [c[2] characterAtIndex:0] <= '5')) return @"stage not 0-5";
        return nil;
    }
    if ([c[0] isEqualToString:@"R"] && c.count == 2) {
        if (strlen(k) != 2 * DM_PSO_SKEY_SIZE) return [NSString stringWithFormat:@"not %d hex digits", 2 * DM_PSO_SKEY_SIZE];
        uint8_t b[DM_PSO_SKEY_SIZE];
        for (int i = 0; i < DM_PSO_SKEY_SIZE; i++) {
            int v = 0;
            for (int j = 0; j < 2; j++) { char h = k[2 * i + j]; int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1; if (d < 0) return @"not lowercase hex"; v = v * 16 + d; }
            b[i] = (uint8_t)v;
        }
        NSString *why = raw_key(b + DM_PSO_VS, NO) ?: raw_key(b + DM_PSO_FS, YES);
        if (why) return why;
        if (b[DM_PSO_VS_STAGE] > 5 || b[DM_PSO_FS_STAGE] > 5) return @"stage not 0-5";
        if (b[DM_PSO_MERGED] > 1 || b[DM_PSO_FZ] > 1) return @"merged or fz flag not 0 or 1";
        return nil;
    }
    return @"not an S, R or C row with its columns";
}

@interface DmPsoExport ()
@property NSUInteger rows; @property BOOL partialTail; @property (copy) NSString *error, *body;
@end
@implementation DmPsoExport
+ (instancetype)fromUser:(NSData *)user seed:(NSString *)seed {
    DmPsoExport *e = [DmPsoExport new];
    NSMutableSet<NSString *> *have = [NSMutableSet set];
    for (NSString *line in [seed componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [have addObject:line];
    NSString *text = user.length ? [[NSString alloc] initWithData:user encoding:NSUTF8StringEncoding] : @"";
    if (!text) { e.error = @"the user list is not UTF-8 text"; return e; }
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    if (lines.count && ![lines.lastObject isEqualToString:@""]) e.partialTail = YES;   // no newline yet: a row being written
    [lines removeLastObject];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *row = lines[i];
        if ([row hasPrefix:@"#"]) continue;
        NSString *why = row.length ? row_problem(row) : @"empty line";
        if (why) { e.error = [NSString stringWithFormat:@"line %lu: %@", (unsigned long)i + 1, why]; return e; }
        if (![have containsObject:row]) { [have addObject:row]; [out addObject:row]; }
    }
    e.rows = out.count; e.body = out.count ? [[out componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"] : @"";
    return e;
}
+ (instancetype)fromUserPath:(NSString *)user seedPath:(NSString *)seed {
    NSData *data = user ? [NSData dataWithContentsOfFile:user] : nil;   // one read: the snapshot every count and row comes from
    return [self fromUser:data seed:seed ? [NSString stringWithContentsOfFile:seed encoding:NSUTF8StringEncoding error:nil] : nil];
}
- (NSString *)writeTo:(NSURL *)url date:(NSDate *)date {
    if (self.error) return self.error;
    if (!self.rows) return @"no new rows to export";
    NSISO8601DateFormatter *f = [NSISO8601DateFormatter new];
    NSString *head = [NSString stringWithFormat:@"# The Legend of Zelda: Skyward Sword HD 1.0.1, shader list contribution exported %@: %lu rows the app's list lacks.\n"
                      "# Shader and pipeline keys only, no game data. Rows as pso_manifest.tsv; merged by games/ss/scripts/pso_seed.py.\n",
                      [f stringFromDate:date], (unsigned long)self.rows];
    NSError *err = nil;
    if (![[head stringByAppendingString:self.body] writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&err])
        return [NSString stringWithFormat:@"could not write %@: %@", url.lastPathComponent, err.localizedDescription ?: @"unknown error"];
    return nil;
}
@end

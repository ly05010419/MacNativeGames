// A warm-up row's shader variants, put on the app's own ones (2026-11-15, user: "做 换成 App 的变体 加上测试").
// Pure functions (Foundation only), compiled straight out of this file by tests/test_shader_variant_order.py.
//
// Until 61e7c86d a shader's device-cache variants came before the app's (totk_shader_rt.m variants_of), so play drew with
// variants a quick start had translated (…-90d07effe7fb, named after all its answers) instead of the app's (…-b7530f698e76,
// the same deps), and this device's lists (pso_user.tsv, pso_collected.tsv) recorded rows naming them. Draws now take the
// app's variant, so such a row only builds a pipeline nothing draws: 134 render, 24 compute and 48 mesh pipelines on this
// Mac's first start after the fix, 116 of their 171 variants in the device cache alone. The row is rewritten to the
// variant the draw takes now; a rewritten row the app's lists already hold is then left out as a repeat.
#pragma once
#import <Foundation/Foundation.h>

/* The variant a draw takes now in place of `variant` ("<shader>-<12 hex>", no "_b"): itself when the seed has it, else the
   seed's first variant of that shader with exactly the same deps, else itself. seed / cache: the shader's .variants rows
   ([{variant, deps, ...}]) of the app's seed and of the device cache. */
static NSString *totk_superseding_variant(NSString *variant, NSArray *seed, NSArray *cache) {
    NSDictionary *deps = nil;
    for (NSDictionary *r in seed) if ([r isKindOfClass:NSDictionary.class] && [r[@"variant"] isEqual:variant]) return variant;
    for (NSDictionary *r in cache) if ([r isKindOfClass:NSDictionary.class] && [r[@"variant"] isEqual:variant]) { deps = r[@"deps"]; break; }
    if (![deps isKindOfClass:NSDictionary.class]) return variant;
    for (NSDictionary *r in seed)
        if ([r isKindOfClass:NSDictionary.class] && [r[@"deps"] isEqual:deps] && [r[@"variant"] isKindOfClass:NSString.class] &&
            [(NSString *)r[@"variant"] length] == variant.length) return r[@"variant"];
    return variant;
}

/* `line` with every variant name ("<64 hex>-<12 hex>_b") put through `map` (given and returning the name without "_b"; a
   name of another length is left as it was). A field of lowercase hex digits long enough to hold a name is a row's encoded
   pipeline state (R, B, T and M rows): its bytes are rewritten in place, the same length, and encoded again. */
static NSString *totk_row_map_variants(NSString *line, NSString *(^map)(NSString *variant)) {
    static NSRegularExpression *name, *blob; static dispatch_once_t once;
    dispatch_once(&once, ^{
        name = [NSRegularExpression regularExpressionWithPattern:@"([0-9a-f]{64}-[0-9a-f]{12})_b" options:0 error:nil];
        blob = [NSRegularExpression regularExpressionWithPattern:@"^[0-9a-f]+$" options:0 error:nil];
    });
    NSString *(^rename)(NSString *) = ^NSString *(NSString *text) {
        NSMutableString *out = [text mutableCopy];
        for (NSTextCheckingResult *m in [[name matchesInString:text options:0 range:NSMakeRange(0, text.length)] reverseObjectEnumerator]) {
            NSRange r = [m rangeAtIndex:1]; NSString *old = [text substringWithRange:r], *now = map(old);
            if (now.length == old.length && ![now isEqualToString:old]) [out replaceCharactersInRange:r withString:now];
        }
        return out;
    };
    NSMutableArray<NSString *> *fields = [[line componentsSeparatedByString:@"\t"] mutableCopy];
    for (NSUInteger i = 0; i < fields.count; i++) {
        NSString *f = fields[i];
        if (f.length >= 2 * 79 && f.length % 2 == 0 && [blob numberOfMatchesInString:f options:0 range:NSMakeRange(0, f.length)]) {
            NSMutableData *bytes = [NSMutableData dataWithLength:f.length / 2]; uint8_t *b = (uint8_t *)bytes.mutableBytes;
            const char *h = f.UTF8String;
            for (NSUInteger j = 0; j < bytes.length; j++) { unsigned v = 0; sscanf(h + 2 * j, "%2x", &v); b[j] = (uint8_t)v; }
            NSString *text = [[NSString alloc] initWithData:bytes encoding:NSISOLatin1StringEncoding];
            NSString *now = text ? rename(text) : nil;
            if (now && ![now isEqualToString:text]) {
                NSData *back = [now dataUsingEncoding:NSISOLatin1StringEncoding];
                if (back.length == bytes.length) {
                    NSMutableString *hex = [NSMutableString stringWithCapacity:f.length]; const uint8_t *p = (const uint8_t *)back.bytes;
                    for (NSUInteger j = 0; j < back.length; j++) [hex appendFormat:@"%02x", p[j]];
                    fields[i] = hex;
                }
            }
        } else fields[i] = rename(f);
    }
    return [fields componentsJoinedByString:@"\t"];
}

/* A whole list's text with each row put through `row` (blank lines and "#" lines as they are, the last newline kept);
   *changed counts the rows that came back different. */
__attribute__((unused)) static NSString *totk_list_map_rows(NSString *text, NSString *(^row)(NSString *line), NSUInteger *changed) {
    if (changed) *changed = 0;
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:lines.count];
    for (NSString *line in lines) {
        if (!line.length || [line hasPrefix:@"#"]) { [out addObject:line]; continue; }
        NSString *now = row(line) ?: line;
        if (changed && ![now isEqualToString:line]) (*changed)++;
        [out addObject:now];
    }
    return [out componentsJoinedByString:@"\n"];
}

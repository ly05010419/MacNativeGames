/* Packed varyings for mesh pipelines (2026-11-08).
   On the M4 a mesh threadgroup's output allocation is the DECLARED vec4 slots x declared vertices, and that allocation
   bounds how many threadgroups a core runs at once (isolated benchmark at fed302d7's scale, claude/oot-fps90/tools/
   meshbench: declared 128 vertices x 10 float4 -> ~2.4 groups in flight a core; writing fewer vertices changes nothing,
   declaring fewer slots scales the time; float/float3 members still take a whole slot). The translator exports one
   float4 slot an attribute with the unused lanes as constants (MeshStage.cs Vec: 0.0, w 1.0): fed302d7 writes 21 real
   lanes into 9 slots. Per (mesh, fragment) pair the fragment's float4 inputs the mesh writes are regrouped by
   interpolation qualifier and their real lanes packed four to a slot; the mesh variant writes the slots, the fragment
   variant rebuilds every original input from the slots and the export's constants (an input the mesh never writes stays
   all zero, as the unwritten-varying patch leaves it). Values, interpolation and precision are unchanged; only the
   declared slot count drops. OOT_MESH_PACK_VARYINGS=0 / `flag mesh_pack 0` turns it off (A/B, probe); g_mesh_pack is
   declared with the other mesh flags in nvn_mtl.m. */
static int mtk_pack_on(void) {
    int v = atomic_load_explicit(&g_mesh_pack, memory_order_relaxed);
    if (v < 0) { const char *e = getenv("OOT_MESH_PACK_VARYINGS"); v = e ? atoi(e) != 0 : 1; atomic_store(&g_mesh_pack, v); }
    return v;
}
typedef struct { int loc; NSString *type, *name, *quals; } pack_member;   // one [[user(locnN)]] struct member
/* The [[user(locnN)]] members of `struct <name> {...};` in src; *block is the whole struct text's range. */
static BOOL pack_members(NSString *src, NSString *structName, NSMutableArray<NSValue *> *out, NSRange *block) {
    NSRegularExpression *decl = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\bstruct %@\\s*\\{", structName] options:0 error:nil];
    NSTextCheckingResult *m = [decl firstMatchInString:src options:0 range:NSMakeRange(0, src.length)];
    if (!m) return NO;
    NSRange end = [src rangeOfString:@"};" options:0 range:NSMakeRange(m.range.location, src.length - m.range.location)];
    if (end.location == NSNotFound) return NO;
    NSRange body = NSMakeRange(NSMaxRange(m.range), end.location - NSMaxRange(m.range));
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^\\s*(\\w+)\\s+(\\w+)\\s*\\[\\[user\\(locn(\\d+)\\)((?:,\\s*\\w+)*)\\]\\]\\s*;" options:NSRegularExpressionAnchorsMatchLines error:nil];
    for (NSTextCheckingResult *r in [re matchesInString:src options:0 range:body]) {
        pack_member *pm = calloc(1, sizeof *pm);
        pm->type = [src substringWithRange:[r rangeAtIndex:1]]; pm->name = [src substringWithRange:[r rangeAtIndex:2]];
        pm->loc = [[src substringWithRange:[r rangeAtIndex:3]] intValue]; pm->quals = [src substringWithRange:[r rangeAtIndex:4]];
        [out addObject:[NSValue valueWithPointer:pm]];
    }
    *block = NSMakeRange(m.range.location, NSMaxRange(end) - m.range.location);
    return YES;
}
static void pack_members_free(NSMutableArray<NSValue *> *a) { for (NSValue *v in a) free(v.pointerValue); }
/* The export lines `v.a<N>=float4(e0, e1, e2, e3);` of the generated entry (both entries carry the same lines). */
static NSDictionary<NSNumber *, NSArray<NSString *> *> *pack_exports(NSString *src, NSMutableDictionary<NSNumber *, NSString *> *lineText) {
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^v\\.a(\\d+)=float4\\(([^\\n]*)\\);$" options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    for (NSTextCheckingResult *r in [re matchesInString:src options:0 range:NSMakeRange(0, src.length)]) {
        NSNumber *loc = @([[src substringWithRange:[r rangeAtIndex:1]] intValue]);
        if (d[loc]) continue;
        NSArray *args = [[src substringWithRange:[r rangeAtIndex:2]] componentsSeparatedByString:@", "];
        if (args.count != 4) return nil;
        d[loc] = args; lineText[loc] = [src substringWithRange:r.range];
    }
    return d;
}
static BOOL pack_is_const(NSString *e) { return [e isEqualToString:@"0.0"] || [e isEqualToString:@"1.0"]; }
typedef struct { int loc, lane; NSString *expr; } pack_lane;
/* Build the packed pair for (ms, fs); nil when packing does not apply or does not reduce the slot count. */
static id<MTLFunction> mtk_pack_varyings(NvShader *ms, NvShader *fs, id<MTLFunction> base, int nodepth, id<MTLFunction> *packedFs) {
    *packedFs = nil;
    if (!fs || !sh_has_src(fs) || !sh_has_src(ms) || ms->mesh_kind != 2) return base;
    NSString *msrc = fn_src(base) ?: sh_src(ms);
    NSString *fsrc = nodepth && fs->writes_depth ? fs_src_without_depth(fs) : sh_src(fs);
    if ([fsrc componentsSeparatedByString:@"main0_in"].count != 3) return base;   // struct + entry signature only
    if ([msrc componentsSeparatedByString:@"dm_out.set_vertex(n,v);"].count < 2) return base;
    NSMutableArray<NSValue *> *fin = [NSMutableArray array], *mout = [NSMutableArray array]; NSRange fblock = {0, 0}, mblock = {0, 0};
    BOOL fok = pack_members(fsrc, @"main0_in", fin, &fblock), mok = pack_members(msrc, @"dm_vertex_t", mout, &mblock);
    NSMutableDictionary<NSNumber *, NSString *> *exportText = [NSMutableDictionary dictionary];
    NSDictionary<NSNumber *, NSArray<NSString *> *> *exports = pack_exports(msrc, exportText);
    if (!fok || !mok || !exports || !fin.count) { pack_members_free(fin); pack_members_free(mout); return base; }
    // Decide per fragment input: kept (not float4, or a per-primitive output of dm_prim_t), packed (the mesh writes it as a
    // vertex attribute), or unwritten (all zero).
    NSMutableSet<NSNumber *> *kept = [NSMutableSet set]; NSMutableArray<NSString *> *groupOrder = [NSMutableArray array];
    { NSMutableArray<NSValue *> *prim = [NSMutableArray array]; NSRange pblock = {0, 0};
      if (pack_members(msrc, @"dm_prim_t", prim, &pblock)) for (NSValue *v in prim) { pack_member *pm = v.pointerValue; [kept addObject:@(pm->loc)]; }
      pack_members_free(prim); }
    NSMutableDictionary<NSString *, NSMutableArray<NSValue *> *> *groups = [NSMutableDictionary dictionary];   // quals -> lanes
    unsigned realLanes = 0, originalSlots = 0;
    for (NSValue *v in fin) {
        pack_member *pm = v.pointerValue; NSNumber *loc = @(pm->loc);
        originalSlots++;
        if (![pm->type isEqualToString:@"float4"] || [kept containsObject:loc]) { [kept addObject:loc]; continue; }
        NSArray *e = exports[loc]; if (!e) continue;   // unwritten: rebuilt as zero
        if (!groups[pm->quals]) { groups[pm->quals] = [NSMutableArray array]; [groupOrder addObject:pm->quals]; }
        for (int lane = 0; lane < 4; lane++) if (!pack_is_const(e[lane])) {
            pack_lane *pl = calloc(1, sizeof *pl); pl->loc = pm->loc; pl->lane = lane; pl->expr = e[lane];
            [groups[pm->quals] addObject:[NSValue valueWithPointer:pl]]; realLanes++;
        }
    }
    // Slots: four lanes each within a qualifier group; locations are the lowest numbers no kept member uses.
    NSMutableArray<NSArray<NSValue *> *> *slots = [NSMutableArray array]; NSMutableArray<NSString *> *slotQuals = [NSMutableArray array];
    for (NSString *q in groupOrder) {
        NSArray *lanes = groups[q];
        for (NSUInteger i = 0; i < lanes.count; i += 4) { [slots addObject:[lanes subarrayWithRange:NSMakeRange(i, MIN(4u, lanes.count - i))]]; [slotQuals addObject:q]; }
    }
    unsigned newSlots = (unsigned)(kept.count + slots.count);
    void (^freeAll)(void) = ^{ pack_members_free(fin); pack_members_free(mout); for (NSString *q in groups) for (NSValue *v in groups[q]) free(v.pointerValue); };
    if (!slots.count || newSlots >= originalSlots) { freeAll(); return base; }
    NSMutableArray<NSNumber *> *slotLoc = [NSMutableArray array];
    for (int l = 0, s = 0; (unsigned)s < slots.count && l < 64; l++) if (![kept containsObject:@(l)]) { [slotLoc addObject:@(l)]; s++; }
    if (slotLoc.count != slots.count) { freeAll(); return base; }
    // Layout string: the cache key of both variants.
    NSMutableString *layout = [NSMutableString string];
    for (NSUInteger s = 0; s < slots.count; s++) {
        [layout appendFormat:@"%@%@[", slotLoc[s], [slotQuals[s] stringByReplacingOccurrencesOfString:@" " withString:@""]];
        for (NSValue *v in slots[s]) { pack_lane *pl = v.pointerValue; [layout appendFormat:@"%d.%d,", pl->loc, pl->lane]; }
        [layout appendString:@"]"];
    }
    for (NSValue *v in fin) { pack_member *pm = v.pointerValue; if (!exports[@(pm->loc)] && ![kept containsObject:@(pm->loc)]) [layout appendFormat:@"z%d", pm->loc]; }
    NSString *mrecipe = [NSString stringWithFormat:@"pack;%p;%@", (__bridge void *)base, layout];
    NSString *frecipe = [NSString stringWithFormat:@"meshpack;%d;%@", nodepth && fs->writes_depth, layout];
    os_unfair_lock_lock(&g_vpatch_lock);
    id<MTLFunction> mfn = ms->vpatch[mrecipe], ffn = fs->vpatch[frecipe];
    os_unfair_lock_unlock(&g_vpatch_lock);
    if (mfn && ffn) { freeAll(); *packedFs = ffn; return mfn; }
    // ---- mesh variant: struct and exports
    NSMutableString *mstruct = [NSMutableString stringWithString:@"struct dm_vertex_t {"];
    { NSString *block = [msrc substringWithRange:mblock];
      NSRegularExpression *pos = [NSRegularExpression regularExpressionWithPattern:@"float4\\s+position\\s*\\[\\[[^\\]]*\\]\\]\\s*;" options:0 error:nil];
      NSTextCheckingResult *p = [pos firstMatchInString:block options:0 range:NSMakeRange(0, block.length)];
      if (!p) { freeAll(); return base; }
      [mstruct appendFormat:@" %@\n", [block substringWithRange:p.range]]; }
    NSMutableString *msource = [NSMutableString stringWithString:msrc];
    for (NSValue *v in mout) { pack_member *pm = v.pointerValue;
        if ([kept containsObject:@(pm->loc)]) [mstruct appendFormat:@"%@ %@ [[user(locn%d)]];\n", pm->type, pm->name, pm->loc];
        else if (exportText[@(pm->loc)]) [msource replaceOccurrencesOfString:[exportText[@(pm->loc)] stringByAppendingString:@"\n"] withString:@"" options:0 range:NSMakeRange(0, msource.length)]; }
    NSMutableString *slotWrites = [NSMutableString string];
    for (NSUInteger s = 0; s < slots.count; s++) {
        [mstruct appendFormat:@"float4 dm_p%lu [[user(locn%@)]];\n", (unsigned long)s, slotLoc[s]];
        NSMutableArray *e = [NSMutableArray array]; for (NSValue *v in slots[s]) { pack_lane *pl = v.pointerValue; [e addObject:pl->expr]; }
        while (e.count < 4) [e addObject:@"0.0"];
        [slotWrites appendFormat:@"v.dm_p%lu=float4(%@);\n", (unsigned long)s, [e componentsJoinedByString:@", "]];
    }
    [mstruct appendString:@"};"];
    [msource replaceCharactersInRange:[msource rangeOfString:[msrc substringWithRange:mblock]] withString:mstruct];
    [msource replaceOccurrencesOfString:@"dm_out.set_vertex(n,v);" withString:[slotWrites stringByAppendingString:@"dm_out.set_vertex(n,v);"] options:0 range:NSMakeRange(0, msource.length)];
    // ---- fragment variant: struct, locals rebuilding the original inputs, renamed uses
    NSRegularExpression *sig = [NSRegularExpression regularExpressionWithPattern:@"fragment\\s+\\w+\\s+main0\\(main0_in\\s+(\\w+)\\s*\\[\\[stage_in\\]\\]" options:0 error:nil];
    NSTextCheckingResult *sm = [sig firstMatchInString:fsrc options:0 range:NSMakeRange(0, fsrc.length)];
    if (!sm) { freeAll(); return base; }
    NSString *inName = [fsrc substringWithRange:[sm rangeAtIndex:1]];
    NSRange open = [fsrc rangeOfString:@"\n{\n" options:0 range:NSMakeRange(NSMaxRange(sm.range), fsrc.length - NSMaxRange(sm.range))];
    if (open.location == NSNotFound) { freeAll(); return base; }
    NSMutableString *fstruct = [NSMutableString stringWithString:@"struct main0_in\n{\n"], *locals = [NSMutableString string];
    NSMutableDictionary<NSNumber *, NSMutableArray<NSString *> *> *rebuilt = [NSMutableDictionary dictionary];   // loc -> 4 lane sources
    for (NSValue *v in fin) { pack_member *pm = v.pointerValue; NSArray *e = exports[@(pm->loc)];
        if ([kept containsObject:@(pm->loc)]) continue;
        NSMutableArray *lanes = [NSMutableArray arrayWithCapacity:4];
        for (int lane = 0; lane < 4; lane++) [lanes addObject:e ? (pack_is_const(e[lane]) ? e[lane] : @"?") : @"0.0"];
        rebuilt[@(pm->loc)] = lanes; }
    static const char *swz = "xyzw";
    for (NSUInteger s = 0; s < slots.count; s++) { NSUInteger i = 0;
        for (NSValue *v in slots[s]) { pack_lane *pl = v.pointerValue; rebuilt[@(pl->loc)][pl->lane] = [NSString stringWithFormat:@"%@.dm_p%lu.%c", inName, (unsigned long)s, swz[i++]]; } }
    NSMutableString *fbody = [NSMutableString stringWithString:fsrc];
    for (NSValue *v in fin) { pack_member *pm = v.pointerValue;
        if ([kept containsObject:@(pm->loc)]) { [fstruct appendFormat:@"    %@ %@ [[user(locn%d)%@]];\n", pm->type, pm->name, pm->loc, pm->quals]; continue; }
        NSArray *lanes = rebuilt[@(pm->loc)]; if ([lanes containsObject:@"?"]) { freeAll(); return base; }
        [locals appendFormat:@"    float4 dm_%@ = float4(%@);\n", pm->name, [lanes componentsJoinedByString:@", "]];
        NSRegularExpression *use = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\b%@\\.%@\\b", inName, pm->name] options:0 error:nil];
        [use replaceMatchesInString:fbody options:0 range:NSMakeRange(0, fbody.length) withTemplate:[@"dm_" stringByAppendingString:pm->name]]; }
    for (NSUInteger s = 0; s < slots.count; s++) [fstruct appendFormat:@"    float4 dm_p%lu [[user(locn%@)%@]];\n", (unsigned long)s, slotLoc[s], slotQuals[s]];
    [fstruct appendString:@"};"];
    // The struct block holds declarations only, so its text survived the renames; replace it, then open the entry body.
    NSRange fblock2 = [fbody rangeOfString:[fsrc substringWithRange:fblock]];
    if (fblock2.location == NSNotFound) { freeAll(); return base; }
    [fbody replaceCharactersInRange:fblock2 withString:fstruct];
    NSTextCheckingResult *sm2 = [sig firstMatchInString:fbody options:0 range:NSMakeRange(0, fbody.length)];
    NSRange openB = sm2 ? [fbody rangeOfString:@"\n{\n" options:0 range:NSMakeRange(NSMaxRange(sm2.range), fbody.length - NSMaxRange(sm2.range))] : NSMakeRange(NSNotFound, 0);
    if (openB.location == NSNotFound) { freeAll(); return base; }
    [fbody replaceCharactersInRange:openB withString:[@"\n{\n" stringByAppendingString:locals]];
    freeAll();
    NSError *error = nil;
    id<MTLLibrary> mlib = variant_library(msource, &error); id<MTLFunction> m = mlib ? [mlib newFunctionWithName:base.name] : nil;
    if (!m) { static int said; if (said++ < 5) MLOG("【红】[mesh-pack] mesh %s variant failed (kept unpacked): %s", ms->key, error.localizedDescription.UTF8String); return base; }
    id<MTLLibrary> flib = variant_library(fbody, &error); id<MTLFunction> f = flib ? [flib newFunctionWithName:fs->fn.name] : nil;
    if (!f) { static int said; if (said++ < 5) MLOG("【红】[mesh-pack] fragment %s variant failed (kept unpacked): %s", fs->key, error.localizedDescription.UTF8String); return base; }
    fn_note_src(m, msource); fn_note_src(f, fbody);
    os_unfair_lock_lock(&g_vpatch_lock);
    if (!ms->vpatch) ms->vpatch = [NSMutableDictionary dictionary]; if (!ms->vpatch[mrecipe]) ms->vpatch[mrecipe] = m; m = ms->vpatch[mrecipe];
    if (!fs->vpatch) fs->vpatch = [NSMutableDictionary dictionary]; if (!fs->vpatch[frecipe]) fs->vpatch[frecipe] = f; f = fs->vpatch[frecipe];
    os_unfair_lock_unlock(&g_vpatch_lock);
    MLOG("[mesh-pack] mesh %.16s fragment %.16s: %u float4 slots -> %u (%u lanes)", ms->key, fs->key, originalSlots, newSlots, realLanes);
    *packedFs = f; return m;
}

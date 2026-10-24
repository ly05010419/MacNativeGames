/* Breath of the Wild 1.9.0: sharp UI art from a high-res texture pack (BOTW_TP_* in nvn_obj.h; included at the end of
   nvn_mtl.m for GAME_BOTW190 only, so TOTK and 1.6.0 objects stay as they were).

   The UI's small textures (48x48 and 72x72 masks, colour icons) are magnified about 2x at 3456x1944 with linear
   filtering, which blurs their one-texel anti-aliased edges. The pack holds larger copies (AI-upscaled game art, or
   prompts drawn from vectors: games/botw/scripts/botw_texpack.py, docs/mods.md). The UI shaders sample with
   normalised coordinates, so a larger texture draws at the same place and size.

   - Key: pkg_hash of the texture's guest bytes (block-linear, as in the pool), width, height and NVN format:
     "<hash 16 hex>-<w>x<h>-<format 2 hex>", file "<key>.png" in the pack folder.
   - Candidates: mirrored (CPU-pool) 2D textures with one level, at most 1024x1024, in R8 / BC4 (masks), RG8 / BC5, or
     RGBA8 / BC1 / BC2 / BC3 / BC7 (linear or sRGB), whose Metal copy has the format's own Metal format.
   - The guest's copy (NvPkg.tex) stays as it is for copies, readback and rendering. The pack's texture (tp_rep) is
     only the sampling base of texture-pool slots (tex_by_id) whose registration views the whole texture in its own
     format; it has the uncompressed format of the guest's Metal format (R8, RG8, RGBA8 or RGBA8 sRGB) and the guest
     copy's swizzle, so it samples like the guest texture. A PNG holds the stored channel values: grey for R8, RGB with
     blue 0 for RG8, RGBA (not premultiplied) for colour; sRGB colour is stored as encoded.
   - A background queue decodes the PNG; the replay thread creates the texture when a slot next validates (after a
     ser_bump of the serial), if the upload it was decoded for is still the current one. Until then the guest texture
     draws. A re-upload, a GPU rendering into the texture or the NvPkg's end retires it.
   - BOTW_TEXPACK_DIR: the pack folder (default ~/Library/Application Support/MacNativeGames/BotW/texture-pack);
     BOTW_TEXPACK=0: off.
   - BOTW_TEXPACK_HARVEST=<folder>: the first time a UI picture shader (BOTW_TEXPACK_HARVEST_FS, key prefixes, default
     87d90016,e6a88e12) samples a candidate, its texels are written there as "<key>.png" (decoded on the CPU from the
     guest bytes) with a line in harvest.tsv. Textures already in the folder are not written again, so it grows over runs. */
#import <CoreGraphics/CoreGraphics.h>

int g_botw_tp = -1;   /* -1 not yet set up, 0 off, 1 on */
static NSSet<NSString *> *g_tp_index;   /* pack keys */
static NSString *g_tp_dir, *g_tp_harv;
static NSMutableSet<NSString *> *g_tp_harv_seen;   /* keys in the harvest folder */
static const char *g_tp_harv_fs;
static dispatch_queue_t g_tp_q;
static os_unfair_lock g_tp_lk = OS_UNFAIR_LOCK_INIT;   /* tp_px / tp_pw / tp_ph / tp_pgen of every NvPkg */
static uint64_t g_tp_matched, g_tp_published, g_tp_bad, g_tp_harvested;

static NSSet<NSString *> *tp_scan(NSString *dir) {
    NSMutableSet *s = [NSMutableSet set];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if ([f.pathExtension isEqualToString:@"png"]) [s addObject:f.stringByDeletingPathExtension];
    return s;
}
static void tp_setup(void) {
    const char *on = getenv("BOTW_TEXPACK"), *dir = getenv("BOTW_TEXPACK_DIR"), *harv = getenv("BOTW_TEXPACK_HARVEST");
    g_botw_tp = 0;
    if (harv && *harv) {
        g_tp_harv = @(harv); [[NSFileManager defaultManager] createDirectoryAtPath:g_tp_harv withIntermediateDirectories:YES attributes:nil error:nil];
        g_tp_harv_seen = [tp_scan(g_tp_harv) mutableCopy];
        g_tp_harv_fs = getenv("BOTW_TEXPACK_HARVEST_FS"); if (!g_tp_harv_fs || !*g_tp_harv_fs) g_tp_harv_fs = "87d90016,e6a88e12";
        MLOG("[texpack] harvest into %s (%lu textures there; fragment shaders %s)", harv, (unsigned long)g_tp_harv_seen.count, g_tp_harv_fs);
        g_botw_tp = 1;
    }
    if (!on || atoi(on)) {
        g_tp_dir = dir && *dir ? @(dir) : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/BotW/texture-pack"];
        g_tp_index = tp_scan(g_tp_dir);
        if (g_tp_index.count) { MLOG("[texpack] %lu textures in %s (BOTW_TEXPACK=0 off)", (unsigned long)g_tp_index.count, g_tp_dir.UTF8String); g_botw_tp = 1; }
        else g_tp_index = nil;
    }
    if (g_botw_tp) g_tp_q = dispatch_queue_create("botw.texpack", DISPATCH_QUEUE_SERIAL);
}

/* 1 R8 (BC4), 2 RG8 (BC5), 3 RGBA8, 4 RGBA8 sRGB; 0 not a candidate */
static int tp_class(const tex_t *t, id<MTLTexture> tex) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!tex || !t->mem || !t->L.size || (t->d.target != 1 && t->d.target != 7) || t->d.levels > 1 || t->d.width > 1024 || t->d.height > 1024) return 0;
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || pf != (uint32_t)tex.pixelFormat || tex.mipmapLevelCount != 1) return 0;
    switch ((MTLPixelFormat)pf) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatBC4_RUnorm: return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatBC5_RGUnorm: return 2;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC7_RGBAUnorm: return 3;
    case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA_sRGB: case MTLPixelFormatBC7_RGBAUnorm_sRGB: return 4;
    default: return 0;
    }
}
static int tp_nc(int cls) { return cls == 1 ? 1 : cls == 2 ? 2 : 4; }
static NSString *tp_key(NvPkg *pk, const tex_t *t) { return [NSString stringWithFormat:@"%016llx-%ux%u-%02x", (unsigned long long)pk->tp_hash, t->d.width, t->d.height, t->d.format]; }

/* Background queue: the PNG's pixels as tight rows of the class's channels, scaled by a whole factor of the guest size */
static uint8_t *tp_load(NSString *path, int cls, uint32_t w, uint32_t h, uint32_t *pw, uint32_t *ph) {
    CGImageSourceRef s = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    CGImageRef im = s ? CGImageSourceCreateImageAtIndex(s, 0, NULL) : NULL;
    if (s) CFRelease(s);
    if (!im) return NULL;
    size_t iw = CGImageGetWidth(im), ih = CGImageGetHeight(im), bpp = CGImageGetBitsPerPixel(im), row = CGImageGetBytesPerRow(im);
    CGImageAlphaInfo ai = CGImageGetAlphaInfo(im);
    uint8_t *out = NULL;
    /* 8-bit grey, grey + alpha, RGB(X) or RGBA (ImageIO keeps a PNG's values: no colour conversion, alpha not premultiplied) */
    int ok = CGImageGetBitsPerComponent(im) == 8 && (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) && iw % w == 0 && ih % h == 0 && iw / w == ih / h && iw / w <= 8
          && (CGImageGetBitmapInfo(im) & kCGBitmapByteOrderMask) == kCGBitmapByteOrderDefault
          && ai != kCGImageAlphaPremultipliedLast && ai != kCGImageAlphaPremultipliedFirst && ai != kCGImageAlphaFirst && ai != kCGImageAlphaNoneSkipFirst
          && CGColorSpaceGetModel(CGImageGetColorSpace(im)) != kCGColorSpaceModelIndexed;      /* a palette PNG holds indices, not values */
    CFDataRef d = ok ? CGDataProviderCopyData(CGImageGetDataProvider(im)) : NULL;
    if (d && (size_t)CFDataGetLength(d) >= row * ih && (out = malloc(iw * ih * tp_nc(cls)))) {
        const uint8_t *b = CFDataGetBytePtr(d); int nc = tp_nc(cls), sc = (int)(bpp / 8), grey = sc <= 2;
        for (size_t y = 0; y < ih; y++) for (size_t x = 0; x < iw; x++) {
            const uint8_t *p = b + y * row + x * sc; uint8_t *o = out + (y * iw + x) * nc;
            uint8_t px[4] = {p[0], grey ? p[0] : p[1], grey ? p[0] : p[2], sc == 2 ? p[1] : sc == 4 && ai != kCGImageAlphaNoneSkipLast ? p[3] : 255};
            memcpy(o, px, nc);
        }
        *pw = (uint32_t)iw; *ph = (uint32_t)ih;
    }
    if (d) CFRelease(d);
    CGImageRelease(im);
    return out;
}

/* nvn_mtl.m tex_mtl, after a first upload or a re-upload of a mirrored texture (replay thread) */
void botw_tp_uploaded(void *pk_, const tex_t *t) {
    if (g_botw_tp < 0) tp_setup();
    if (!g_botw_tp) return;
    NvPkg *pk = (__bridge NvPkg *)pk_;
    os_unfair_lock_lock(&g_tp_lk); uint64_t gen = ++pk->tp_gen; free(pk->tp_px); pk->tp_px = NULL; os_unfair_lock_unlock(&g_tp_lk);
    if (pk->tp_rep) { defer_obj_release(pk->tp_rep); pk->tp_rep = nil; }   // the serial was bumped by the re-upload: slot views look again
    pk->tp_ser = t->serial; pk->tp_hash = 0; pk->tp_hv = 0;
    if (!(pk->tp_cls = tp_class(t, pk->tex))) return;
    pk->tp_hash = pkg_hash(t->mem, t->L.size);
    if (!g_tp_index) return;
    NSString *key = tp_key(pk, t);
    if (![g_tp_index containsObject:key]) return;
    g_tp_matched++;
    NSString *path = [[g_tp_dir stringByAppendingPathComponent:key] stringByAppendingPathExtension:@"png"];
    int cls = pk->tp_cls; uint32_t w = t->d.width, h = t->d.height; uint64_t ser = t->serial;
    dispatch_async(g_tp_q, ^{
        uint32_t pw = 0, ph = 0; uint8_t *px = tp_load(path, cls, w, h, &pw, &ph);
        if (!px) { g_tp_bad++; MLOG("【红】[texpack] %s: not an 8-bit PNG of a whole multiple of %ux%u, not used", path.lastPathComponent.UTF8String, w, h); return; }
        os_unfair_lock_lock(&g_tp_lk);
        if (pk->tp_gen == gen) { free(pk->tp_px); pk->tp_px = px; pk->tp_pw = pw; pk->tp_ph = ph; pk->tp_pgen = gen; px = NULL; }
        os_unfair_lock_unlock(&g_tp_lk);
        if (px) free(px); else ser_bump(ser);   // the slots of this serial validate again and publish it (botw_tp_pick)
    });
}

/* the decoded pixels of the current upload, if any, become the pack texture (replay thread) */
static void tp_publish(NvPkg *pk) {
    os_unfair_lock_lock(&g_tp_lk);
    uint8_t *px = pk->tp_pgen == pk->tp_gen ? pk->tp_px : NULL; uint32_t pw = pk->tp_pw, ph = pk->tp_ph;
    if (px) pk->tp_px = NULL;
    os_unfair_lock_unlock(&g_tp_lk);
    if (!px) return;
    int nc = tp_nc(pk->tp_cls);
    MTLPixelFormat pf = pk->tp_cls == 1 ? MTLPixelFormatR8Unorm : pk->tp_cls == 2 ? MTLPixelFormatRG8Unorm : pk->tp_cls == 3 ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA8Unorm_sRGB;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:pw height:ph mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; d.swizzle = pk->tex.swizzle;
    id<MTLTexture> rep = [g_dev newTextureWithDescriptor:d];
    if (rep) {
        [rep replaceRegion:MTLRegionMake2D(0, 0, pw, ph) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)pw * nc];
        res_add(rep); pk->tp_rep = rep; g_tp_published++;
        if (g_tp_published <= 3 || g_tp_published % 100 == 0) MLOG("[texpack] serial %llu: %lux%lu pack texture in use (%llu so far, %llu matched, %llu unusable)",
            (unsigned long long)pk->tp_ser, (unsigned long)pw, (unsigned long)ph, (unsigned long long)g_tp_published, (unsigned long long)g_tp_matched, (unsigned long long)g_tp_bad);
    }
    free(px);
}
/* the registration views the whole texture in its own format, as one 2D level and layer */
static int tp_slot_ok(const treg_t *rp) {
    if (!rp->has_v) return 1;
    const view_t *v = &rp->v;
    return !(v->set_format && v->format != rp->t.d.format) && !(v->set_levels && (v->base_level || v->nlevels > 1)) && !(v->set_layers && (v->min_layer || v->nlayers > 1))
        && !(v->set_target && ttype(v->target, MTLTextureType2D) != MTLTextureType2D);
}
/* nvn_mtl.m tex_by_id (g_rmu held): the sampling base of a texture-pool slot, the pack texture or base (tex_mtl's) */
void *botw_tp_pick(void *rp_, void *base_) {
    if (g_botw_tp <= 0 || !base_) return base_;
    treg_t *rp = rp_; __unsafe_unretained id o = serial_get(rp->t.serial);
    if (![o isKindOfClass:[NvPkg class]]) return base_;
    NvPkg *pk = o;
    if ((__bridge void *)pk->tex != base_ || !pk->tp_cls) return base_;   // a rendering read in its place (BOTW_PKG_RENDER_ALIAS), or no candidate
    if (pk->owned) { if (pk->tp_rep) { defer_obj_release(pk->tp_rep); pk->tp_rep = nil; } return base_; }   // the GPU renders into it: its own copy
    if (!pk->tp_rep) tp_publish(pk);
    return pk->tp_rep && tp_slot_ok(rp) ? (__bridge void *)pk->tp_rep : base_;
}

/* Harvest: the texels as stored (BC decoded on the CPU), tight rows of the class's channels */
static uint8_t *tp_texels(const tex_t *t, int cls) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), nc = tp_nc(cls);
    uint32_t w = t->d.width, h = t->d.height; size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
    uint8_t *raw = malloc(pitch * rows), *out = malloc((size_t)w * h * nc);
    int bad = !raw || !out || !bpb;
    if (!bad && nvn_tex_is_linear(&t->d)) { size_t stride = nvn_tex_linear_stride(&t->d); for (size_t r = 0; r < rows; r++) memcpy(raw + r * pitch, t->mem + r * stride, pitch); }
    else if (!bad) bad = nvn_deswizzle(&t->d, 0, 0, t->mem, raw, pitch) != 0;
    if (!bad && fmt_compressed(t->d.format)) bad = nvn_bc_decode(t->d.format, raw, pitch, w, h, out, (size_t)w * nc) != 0;
    else if (!bad) { if (bw != 1 || bh != 1 || bpb != nc) bad = 1; else memcpy(out, raw, pitch * rows); }
    free(raw);
    if (bad) { free(out); return NULL; }
    return out;
}
static int tp_write_png(NSString *path, const uint8_t *px, uint32_t w, uint32_t h, int cls) {
    int nc = cls == 2 ? 3 : tp_nc(cls); size_t n = (size_t)w * h;   // RG8 as RGB with blue 0
    NSMutableData *buf = [NSMutableData dataWithLength:n * nc]; uint8_t *b = buf.mutableBytes;
    for (size_t i = 0; i < n; i++) { if (cls == 2) { b[i * 3] = px[i * 2]; b[i * 3 + 1] = px[i * 2 + 1]; } else memcpy(b + i * nc, px + i * nc, nc); }
    CGColorSpaceRef cs = nc == 1 ? CGColorSpaceCreateDeviceGray() : CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)buf);
    CGImageRef im = CGImageCreate(w, h, 8, 8 * nc, (size_t)w * nc, cs, nc == 4 ? (CGBitmapInfo)kCGImageAlphaLast : (CGBitmapInfo)kCGImageAlphaNone, dp, NULL, false, kCGRenderingIntentDefault);
    CGImageDestinationRef ds = im ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    int ok = 0; if (ds) { CGImageDestinationAddImage(ds, im, NULL); ok = CGImageDestinationFinalize(ds); CFRelease(ds); }
    if (im) CGImageRelease(im);
    CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    return ok;
}
static int tp_fs_ok(const char *key) {
    for (const char *q = g_tp_harv_fs; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) {
        size_t n = strchr(q, ',') ? (size_t)(strchr(q, ',') - q) : strlen(q);
        if (n && !strncmp(key, q, n)) return 1;
    }
    return 0;
}
/* nvn_mtl.m encode_args, a texture bound for a draw (replay thread, g_rmu held) */
void botw_tp_harvest(const char *fs_key, const tex_t *t) {
    if (!g_tp_harv || !fs_key || !tp_fs_ok(fs_key)) return;
    __unsafe_unretained id o = serial_get(t->serial);
    if (![o isKindOfClass:[NvPkg class]]) return;
    NvPkg *pk = o;
    if (!pk->tp_cls || pk->owned || pk->tp_hv == pk->tp_gen) return;
    pk->tp_hv = pk->tp_gen;
    NSString *key = tp_key(pk, t);
    if ([g_tp_harv_seen containsObject:key]) return;
    [g_tp_harv_seen addObject:key];
    int cls = pk->tp_cls; uint8_t *px = tp_texels(t, cls);
    if (!px) { MLOG("【红】[texpack] harvest %s: texels not readable", key.UTF8String); return; }
    uint32_t w = t->d.width, h = t->d.height;
    NSString *line = [NSString stringWithFormat:@"%@\t%u\t%u\t%#x\t%lu\t%s\t%d\t%.16s\t%llu\t%llu\n", key, w, h, t->d.format, (unsigned long)pk->tex.pixelFormat,
                      cls <= 2 ? "mask" : "colour", cls == 4, fs_key, (unsigned long long)t->L.size, (unsigned long long)atomic_load(&g_cur_frame)];
    g_tp_harvested++;
    dispatch_async(g_tp_q, ^{
        NSString *path = [[g_tp_harv stringByAppendingPathComponent:key] stringByAppendingPathExtension:@"png"];
        int ok = tp_write_png(path, px, w, h, cls);
        free(px);
        if (!ok) { MLOG("【红】[texpack] harvest: cannot write %s", path.UTF8String); return; }
        NSString *tsv = [g_tp_harv stringByAppendingPathComponent:@"harvest.tsv"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:tsv])
            [@"key\twidth\theight\tnvn_format\tmetal_format\tclass\tsrgb\tfs_key\tbytes\tframe\n" writeToFile:tsv atomically:NO encoding:NSUTF8StringEncoding error:nil];
        NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:tsv];      /* the error-returning calls: the plain ones raise on a full disk */
        if (!fh || ![fh seekToEndReturningOffset:NULL error:NULL] || ![fh writeData:[line dataUsingEncoding:NSUTF8StringEncoding] error:NULL])
            MLOG("【红】[texpack] harvest: cannot add %s to %s", key.UTF8String, tsv.UTF8String);
        [fh closeAndReturnError:NULL];
    });
    if (g_tp_harvested <= 3 || g_tp_harvested % 50 == 0) MLOG("[texpack] harvested %s (%llu this run)", key.UTF8String, (unsigned long long)g_tp_harvested);
}

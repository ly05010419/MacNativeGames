// Shader reflection json (B: produced by translating the original Switch shaders, output of tools/dmsh_build.py) → fixed-length C arrays (nvn_refl.h)
#import "nvn_refl.h"
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

// Dummy texture type: same prefix rules as the original dummy_tex (an unbound texture slot reads 0 per D3D semantics)
static void dummy_kind(NSString *t, uint8_t *tt, uint8_t *pf) {
    MTLTextureType y = MTLTextureType2D;
    if ([t hasPrefix:@"texture2d_array"] || [t hasPrefix:@"depth2d_array"]) y = MTLTextureType2DArray;
    else if ([t hasPrefix:@"texturecube_array"] || [t hasPrefix:@"depthcube_array"]) y = MTLTextureTypeCubeArray;
    else if ([t hasPrefix:@"texturecube"] || [t hasPrefix:@"depthcube"]) y = MTLTextureTypeCube;
    else if ([t hasPrefix:@"texture3d"]) y = MTLTextureType3D;
    else if ([t hasPrefix:@"texture1d_array"]) y = MTLTextureType1DArray;
    else if ([t hasPrefix:@"texture1d"]) y = MTLTextureType1D;
    else if ([t hasPrefix:@"texture_buffer"]) y = MTLTextureTypeTextureBuffer;
    *tt = (uint8_t)y;
    *pf = [t hasPrefix:@"depth"] ? NVN_DPF_DEPTH : [t containsString:@"<uint"] ? NVN_DPF_UINT : [t containsString:@"<int"] ? NVN_DPF_SINT : NVN_DPF_FLOAT;
}

// array<element, n> → n and the element type (0 and the type itself for anything else)
static int arr_len(NSString *ty, NSString **el) {
    *el = ty;
    if (![ty isKindOfClass:[NSString class]] || ![ty hasPrefix:@"array<"] || ![ty hasSuffix:@">"]) return 0;
    NSRange c = [ty rangeOfString:@", " options:NSBackwardsSearch];
    if (c.location == NSNotFound) return 0;
    *el = [ty substringWithRange:NSMakeRange(6, c.location - 6)];
    return [[ty substringWithRange:NSMakeRange(NSMaxRange(c), ty.length - 1 - NSMaxRange(c))] intValue];
}

// Whether a storage buffer binding may be written: current B reflection carries the translator's write flag ("rw"); old samples without it fall back to SPIRV-Cross's const device qualifier.
static int sbuf_rw(NSDictionary *b) { id rw = b[@"rw"]; if (rw) return [rw boolValue]; id ty = b[@"msl_type"]; return !([ty isKindOfClass:[NSString class]] && [ty hasPrefix:@"const "]); }
int nvn_refl_from_json(NSDictionary *j, nvn_refl *o, char *err, int errlen) {
    memset(o, 0, sizeof *o);
    #define FAIL(...) do { free(o->c1); o->c1 = NULL; snprintf(err, errlen, __VA_ARGS__); return 1; } while (0)
    if (![j[@"route"] isEqual:@"B"]) FAIL("not a B reflection (route ≠ B; route A / PC-chain reflection is retired)");
    {   // Translated original Switch shaders: kind and NVN slot number are already parsed from the MSL member names by dmsh_build.py
        NSArray *bs = j[@"bindings"];
        if (![bs isKindOfClass:[NSArray class]]) FAIL("missing bindings");
        if (bs.count > NVN_REFL_MAXB) FAIL("%lu bindings, over the limit %d", (unsigned long)bs.count, NVN_REFL_MAXB);
        o->route_b = 1; o->abi = 0;   // spirv-cross set 0 → [[buffer(0)]]
        for (NSDictionary *b in bs) {
            NSString *k = b[@"kind"]; nvn_rbind *r = &o->b[o->nb];
            id rg = b[@"reg"]; int reg = [rg isKindOfClass:[NSNumber class]] ? [rg intValue] : -1, aid = [b[@"arg_id"] intValue];
            r->aid = (uint16_t)aid; r->reg = reg < 0 ? 0 : (uint16_t)reg;
            NSString *nm = b[@"msl_name"], *ty = b[@"msl_type"];
            // Ryujinx's fallback for a handle it cannot trace to one slot (handle type 4: <stage>_<t|tb|s>_cb0_40000000, declared array<element, n>); the build leaves it without a slot
            // ('tbuf' / 'tex' with no reg, '?' for the samplers). Octopath Traveler 0's vertex shaders pick one of several c2 table slots at run time (61 shaders, slots 0-10)
            NSString *el; int n = arr_len(ty, &el);
            if ([nm isKindOfClass:[NSString class]] && [nm hasSuffix:@"_cb0_40000000"] && [ty isKindOfClass:[NSString class]] && [ty hasPrefix:@"array<"] && [ty hasSuffix:@">"]) {
                if (n < 1 || n > 32) FAIL("B binding %s: array of %d", nm.UTF8String, n);
                if ([el hasPrefix:@"sampler"]) r->kind = NVN_RB_SAMPD;
                else if ([el hasPrefix:@"texture_buffer"]) { r->kind = NVN_RB_TEXD; r->dtt = (uint8_t)MTLTextureTypeTextureBuffer; uint8_t tt; dummy_kind(el, &tt, &r->dpf); }
                else if ([el hasPrefix:@"texture"] || [el hasPrefix:@"depth"]) { r->kind = NVN_RB_TEXD; dummy_kind(el, &r->dtt, &r->dpf); }
                else FAIL("B binding %s: dynamic handle array of %s", nm.UTF8String, el.UTF8String);
                r->n = (uint8_t)n; o->dyn = 1; o->nb++; continue;
            }
            if ([k isEqualToString:@"cbuf"]) r->kind = NVN_RB_CBUF;
            else if ([k isEqualToString:@"samp"]) r->kind = NVN_RB_SAMP;
            else if ([k isEqualToString:@"samp_tex"]) r->kind = NVN_RB_SAMPT;
            else if ([k isEqualToString:@"tex"]) { r->kind = NVN_RB_TEX; dummy_kind(b[@"msl_type"], &r->dtt, &r->dpf); r->f1d = [b[@"fetch1d"] boolValue]; }
            // An array of them (array<element, n>): the handles of a uniform buffer the shader indexes at run time (Ryujinx's texture array from a buffer, handle 0: the index is the handle's
            // byte offset ÷ 8, an NVN handle being 64-bit). Paper Mario: The Thousand-Year Door's reflection shaders: array<texture2d<float>, 32> fp_t2d_cb14_0, the handles of uniform slot 11
            else if ([k isEqualToString:@"tex_ubo"]) { r->kind = NVN_RB_TEXU; dummy_kind(el, &r->dtt, &r->dpf); r->off = (uint16_t)[b[@"off"] intValue]; o->bindless = 1;
                if (n) { if (n > 32) FAIL("B binding %s: array of %d", nm.UTF8String, n); r->n = (uint8_t)n; } }
            else if ([k isEqualToString:@"samp_ubo"]) { r->kind = NVN_RB_SAMPU; r->off = (uint16_t)[b[@"off"] intValue]; o->bindless = 1;
                if (n) { if (n > 32) FAIL("B binding %s: array of %d", nm.UTF8String, n); r->n = (uint8_t)n; } }
            else if ([k isEqualToString:@"samp_id"]) r->kind = NVN_RB_SAMPI;
            else if ([k isEqualToString:@"tex_c2a"] || [k isEqualToString:@"samp_c2a"]) {   // an array of the driver table's handles (nvn_refl.h TEXA / SAMPA)
                if (n < 1 || n > 32) FAIL("B binding %s: array of %d", nm.UTF8String, n);
                r->kind = [k isEqualToString:@"tex_c2a"] ? NVN_RB_TEXA : NVN_RB_SAMPA; r->n = (uint8_t)n; r->off = (uint16_t)[b[@"off"] intValue];
                if (r->kind == NVN_RB_TEXA) dummy_kind(el, &r->dtt, &r->dpf);
            }
            else if ([k isEqualToString:@"img"]) { r->kind = NVN_RB_IMG; dummy_kind(b[@"msl_type"], &r->dtt, &r->dpf);   // rw: the shader writes it (access::write / read_write; a load-only view of the slot is not)
                r->rw = [ty isKindOfClass:[NSString class]] && ([ty containsString:@"access::write"] || [ty containsString:@"access::read_write"]); r->atom = [b[@"atomic"] boolValue]; }
            else if ([k isEqualToString:@"tbuf"]) { r->kind = NVN_RB_TBUF; uint8_t tt; dummy_kind(b[@"msl_type"], &tt, &r->dpf); }
            else if ([k isEqualToString:@"sbuf"]) { r->kind = NVN_RB_SBUFP; r->rw = sbuf_rw(b); }
            else if ([k isEqualToString:@"ubo_ptr"]) r->kind = NVN_RB_UBOP;
            else if ([k isEqualToString:@"sbuf_ubo"]) { r->kind = NVN_RB_SBUFU; r->rw = sbuf_rw(b); r->off = (uint16_t)[b[@"off"] intValue]; }
            else if ([k isEqualToString:@"geometry"]) { if (reg < 0 || reg > 4) FAIL("invalid geometry helper slot"); r->kind = NVN_RB_GEOMETRY; }
            else if ([k isEqualToString:@"support"]) r->kind = NVN_RB_SUPPORT;
            else if ([k isEqualToString:@"c1"]) r->kind = NVN_RB_C1;
            else if ([k isEqualToString:@"drvcb"]) r->kind = [b[@"bank"] intValue] == 2 ? NVN_RB_DRV2 : NVN_RB_DRV0;
            else FAIL("B binding kind %s not implemented (%s)", k.UTF8String, [b[@"msl_name"] UTF8String]);
            if ((r->kind == NVN_RB_CBUF || r->kind == NVN_RB_SAMP || r->kind == NVN_RB_SAMPT || r->kind == NVN_RB_TEX || r->kind == NVN_RB_IMG || r->kind == NVN_RB_TBUF || r->kind == NVN_RB_SBUFP || r->kind == NVN_RB_UBOP || r->kind == NVN_RB_SBUFU || r->kind == NVN_RB_TEXU || r->kind == NVN_RB_SAMPU || r->kind == NVN_RB_SAMPI) && reg < 0)
                FAIL("B binding %s has no resolved slot number", [b[@"msl_name"] UTF8String]);
            o->nb++;
        }
        NSDictionary *dr = j[@"drv_reads"];
        if ([dr isKindOfClass:[NSDictionary class]]) { o->drv_read[0] = dr[@"0"] != nil; o->drv_read[2] = dr[@"2"] != nil; }
        // A declared driver-segment binding means the shader reads it: the build's drv_reads scan (dmsh_build.py) matches only fragment-shader names (fp_c0 / fp_c2), so vertex and compute
        // shaders reading c0 got an all-zero segment (Link's Awakening's light culling multiplied the tile row by a zero workgroup count: every row's lights landed in row 0)
        for (int i = 0; i < o->nb; i++) { if (o->b[i].kind == NVN_RB_DRV0) o->drv_read[0] = 1; else if (o->b[i].kind == NVN_RB_DRV2) o->drv_read[2] = 1; }
        NSArray *uf = j[@"drv_unfillable"];
        if ([uf isKindOfClass:[NSArray class]] && uf.count) FAIL("reads driver-bank locations at run time that the replay layer cannot fill: %s", [[uf componentsJoinedByString:@" "] UTF8String]);
        NSString *c1 = j[@"c1"];
        if (c1.length % 2 || c1.length / 2 > 0xffff) FAIL("c1 is %lu bytes: over the limit or an odd hex length", (unsigned long)c1.length / 2);
        if (c1.length && !(o->c1 = malloc(c1.length / 2))) FAIL("c1 allocation of %lu bytes failed", (unsigned long)c1.length / 2);
        if (c1.length) { o->has_c1 = 1; o->c1_len = (uint16_t)(c1.length / 2); for (unsigned i = 0; i < o->c1_len; i++) o->c1[i] = (uint8_t)strtoul([[c1 substringWithRange:NSMakeRange(2 * i, 2)] UTF8String], NULL, 16); }
        for (NSDictionary *v in j[@"vertex_inputs"]) {   // [[attribute(n)]]: n is the NVN vertex attribute number
            int loc = [v[@"location"] intValue]; NSString *t = v[@"msl_type"];
            if (loc < 0 || loc >= NVN_REFL_MAXV || o->nv >= NVN_REFL_MAXV) FAIL("B vertex input %d out of range", loc);
            o->v[o->nv].loc = o->v[o->nv].nvn = (uint8_t)loc;
            o->v[o->nv].ct = [t hasPrefix:@"uint"] ? NVN_CT_UINT : [t hasPrefix:@"int"] ? NVN_CT_INT : NVN_CT_FLOAT;
            memset(o->v[o->nv].sh, 0xff, sizeof o->v[o->nv].sh);
            o->nv++;
        }
        NSDictionary *vin = j[@"vin"];   // shadow inputs of the <key>_bv library (dmsh_vin.py): per input, the Metal attribute index of its float / other-sign declaration
        if ([vin isKindOfClass:[NSDictionary class]]) {
            uint32_t used = 0;
            for (NSDictionary *e in vin[@"shadow"]) {
                int loc = [e[@"location"] intValue], v = 0;
                while (v < o->nv && o->v[v].loc != loc) v++;
                if (v == o->nv || o->v[v].ct == NVN_CT_FLOAT) FAIL("shadow input for location %d, which is not an integer input", loc);
                const struct { NSString *k; int ct; } T[3] = { { @"float", NVN_CT_FLOAT }, { @"sint", NVN_CT_INT }, { @"uint", NVN_CT_UINT } };
                for (int q = 0; q < 3; q++) { id x = e[T[q].k]; if (!x) continue; int idx = [x intValue];
                    if (idx < 16 || idx > 30 || used >> idx & 1 || T[q].ct == o->v[v].ct) FAIL("shadow input %s of location %d at attribute %d: out of 16-30, used twice or of the input's own type", T[q].k.UTF8String, loc, idx);
                    used |= 1u << idx; o->v[v].sh[T[q].ct] = (uint8_t)idx; }
            }
            o->has_vin = used != 0;
        }
        NSDictionary *ra = j[@"raster"];
        o->pos_attr = -1; o->may_discard = 1; o->writes_smask = 1;
        if ([ra isKindOfClass:[NSDictionary class]]) {
            if (ra[@"pos_attr"]) { int pa = [ra[@"pos_attr"] intValue]; o->pos_attr = (int8_t)(pa >= 0 && pa < 16 ? pa : -1); o->pos_w1 = [ra[@"pos_w"] isEqual:@"one"]; }
            if (ra[@"discard"]) o->may_discard = [ra[@"discard"] intValue] != 0;
            if (ra[@"sample_mask"]) o->writes_smask = [ra[@"sample_mask"] intValue] != 0;
            for (NSNumber *c in ra[@"colors"]) if (c.intValue >= 0 && c.intValue < 8) o->color_mask |= (uint8_t)(1u << c.intValue);
            o->color_known = [ra[@"colors"] isKindOfClass:[NSArray class]];
        }
        memset(o->uv_attr, -1, sizeof o->uv_attr); memset(o->uv_aff_attr, -1, sizeof o->uv_aff_attr);
        NSDictionary *ua = [ra isKindOfClass:[NSDictionary class]] ? ra[@"uv_affine"] : nil;
        if ([ua isKindOfClass:[NSDictionary class]]) for (NSString *k in ua) { int l = k.intValue; NSDictionary *e = ua[k]; int a = [e[@"attr"] intValue];
            if (l < 0 || l >= 32 || a < 0 || a >= 16) continue;
            NSArray *refs[4] = {e[@"sx"], e[@"ox"], e[@"sy"], e[@"oy"]}; int ok = 1;
            for (int q = 0; q < 4; q++) { NSArray *r = refs[q]; if (![r isKindOfClass:[NSArray class]] || r.count != 3) { ok = 0; break; }
                int sl = [r[0] intValue], vec = [r[1] intValue], cmp = [r[2] intValue]; if (sl < 0 || sl > 15 || vec < 0 || vec > 63 || cmp < 0 || cmp > 3) { ok = 0; break; }
                o->uv_aff[l][q] = (uint16_t)(sl << 8 | vec << 2 | cmp); }
            if (ok) o->uv_aff_attr[l] = (int8_t)a; }
        NSDictionary *uv = [ra isKindOfClass:[NSDictionary class]] ? ra[@"uv_from_attr"] : nil;
        if ([uv isKindOfClass:[NSDictionary class]]) for (NSString *k in uv) { int l = k.intValue, a = [uv[k] intValue]; if (l >= 0 && l < 32 && a >= 0 && a < 16) o->uv_attr[l] = (int8_t)a; }
        NSDictionary *fe = j[@"fetch"];
        if ([fe isKindOfClass:[NSDictionary class]]) {
            for (NSDictionary *c in fe[@"calls"]) { int i = [c[@"fc"] intValue], r = [c[@"tex_reg"] intValue], l = [c[@"varying_locn"] intValue];
                if (i != o->nfc || i >= 32 || r < 0 || r >= 256 || l < 0 || l >= 32) FAIL("fetch call table not contiguous or out of range (entry %d)", i);
                o->fc_reg[o->nfc] = (uint8_t)r; o->fc_locn[o->nfc] = (uint8_t)l; o->nfc++; }
            for (NSNumber *r in fe[@"mixed_regs"]) if (r.intValue >= 0 && r.intValue < 256) o->fc_mixed[r.intValue >> 6] |= 1ull << (r.intValue & 63);
            for (NSDictionary *u in fe[@"outputs"]) { if (o->nfo >= 4) FAIL("more than 4 fetch outputs"); o->fo_color[o->nfo++] = (uint8_t)[u[@"color"] intValue]; }
        }
        NSArray *tg = j[@"threadgroup_size"];
        if ([tg isKindOfClass:[NSArray class]] && tg.count == 3) for (int i = 0; i < 3; i++) o->tg[i] = (uint16_t)[tg[i] intValue];
        return 0;
    }
    #undef FAIL
}

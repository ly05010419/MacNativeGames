// Reflection conversion unit tests: real B reflection json (translation of the original Switch shaders, build/cache/metal_shader_cache/<key>_b.json) → C arrays, expectations hand-copied from the json text.
// Dummy texture type MTLTextureType: 1D=0 1DArray=1 2D=2 2DArray=3 Cube=5 CubeArray=6 3D=7
#import "../nvn/nvn_refl.h"
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include "dmsh_chain.h"   // dm/gfx/nvn (-I from the Makefile)

enum { C = NVN_RB_CBUF, S = NVN_RB_SAMP, T = NVN_RB_TEX, I = NVN_RB_IMG, TB = NVN_RB_TBUF };
enum { F = NVN_DPF_FLOAT, U = NVN_DPF_UINT, D = NVN_DPF_DEPTH };
static int fail, n;
static void check(const char *what, long want, long got) {
    n++; int ok = want == got; fail += !ok;
    if (!ok) printf("  %-40s expected %ld → got %ld ✗\n", what, want, got);
}
static NSDictionary *load(const char *key) {
    NSString *p = [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache/b-%s/%s.json", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", DMSH_CHAIN, key];   // the library of the current translation chain version
    NSData *d = [NSData dataWithContentsOfFile:p];
    if (!d) { printf("[RED] missing reflection sample %s\n", p.UTF8String); fail++; return nil; }
    return [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
}

int main(void) { @autoreleasepool {
    nvn_refl r; char err[160] = "";
    {   int f0 = fail;   // Route A is retired: PC-chain reflection (no route = B) must be rejected
        NSDictionary *pc = @{@"arg_buffer_index": @0, @"bindings": @[@{@"type": @"cbuffer", @"reg": @10, @"arg_id": @0, @"access": @"constant", @"msl_type": @"constant cb1_struct*"}], @"vertex_inputs": @[]};
        check("PC chain reflection (no route = B) fails", 1, nvn_refl_from_json(pc, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "negative control: PC chain reflection rejected (route A retired)", fail == f0 ? "✓" : "✗");
    }

    // Route B (translation of the original Switch shaders, produced by tools/dmsh_build.py): real reflection ps_models_p#0 / vs_models_v#0, expectations hand-copied from the json text
    {   NSDictionary *j = load("5c1ca4e7c3e4657c_b");
        if (j) { int f1 = fail;
            check("B fragment: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
            check("B fragment: B flag", 1, r.route_b); check("B fragment: argument buffer slot", 0, r.abi); check("B fragment: binding count", 44, r.nb); check("B fragment: vertex input count", 0, r.nv);
            check("B fragment: has c1", 1, r.has_c1); check("B fragment: c1 length 256 (control section field)", 256, r.c1_len);
            uint32_t c1w; memcpy(&c1w, r.c1, 4); check("B fragment: first c1 word = −√1.5 (0xbf9cc471)", 0xbf9cc471u, c1w);
            struct { int i, kind, reg, aid, rw, dtt, dpf; } w[] = {
                {0, NVN_RB_SUPPORT, 0, 0, 0, -1, -1}, {1, C, 10, 1, 0, -1, -1}, {4, NVN_RB_C1, 0, 4, 0, -1, -1}, {7, NVN_RB_DRV0, 0, 7, 0, -1, -1},
                {8, NVN_RB_DRV2, 0, 8, 0, -1, -1}, {11, NVN_RB_SBUFP, 4, 11, 0, -1, -1}, {13, TB, 8, 13, 0, -1, -1}, {14, T, 41, 14, 0, 2, F},
                {15, S, 3, 15, 0, -1, -1}, {28, T, 39, 28, 0, 3, D}, {34, T, 33, 34, 0, 5, D}, {36, T, 46, 36, 0, 7, F}};
            for (unsigned k = 0; k < sizeof w / sizeof *w; k++) {
                const nvn_rbind *g = &r.b[w[k].i]; char m[64];
                snprintf(m, sizeof m, "B binding[%d] kind", w[k].i); check(m, w[k].kind, g->kind);
                snprintf(m, sizeof m, "B binding[%d] slot", w[k].i); check(m, w[k].reg, g->reg);
                snprintf(m, sizeof m, "B binding[%d] argument index", w[k].i); check(m, w[k].aid, g->aid);
                snprintf(m, sizeof m, "B binding[%d] read-write", w[k].i); check(m, w[k].rw, g->rw);
                if (w[k].dtt >= 0) { snprintf(m, sizeof m, "B binding[%d] dummy texture type", w[k].i); check(m, w[k].dtt, g->dtt);
                                     snprintf(m, sizeof m, "B binding[%d] dummy texture pixel class", w[k].i); check(m, w[k].dpf, g->dpf); }
            }
            printf("%-60s %s\n", "B fragment ps_models_p#0: 44 bindings (incl. support / c1 / driver section / storage buffer pointers)", fail == f1 ? "✓" : "✗");
        } }
    {   NSDictionary *j = load("54f730a6c0efc118_b");
        if (j) { int f1 = fail;
            check("B vertex: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
            check("B vertex: binding count", 2, r.nb); check("B vertex: input count", 4, r.nv); check("B vertex: c1 length 0 (control section +0x704 − +0x700) → no c1", 0, r.has_c1);
            for (int i = 0; i < 4 && i < r.nv; i++) { char m[48]; snprintf(m, sizeof m, "B vertex input[%d] attribute = location", i); check(m, i, r.v[i].nvn); check(m, i, r.v[i].loc); check("B vertex component float", NVN_CT_FLOAT, r.v[i].ct); }
            check("B vertex: binding[0] uniform slot 0", 0, r.b[0].reg); check("B vertex: binding[1] uniform slot 3", 3, r.b[1].reg);
            printf("%-60s %s\n", "B vertex vs_models_v#0: 4 attributes (attribute = MSL attribute)", fail == f1 ? "✓" : "✗");
        } }
    {   int f1 = fail;   // an array of handles in a uniform buffer (Paper Mario: The Thousand-Year Door b352f7af1c86e9fa_b, its last four bindings): array<…, 32> takes 32 argument entries each
        NSDictionary *j = @{@"route": @"B", @"bindings": @[
            @{@"kind": @"tex_ubo", @"reg": @11, @"off": @16, @"arg_id": @14, @"msl_name": @"fp_t_cb14_4", @"msl_type": @"texture2d<float>"},
            @{@"kind": @"samp_ubo", @"reg": @11, @"off": @16, @"arg_id": @15, @"msl_name": @"fp_t_cb14_4Smplr", @"msl_type": @"sampler"},
            @{@"kind": @"tex_ubo", @"reg": @11, @"off": @0, @"arg_id": @16, @"msl_name": @"fp_t2d_cb14_0", @"msl_type": @"array<texture2d<float>, 32>"},
            @{@"kind": @"samp_ubo", @"reg": @11, @"off": @0, @"arg_id": @48, @"msl_name": @"fp_t2d_cb14_0Smplr", @"msl_type": @"array<sampler, 32>"}]};
        check("B handle array: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B handle array: single texture n", 0, r.b[0].n); check("B handle array: single texture offset", 16, r.b[0].off); check("B handle array: single sampler n", 0, r.b[1].n);
        check("B handle array: texture kind", NVN_RB_TEXU, r.b[2].kind); check("B handle array: texture n", 32, r.b[2].n); check("B handle array: texture dummy type 2D", 2, r.b[2].dtt); check("B handle array: texture slot", 11, r.b[2].reg);
        check("B handle array: sampler kind", NVN_RB_SAMPU, r.b[3].kind); check("B handle array: sampler n", 32, r.b[3].n); check("B handle array: bindless", 1, r.bindless);
        printf("%-60s %s\n", "B array of 32 handles in uniform slot 11 (texture + sampler)", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;
        NSDictionary *b1 = @{@"route": @"B", @"bindings": @[@{@"kind": @"foo", @"arg_id": @0, @"msl_name": @"x", @"msl_type": @"constant _16*"}]};
        NSDictionary *b2 = @{@"route": @"B", @"bindings": @[@{@"kind": @"tex", @"reg": [NSNull null], @"arg_id": @0, @"msl_name": @"fp_t_x", @"msl_type": @"texture2d<float>"}]};
        check("B unknown kind fails", 1, nvn_refl_from_json(b1, &r, err, sizeof err) != 0);
        check("B texture without a resolved slot fails", 1, nvn_refl_from_json(b2, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "B negative control: unknown kind / missing slot", fail == f1 ? "✓" : "✗");
    }
    printf("== %d checks, %d failed ==\n", n, fail);
    return fail != 0;
} }

// Uniform textures (nvn_refl.m utex_slot): a texture whose combined handle the shader loads from a user uniform block gets
// texture slot NVN_UTEX_SLOT0 + i, the texture and its sampler share it, and the forms this runtime cannot bind are
// refused with a reason. Optional argument: a translated reflection file (<key>_b.json) that must load with at least
// one uniform texture, e.g. the fragment shader that stopped Super Mario Odyssey on 2026-10-22.
#import <Foundation/Foundation.h>
#import "nvn_refl.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static NSDictionary *refl(NSArray *bindings) {
    return @{@"route": @"B", @"bindings": bindings};
}
static NSDictionary *tex(NSString *name, int aid) {
    return @{@"msl_name": name, @"msl_type": @"texture2d<float>", @"arg_id": @(aid), @"kind": @"tex"};
}
static NSDictionary *samp(NSString *name, int aid) {
    return @{@"msl_name": name, @"msl_type": @"sampler", @"arg_id": @(aid), @"kind": @"samp_tex"};
}

int main(int argc, char **argv) {
    @autoreleasepool {
        nvn_refl o; char err[400];

        // The form that stopped the game: fragment texture + sampler from uniform c3 (user slot 0), word 0.
        int r = nvn_refl_from_json(refl(@[@{@"msl_name": @"fp_c3", @"msl_type": @"constant fp_c3_t&", @"arg_id": @0, @"kind": @"cbuf", @"reg": @0},
                                           tex(@"fp_t_cb3_0", 1), samp(@"fp_t_cb3_0Smplr", 2)]), &o, err, sizeof err);
        CHECK(r == 0, "cb3 word 0 loads (%s)", r ? err : "");
        CHECK(o.nutex == 1 && o.utex[0].reg == 0 && o.utex[0].off == 0, "one uniform texture: slot 0, byte 0 (n=%d reg=%d off=%d)", o.nutex, o.utex[0].reg, o.utex[0].off);
        CHECK(o.b[1].kind == NVN_RB_TEX && o.b[1].reg == NVN_UTEX_SLOT0, "texture -> slot %d (got kind %d reg %d)", NVN_UTEX_SLOT0, o.b[1].kind, o.b[1].reg);
        CHECK(o.b[2].kind == NVN_RB_SAMPT && o.b[2].reg == NVN_UTEX_SLOT0, "its sampler shares slot %d (got kind %d reg %d)", NVN_UTEX_SLOT0, o.b[2].kind, o.b[2].reg);

        // A second handle: uniform c5 (user slot 2), word 0x1A -> byte 0x68, the next slot.
        r = nvn_refl_from_json(refl(@[tex(@"fp_t_cb3_0", 0), tex(@"fp_t_cb5_1A", 1), samp(@"fp_t_cb5_1ASmplr", 2)]), &o, err, sizeof err);
        CHECK(r == 0 && o.nutex == 2 && o.utex[1].reg == 2 && o.utex[1].off == 0x68 && o.b[1].reg == NVN_UTEX_SLOT0 + 1 && o.b[2].reg == NVN_UTEX_SLOT0 + 1,
              "two uniform textures get slots %d and %d (r=%d n=%d reg=%d off=%#x)", NVN_UTEX_SLOT0, NVN_UTEX_SLOT0 + 1, r, o.nutex, o.utex[1].reg, o.utex[1].off);

        // The driver table still decodes as before: a texture the translator gave a slot keeps it.
        r = nvn_refl_from_json(refl(@[@{@"msl_name": @"fp_t_cb2_8", @"msl_type": @"texture2d<float>", @"arg_id": @0, @"kind": @"tex", @"reg": @0}]), &o, err, sizeof err);
        CHECK(r == 0 && o.nutex == 0 && o.b[0].reg == 0, "driver-table texture keeps slot 0 (r=%d n=%d reg=%d)", r, o.nutex, o.b[0].reg);

        // Refused, with a reason: separate handles, a handle in the driver bank without a slot, no handle in the name,
        // too many uniform textures.
        r = nvn_refl_from_json(refl(@[tex(@"fp_t_cb3_10000004", 0)]), &o, err, sizeof err);
        CHECK(r != 0 && strstr(err, "separate"), "separate handles refused (%s)", err);
        r = nvn_refl_from_json(refl(@[tex(@"fp_t_cb2_40", 0)]), &o, err, sizeof err);
        CHECK(r != 0 && strstr(err, "c2"), "a slotless driver-bank handle refused (%s)", err);
        r = nvn_refl_from_json(refl(@[tex(@"fp_tex0", 0)]), &o, err, sizeof err);
        CHECK(r != 0 && strstr(err, "没解出槽号"), "a name without a handle keeps the old refusal (%s)", err);
        NSMutableArray *many = [NSMutableArray array];
        for (int i = 0; i <= NVN_UTEX_MAX; i++) [many addObject:tex([NSString stringWithFormat:@"fp_t_cb3_%X", i], i)];
        r = nvn_refl_from_json(refl(many), &o, err, sizeof err);
        CHECK(r != 0 && strstr(err, "more than"), "%d uniform textures refused (%s)", NVN_UTEX_MAX + 1, err);

        if (argc > 1) {
            NSData *d = [NSData dataWithContentsOfFile:@(argv[1])];
            NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
            r = j ? nvn_refl_from_json(j, &o, err, sizeof err) : -1;
            CHECK(r == 0 && o.nutex >= 1, "%s loads with %d uniform texture(s) (%s)", argv[1], r == 0 ? o.nutex : 0, r ? (r < 0 ? "unreadable" : err) : "");
        }
        printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    }
    return fails != 0;
}

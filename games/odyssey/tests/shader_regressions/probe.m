/* Production cache matching/index loading and reflection, without a translator or GPU. */
#include "../../third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m"
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_refl.h"
#include <assert.h>

void runtime_boot_count(const char *message, uint64_t done, uint64_t total) { (void)message; (void)done; (void)total; }
void runtime_boot_status(const char *title, const char *message) { (void)title; (void)message; }

static nvn_refl reflect(NSString *kind, NSString *name, id reg, int fail) {
    NSDictionary *j = @{@"route":@"B", @"bindings":@[@{@"kind":kind, @"msl_name":name,
        @"reg":reg, @"arg_id":@7, @"msl_type":@"texture2d<float>"}]};
    nvn_refl r; char error[256];
    assert((nvn_refl_from_json(j, &r, error, sizeof error) != 0) == fail);
    return r;
}
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2); setenv("TOTK_SHADER_CACHE", argv[1], 1); setenv("TOTK_SHADER_SEED", "off", 1);
    for (NSString *type in @[@"1d", @"2d", @"3d", @"cube", @"2d_array"]) {
        assert(deps_match(@{@"tex":@{@"0":@"*"}}, @{@"tex":@{@"0":type}}));
        assert(!deps_match(@{@"tex":@{@"0":@"*_depth"}}, @{@"tex":@{@"0":type}}));
    }
    assert(deps_match(@{@"tex":@{@"0":@"*_depth"}}, @{@"tex":@{@"0":@"2d_depth"}}));
    assert(!deps_match(@{@"tex":@{@"0":@"*"}}, @{@"tex":@{@"0":@"2d_depth"}}));
    assert(!deps_match(@{@"tex":@{@"0":@"2d"}}, @{@"tex":@{@"0":@"cube"}}));
    assert(deps_match(@{@"attr":@{@"2":@"uint"}}, @{}));
    assert(deps_match(@{@"tex":@{@"0":@"*"}}, @{}));
    NSMutableArray *old = [NSMutableArray array];
    for (int i=0; i<22704; i++) [old addObject:@{@"variant":@(i%3).stringValue, @"revision":@(i)}];
    [[json_string(old) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:index_path(@"repeated") atomically:YES];
    NSMutableArray *v = variants_of(@"repeated"); assert(v.count == 3);
    assert([v[0][@"revision"] intValue] == 22701);
    NSArray *disk = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:index_path(@"repeated")] options:0 error:nil];
    assert(disk.count == 3);
    for (int i=0; i<1000; i++) variant_put(v, @{@"variant":@"0", @"revision":@(i)});
    assert(v.count == 3 && [v[0][@"revision"] intValue] == 999);
    variant_put(v, @{@"variant":@"new"}); assert(v.count == 4);

    nvn_refl r = reflect(@"tex", @"fp_t_cb3_0", NSNull.null, 0);
    assert(r.b[0].kind == NVN_RB_TEXU && r.b[0].reg == 0 && r.b[0].off == 0 && r.b[0].aid == 7);
    r = reflect(@"samp_tex", @"fp_t_cb3_1aSmplr", NSNull.null, 0);
    assert(r.b[0].kind == NVN_RB_SAMPU && r.b[0].reg == 0 && r.b[0].off == 104);
    r = reflect(@"tex", @"vp_t_cb16_3fff", NSNull.null, 0);
    assert(r.b[0].reg == 13 && r.b[0].off == 65532);
    r = reflect(@"tex", @"fp_t7", @7, 0); assert(r.b[0].kind == NVN_RB_TEX && r.b[0].reg == 7);
    for (NSString *name in @[@"fp_t_cb2_0", @"fp_t_cb17_0", @"fp_t_cb3_4000", @"fp_t_cb3_", @"fp_t_cb3_0_extra"])
        reflect(@"tex", name, NSNull.null, 1);
    reflect(@"samp_tex", @"fp_t_cb3_0", NSNull.null, 1);
    puts("shader wildcard, cache repair/upsert and uniform reflection passed");
    return 0;
} }

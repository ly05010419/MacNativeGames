// The host's own shaders (dm/gfx/msl, compiled when the host is built: dm/gfx/tools/host_msl.py) load without a compile at run time: every library of host_msl_gen.h comes from its
// metallib with no call to the device's newLibraryWithSource: (which MTL4Compiler's libraries from source also make, and the Metal performance HUD counts), through MTL4Compiler
// and without one, the same library object comes back for a name, and the clear library has its triangle and a fragment function for every colour slot and type
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
#include "../nvn/host_msl.h"
#include "host_msl_gen.h"

static int g_src_compiles, fails, checks;
static IMP g_old;
static id src_hook(id self, SEL s, NSString *src, MTLCompileOptions *o, NSError **e) { g_src_compiles++; return ((id(*)(id, SEL, NSString *, MTLCompileOptions *, NSError **))g_old)(self, s, src, o, e); }
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    Method m = class_getInstanceMethod([(id)dev class], @selector(newLibraryWithSource:options:error:));
    if (!m) { printf("FAIL: the device has no newLibraryWithSource:options:error: to count\n"); return 1; }
    g_old = method_setImplementation(m, (IMP)src_hook);
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:nil];
    int n = (int)(sizeof k_host_msl / sizeof *k_host_msl);
    CHECK(n >= 12, "%d host libraries", n);
    for (int i = 0; i < n; i++) {
        CHECK(k_host_msl[i].lib_len > 0, "%s: no metallib in the build (Metal toolchain?)", k_host_msl[i].name);
        NSError *e = nil; id<MTLLibrary> a = host_msl_lib(dev, comp, k_host_msl[i].name, &e), b = host_msl_lib(dev, nil, k_host_msl[i].name, &e);
        CHECK(a && a.functionNames.count, "%s: no library or no functions (%s)", k_host_msl[i].name, e.localizedDescription.UTF8String);
        CHECK(a == b, "%s: another library the second time", k_host_msl[i].name);
    }
    CHECK(g_src_compiles == 0, "%d libraries compiled from source", g_src_compiles);
    NSError *e = nil; id<MTLLibrary> clr = host_msl_lib(dev, comp, "clear", &e);
    NSSet *fn = [NSSet setWithArray:clr.functionNames];
    CHECK([fn containsObject:@"clr_vs"], "clear: no clr_vs");
    const char *ty[3] = {"float", "uint", "int"};
    for (int s = 0; s < 8; s++) for (int k = 0; k < 3; k++) CHECK(([fn containsObject:[NSString stringWithFormat:@"clr_fs_%d_%s", s, ty[k]]]), "clear: no clr_fs_%d_%s", s, ty[k]);
    CHECK(host_msl_lib(dev, comp, "no_such_library", &e) == nil, "an unknown name gave a library");
    printf("t_host_msl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }

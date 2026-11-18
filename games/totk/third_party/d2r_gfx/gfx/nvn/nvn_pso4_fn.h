// Ocarina of Time on the Mac: which library and entry each game function was made from, for the Metal 4 pipeline archive
// (nvn_pso4.h). MTL4Compiler builds a pipeline from function descriptors (a library and an entry name), not from MTLFunction
// objects, so every game function a pipeline can be built from is made through lib_fn, which notes them. A function with no
// note (a diagnostic override, the mesh stages, a host helper) keeps the MTLDevice path and the old archive.
// The same idea as nxnative's fn4_note / fn4_of (seedds/nxnative dm/gfx/nvn/nvn_mtl.m, MIT).
// Included by nvn_mtl.m before the shader loaders. Other games: lib_fn is newFunctionWithName and notes nothing.
#pragma once
#import <Metal/Metal.h>
#include <os/lock.h>

#if defined(GAME_OOT) && !TARGET_OS_IPHONE
#define PSO4_BUILT 1
static os_unfair_lock g_fn4_lk = OS_UNFAIR_LOCK_INIT;
/* function -> MTL4LibraryFunctionDescriptor. Weak keys: a variant function a pipeline no longer needs goes as before; the
   descriptor holds its library while the function lives. By identity: Metal may hand out one object for the same compiled
   function made twice (nvn_mtl.m g_fn_src), and either library builds it. */
static NSMapTable *g_fn4;
static id<MTLFunction> fn4_note(id<MTLFunction> f, id<MTLLibrary> lib, NSString *name) {
    if (!f || !lib || !name.length) return f;
    MTL4LibraryFunctionDescriptor *d = [MTL4LibraryFunctionDescriptor new]; d.library = lib; d.name = name;
    os_unfair_lock_lock(&g_fn4_lk);
    if (!g_fn4) g_fn4 = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                                  valueOptions:NSPointerFunctionsStrongMemory capacity:0];
    [g_fn4 setObject:d forKey:f];
    os_unfair_lock_unlock(&g_fn4_lk);
    return f;
}
static MTL4FunctionDescriptor *fn4_of(id<MTLFunction> f) {
    if (!f) return nil;
    os_unfair_lock_lock(&g_fn4_lk); MTL4FunctionDescriptor *d = [g_fn4 objectForKey:f]; os_unfair_lock_unlock(&g_fn4_lk);
    return d;
}
#endif

static inline id<MTLFunction> lib_fn(id<MTLLibrary> lib, NSString *name) {
    id<MTLFunction> f = lib && name ? [lib newFunctionWithName:name] : nil;
#ifdef PSO4_BUILT
    fn4_note(f, lib, name);
#endif
    return f;
}

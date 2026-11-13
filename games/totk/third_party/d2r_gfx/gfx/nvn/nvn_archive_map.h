#pragma once
/* A pipeline archive is mapped, not read into memory, wherever the app is (Mac, every game; 2026-11-13).

   Metal opens an MTLBinaryArchive through -[NSData initWithContentsOfURL:options:error:] with NSDataReadingMappedIfSafe,
   and Foundation counts a file on an external drive on USB as unsafe to map: the whole archive is read into allocated
   memory and stays for the run. Ocarina of Time from /Volumes/开发 (USB, external): 2,946 NSData of 5.9 GB in the heap for
   its 3,129 archive files (5.7 GB), a footprint of 18.2 GB on a 16 GB Mac. On the internal disk the same files are mapped:
   clean pages the system can drop, not counted in the footprint. MTLBinaryArchiveDescriptor takes only a URL, so while the
   host opens an archive (arch_map_begin / arch_map_end around newBinaryArchiveWithDescriptor) that public NSData method asks
   for NSDataReadingMappedAlways for a .metalarchive file on this thread; every other read keeps its options. Measured here:
   20 archives of 29.2 MB, footprint +33.8 MB read, +4.4 MB mapped (tests/test_archive_map.py). Found by seedds/nxnative
   (docs/reference/memory.md "A pipeline archive on an external drive is read into memory", which replaces the private method
   behind it for MTL4 archives); written anew for the public method. The app already runs from that drive, so a drive pulled
   out ends the run either way. iOS keeps its files on the internal disk: nothing changes there. */
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdatomic.h>

static _Thread_local int g_arch_map_depth;           // archive opens in progress on this thread
static _Atomic uint64_t g_arch_mapped;               // archive files opened mapped (the log's count)
typedef id (*arch_map_init_t)(id, SEL, NSURL *, NSDataReadingOptions, NSError **);
static arch_map_init_t g_arch_map_orig;

static id arch_map_read(id self, SEL sel, NSURL *url, NSDataReadingOptions options, NSError **error) {
    if (g_arch_map_depth > 0 && url.isFileURL && [url.pathExtension isEqualToString:@"metalarchive"]) {
        options = (options & ~(NSDataReadingOptions)NSDataReadingMappedIfSafe) | NSDataReadingMappedAlways;
        atomic_fetch_add(&g_arch_mapped, 1);
    }
    return g_arch_map_orig(self, sel, url, options, error);
}
static void arch_map_install(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Method m = class_getInstanceMethod(NSData.class, @selector(initWithContentsOfURL:options:error:));
        if (m) g_arch_map_orig = (arch_map_init_t)method_setImplementation(m, (IMP)arch_map_read);
    });
}
static inline void arch_map_begin(void) { arch_map_install(); if (g_arch_map_orig) g_arch_map_depth++; }
static inline void arch_map_end(void) { if (g_arch_map_orig && g_arch_map_depth > 0) g_arch_map_depth--; }
static inline uint64_t arch_map_count(void) { return atomic_load(&g_arch_mapped); }
#else
static inline void arch_map_begin(void) {}
static inline void arch_map_end(void) {}
static inline uint64_t arch_map_count(void) { return 0; }
#endif

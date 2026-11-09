// TOTK: which pipeline archive file holds each key (par_load_dir). Included by totk_pipeline_archive.h after its globals and file names; MLOG comes from its includer.
#pragma once
/* The keys of the files in the folder (g_par_have, g_par_where); removes what a write cut short left, unless another
   process is publishing (totk_pipeline_archive_publish.h par_pub_scan). Returns the files' bytes. A key in two files is the
   newest file's: the later modification time of its key list, then the larger name,
   so it does not depend on the order the folder is listed in. */
static uint64_t par_load_dir(NSUInteger *files) {
    NSFileManager *fm = NSFileManager.defaultManager;
    g_par_have = [NSMutableSet set]; g_par_where = [NSMutableDictionary dictionary]; g_par_pending = [NSMutableSet set];
    g_par_open = [NSMutableDictionary dictionary]; uint64_t bytes = 0; *files = 0;
    for (NSString *base in par_pub_scan(par_dir(), par_lock_file(), YES, [NSSet setWithObject:@"identity.json"])) {   // oldest first
        NSString *list = [NSString stringWithContentsOfFile:par_keys_file(base) encoding:NSUTF8StringEncoding error:nil];
        for (NSString *key in [list componentsSeparatedByString:@"\n"]) if (key.length) { [g_par_have addObject:key]; g_par_where[key] = base; }
        bytes += [[fm attributesOfItemAtPath:par_file(base) error:nil] fileSize]; ++*files;
    }
    return bytes;
}

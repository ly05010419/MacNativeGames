/* probe-gpu-pipeline-archive, part two. */
/* The folder's identity is written only under the shared lock: with the lock file unopenable (a folder in its place),
   par_open_identity writes no identity.json and keeps the archive off (0); once it opens again, it writes it (1). */
static int par_probe_identity_lock(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *stamp = [par_dir() stringByAppendingPathComponent:@"identity.json"];
    [fm removeItemAtPath:par_dir() error:nil]; [fm removeItemAtPath:par_lock_file() error:nil];
    if (![fm createDirectoryAtPath:par_lock_file() withIntermediateDirectories:YES attributes:nil error:nil]) MDIE("pipeline archive identity probe: cannot block the lock file");
    int opened = par_open_identity(g_dev), written = [fm fileExistsAtPath:stamp];
    [fm removeItemAtPath:par_lock_file() error:nil];
    if (opened || written) MDIE("pipeline archive identity probe: without the lock the folder was opened (%d) or identity.json written (%d)", opened, written);
    if (!par_open_identity(g_dev) || ![fm fileExistsAtPath:stamp]) MDIE("pipeline archive identity probe: with the lock back identity.json was not written");
    return 2;
}

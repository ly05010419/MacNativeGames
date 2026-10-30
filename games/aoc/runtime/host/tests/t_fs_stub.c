// Stub for t_fs: strong implementation of the front end's save-failure notification (a separate compilation unit that replaces the weak implementation in hle_fs.c at link time, the same way the iOS front end does it)
int g_nfail, g_lasterr;
void front_save_failed(int err) { g_nfail++; g_lasterr = err; }

// the loader's module table (dm.h): the unit test has no modules, so the table is empty
#include "../dm.h"
module_t g_mod[MAX_MOD];
int g_nmod;
// the guest address space (hle_svc.c): the unit test has none, so every stack is a host stack and the big-stack takeovers run on it directly (hle_bigstack.c)
#include <stdint.h>
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
// main's imports (loader.c) and the MiiModel stand-in (hle_mii.c): no main module, no system data
int dm_main_imports(const char *prefix) { (void)prefix; return 0; }
int dm_mii_standin(const char *dir) { (void)dir; return 0; }

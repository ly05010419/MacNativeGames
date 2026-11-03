// the loader's module table (loader.c), which crash.c's report walks: the unit test has no modules
#include "../dm.h"
module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return 0; }

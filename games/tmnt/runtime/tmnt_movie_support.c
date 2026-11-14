#include "tmnt_movie_compat.h"
#include <pthread/qos.h>
void dm_host_lock(pthread_mutex_t *m) { (void)dm_host_mutex_lock(m); }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
typedef int (*create_fn)(void *,void (*)(void *),void *,void *,size_t,int);
typedef void (*thread_fn)(void *);
typedef struct { _Alignas(16) uint8_t tt[0x200]; void *stack_alloc; } TmntHelperThread;
static void *need_sym(const char *name) { void *p=runtime_symbol(name); if (!p) runtime_fail("movie guest thread symbol missing"); return p; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    stack_size=(stack_size+0xfff)&~(size_t)0xfff; TmntHelperThread *t=calloc(1,sizeof *t); if(!t) runtime_fail("movie helper alloc");
    t->stack_alloc=((void *(*)(size_t))need_sym("malloc"))(stack_size+0xfff); if (!t->stack_alloc) runtime_fail("movie guest stack alloc");
    void *stack=(void *)(((uintptr_t)t->stack_alloc+0xfff)&~(uintptr_t)0xfff);
    int rc=((create_fn)need_sym("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))(t->tt,fn,arg,stack,stack_size,prio);
    if(rc) runtime_fail("movie guest thread create"); ((thread_fn)need_sym("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(t->tt); return t;
}

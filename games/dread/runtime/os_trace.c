/* DREAD_OS_TRACE=1 (diagnostic, off by default): main's calls that link, unlink and finalize multi-wait holders and
   finalize events are logged with the calling guest thread, then run by the SDK's own nn::os code unchanged. Used to
   order the movie player's holder handling against decoder teardown. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t runtime_thread_current_handle(void);
static void *orig(const char *name) { void *p = runtime_symbol(name); if (!p) runtime_fail("os trace: SDK symbol missing"); return p; }
#define N_LINK "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE"
#define N_UNLINK "_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"
#define N_INIT "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"
#define N_FINI "_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"
#define N_FEV "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE"
static void link(void *mw, void *h) { fprintf(stderr, "[os] t=%#x link %p -> %p\n", runtime_thread_current_handle(), h, mw); ((void (*)(void *, void *))orig(N_LINK))(mw, h); }
static void unlink_(void *h) { fprintf(stderr, "[os] t=%#x unlink %p\n", runtime_thread_current_handle(), h); ((void (*)(void *))orig(N_UNLINK))(h); }
static void init(void *h, void *ev) { fprintf(stderr, "[os] t=%#x init %p event %p\n", runtime_thread_current_handle(), h, ev); ((void (*)(void *, void *))orig(N_INIT))(h, ev); }
static void fini(void *h) { fprintf(stderr, "[os] t=%#x finalize holder %p\n", runtime_thread_current_handle(), h); ((void (*)(void *))orig(N_FINI))(h); }
static void fev(void *ev) { fprintf(stderr, "[os] t=%#x finalize event %p\n", runtime_thread_current_handle(), ev); ((void (*)(void *))orig(N_FEV))(ev); }
void *runtime_os_trace_override(const char *name) {
    static int on = -1; if (on < 0) on = getenv("DREAD_OS_TRACE") != NULL;
    if (!on) return NULL;
    if (!strcmp(name, N_LINK)) return link;
    if (!strcmp(name, N_UNLINK)) return unlink_;
    if (!strcmp(name, N_INIT)) return init;
    if (!strcmp(name, N_FINI)) return fini;
    if (!strcmp(name, N_FEV)) return fev;
    return NULL;
}

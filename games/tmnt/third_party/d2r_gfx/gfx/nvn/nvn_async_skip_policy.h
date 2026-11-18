#ifndef TMNT_NVN_ASYNC_SKIP_POLICY_H
#define TMNT_NVN_ASYNC_SKIP_POLICY_H

#include "nvn_obj.h"

/* A swapchain target may be presented before another draw replaces its clear.
   Callers must resolve its shader instead of treating that draw as disposable. */
static inline int nvn_async_target_is_swapchain(const p_set_rt *rt,
                                                 int (*is_window_serial)(uint64_t)) {
    if (!rt || !is_window_serial) return 0;
    for (int i = 0; i < rt->n && i < 8; i++)
        if (rt->color[i] && is_window_serial(rt->color[i]->serial)) return 1;
    return 0;
}

#endif

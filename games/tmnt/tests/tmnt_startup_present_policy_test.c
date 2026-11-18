#include <assert.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_startup_present_policy.h"

int main(void) {
    /* The observed setup sequence must not expose the new window texture. */
    assert(!nvn_startup_present_ready(0, 1, 0));
    /* A first GPU pass that only clears the window is still a valid frame. */
    assert(nvn_startup_present_ready(0, 1, 1));
    /* Other targets and already-rendered window images retain their meaning. */
    assert(nvn_startup_present_ready(0, 0, 0));
    assert(nvn_startup_present_ready(1, 1, 0));
    assert(nvn_startup_present_ready(1, 1, 1));
    return 0;
}

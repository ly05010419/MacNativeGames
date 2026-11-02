#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_scan_budget.h"
#include <assert.h>

int main(void) {
    nvn_scan_budget b = {0};
    assert(nvn_scan_budget_left(&b, 1, 4000000, 1));
    b.used_ns = 4000000;
    assert(!nvn_scan_budget_left(&b, 1, 4000000, 1));
    nvn_scan_budget_build(&b, 1, 23000000);
    assert(nvn_scan_budget_left(&b, 1, 4000000, 1));
    assert(!nvn_scan_budget_left(&b, 1, 4000000, 0));
    b.used_ns = 23000000;
    assert(!nvn_scan_budget_left(&b, 1, 4000000, 1));
    // Spending the scan budget never replenishes it; a later real build can.
    nvn_scan_budget_build(&b, 1, 1000000);
    assert(nvn_scan_budget_left(&b, 1, 4000000, 1));
    assert(nvn_scan_budget_left(&b, 2, 4000000, 1));
    assert(b.used_ns == 0 && b.sync_ns == 0);
    assert(!nvn_scan_budget_left(&b, 3, 0, 1));
    nvn_scan_budget_build(&b, 3, 5);
    assert(nvn_scan_budget_left(&b, 3, 0, 1));
    b.sync_ns = UINT64_MAX - 1;
    nvn_scan_budget_build(&b, 3, 2);
    assert(b.sync_ns == UINT64_MAX);
    return 0;
}

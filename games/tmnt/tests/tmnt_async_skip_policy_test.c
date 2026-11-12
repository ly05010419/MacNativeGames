#include <assert.h>
#include <stdint.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_async_skip_policy.h"

static int is_window(uint64_t serial) { return serial == 12 || serial == 13; }

int main(void) {
    tex_t offscreen = {.serial = 7};
    tex_t first_window = {.serial = 12};
    tex_t second_window = {.serial = 13};
    p_set_rt rt = {.n = 2, .color = {&offscreen, NULL}};
    assert(!nvn_async_target_is_swapchain(NULL, is_window));
    assert(!nvn_async_target_is_swapchain(&rt, NULL));
    assert(!nvn_async_target_is_swapchain(&rt, is_window));
    rt.color[1] = &first_window;
    assert(nvn_async_target_is_swapchain(&rt, is_window));
    rt.color[1] = &second_window;
    assert(nvn_async_target_is_swapchain(&rt, is_window));
    rt.n = 1;
    assert(!nvn_async_target_is_swapchain(&rt, is_window));
    rt.n = 0;
    rt.color[0] = &first_window;
    assert(!nvn_async_target_is_swapchain(&rt, is_window));
    return 0;
}

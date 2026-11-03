// The running game's addresses (addresses.h).

#include "addresses.h"

namespace ra2 {

// Red Alert 2: main loop 0x53FBD0 (sets 0xA6EB30), the view's update
// 0x6A2DD0, the sidebar's layout 0x67B910 (its view rectangle at 0x839630:
// x, y, w, h).
const GameAddresses kRa2Addresses = {
    .in_main_loop = 0xA6EB30,
    .tactical_ptr = 0x83996C,
    .clamp_to_map = 0x6A8C80,
    .view_moved = 0x6A9170,
    .unclamped = 0xA40D13,
    .view_y = 0x839634,
    .view_h = 0x83963C,
    .data_va = 0x7C9000,
    .data_size = 0x3639C4,
};

// Yuri's Revenge: main loop 0x55D360 (sets 0xABCD58), the view's update
// 0x6D2540, the sidebar's layout 0x6A5130 (the view rectangle at 0x886FA0).
const GameAddresses kYrAddresses = {
    .in_main_loop = 0xABCD58,
    .tactical_ptr = 0x887324,
    .clamp_to_map = 0x6D8640,
    .view_moved = 0x6D8B30,
    .unclamped = 0xA8ED6B,
    .view_y = 0x886FA4,
    .view_h = 0x886FAC,
    .data_va = 0x812000,
    .data_size = 0x367BE4,
};

namespace {
const GameAddresses* g_addresses = &kRa2Addresses;
}  // namespace

const GameAddresses& Addresses() { return *g_addresses; }
void UseAddresses(const GameAddresses& a) { g_addresses = &a; }

}  // namespace ra2

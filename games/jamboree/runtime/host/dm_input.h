// Front end (Mac keyboard / iOS touchscreen) -> nn::hid input state. Button bits match nn::hid::NpadButton:
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; sticks ±32767.
#pragma once
#include <stdint.h>
// Button bits (= nn::hid::NpadButton bit order; the DM_KEYS key-name table in hle_hid.c is generated from here). Frontends always compose buttons from these names.
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
};
#define DM_BTN_SHAKE (1ull << 63)   // not an NpadButton: the controller is shaken while set (its six-axis sensor, hle_hid.c); never reaches the game's buttons
#define DM_STICK_MAX 32767
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t dm_input_samples(void);   // Cumulative number of times the game has sampled the controller
// Physical controller path (Mac GameController, DM_KEYS / DM_CTL): stored separately from the keyboard / touch input; when the game reads player 1, buttons are
// ORed bitwise and each stick axis takes whichever source is deflected more. All 0 when no controller is connected, so values are bit-for-bit the same as before.
// dm_pad_set is player 1's controller (DM_BTN_SHAKE counts there only); dm_pad_set_player sets player n + 1's, which the game reads on its own npad (hle_hid.c) while
// dm_pad_players has its bit set; a player whose npad the game does not support (SetSupportedNpadIdType) feeds player 1 instead, so a one-player game sees every controller
#define DM_PLAYERS 8   // npads No1..No8; player 1 is the Handheld npad in handheld mode (DM_OPMODE=0)
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void dm_pad_set_player(int player, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);   // player 0..DM_PLAYERS-1 (player 1..8)
void dm_pad_players(uint32_t mask);   // bit p: player p + 1 has a controller; player 1 always counts as connected (the keyboard, the touch gamepad, DM_KEYS)
uint32_t dm_player_npad(int player);  // the npad id the game reads player p + 1 on: Handheld (0x20) or No1 (0) for player 1, No n (id n - 1) for player n

// Optional diagnostic DM_HID_RECEIPT_LOG=<writable JSONL path>: received samples of real GetNpadState returns.
// Fields read_start_ns/time_ns use the same host now_ns(); the front-end publish window must use the same helper, and a read interval that spans a publish must not be force-matched.
// State changes + at most one activity witness per second, limit_reached after 1024 entries; no file is written when the environment variable is not set.
// The actual return includes id/style/sampling/buttons/four axes/attributes/script_mask; front-end output is not treated as received.
// No new line does not prove that a blank touch point was read; a valid connection / sampling activity / evidence from the original UI and command packets is required; the log has IO overhead and is not used for timing.

// Private skill-aim-v1: takes only paired received samples of the right axis that GetNpad has actually returned, not values the front end has not yet had consumed.
void dm_input_sampled_right(int32_t *rx,int32_t *ry);

// Controller state injection point: called after GetNpadState has filled in the original state and before the pseudo-key override. The Mac host touch point script (touch_mac.c, Mac only, kept out of the iOS app by the _mac convention) registers at startup; iOS does not carry that file and it stays empty
extern void (*dm_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

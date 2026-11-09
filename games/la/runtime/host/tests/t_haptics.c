// Exercise the same state interface used by HID and the Apple output, with a recording sink.
#include "hle_haptics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static dm_haptic_state last[DM_PLAYERS];
static int calls;
static void record(dm_haptic_state s) { last[s.player] = s; calls++; }
static void send(int p, int m, float a) {
    dm_vibration_update u = {p, m, {a, 160, 0, 320}}; dm_haptics_send(&u, 1);
}
int main(void) {
    dm_haptics_init(record, 1);
    assert(!dm_haptics_available());
    send(0, 0, 0.5); assert(dm_haptics_actual(0, 0).amplitude_low == 0);
    dm_haptics_route(0, 1); dm_haptics_route(1, 1);
    calls = 0;
    dm_vibration_update batch[] = {{0, 0, {0.2, 160, 0, 320}}, {0, 1, {0.4, 160, 0, 320}}};
    dm_haptics_send(batch, 2);
    assert(calls == 1 && last[0].motor[0].amplitude_low == 0.2f && last[0].motor[1].amplitude_low == 0.4f);
    send(1, 0, 0.7); send(0, 1, 0);
    assert(last[1].motor[0].amplitude_low == 0.7f && last[0].motor[0].amplitude_low == 0.2f);
    calls = 0; send(0, 1, 0.3); send(0, 1, 0);
    assert(calls == 2 && last[0].motor[1].amplitude_low == 0);   // both edges of a short pulse survive
    usleep(600000);
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.2f);   // steady state never times out
    uint64_t old = last[0].epoch;
    dm_haptics_route(0, 0); dm_haptics_route(0, 1);
    assert(dm_haptics_epoch(0) != old && dm_haptics_actual(0, 0).amplitude_low == 0);
    assert(dm_haptics_actual(1, 0).amplitude_low == 0.7f);   // other player unaffected
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1);
    dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, 1);
    send(1, 0, 0.8);
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0);
    send(1, 0, 0.8); assert(dm_haptics_actual(1, 0).amplitude_low == 0);
    dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, 0);
    assert(dm_haptics_actual(1, 0).amplitude_low == 0);
    send(1, 0, 0.8); assert(dm_haptics_actual(1, 0).amplitude_low == 0.8f);
    dm_vibration_update bad = {1, 1, {NAN, INFINITY, 2, -1}};
    dm_haptics_send(&bad, 1);
    dm_vibration v = dm_haptics_actual(1, 1);
    assert(v.amplitude_low == 0 && v.amplitude_high == 1 && v.frequency_low == 160 && v.frequency_high == 320);
    calls = 0; send(-1, 0, 1); send(8, 0, 1); send(0, 2, 1); assert(!calls);
    dm_vibration low = {0.2, 80, 0, 320}, high = {0.2, 640, 0, 320};
    dm_haptic_level l = dm_haptics_level(&low, 1, 1), h = dm_haptics_level(&high, 1, 1);
    assert(l.intensity == h.intensity && l.sharpness < h.sharpness);
    assert(dm_haptics_level(&low, 1, 0).intensity == 0);
    assert(fabsf(dm_haptics_level(&low, 1, 2).intensity - 2 * l.intensity) < 0.0001f);
    dm_haptics_init(record, 0); dm_haptics_route(0, 1); send(0, 0, 1);
    assert(!dm_haptics_available() && dm_haptics_actual(0, 0).amplitude_low == 0);
    puts("haptics: state, timing, routing, sanitization and mapping passed");
}

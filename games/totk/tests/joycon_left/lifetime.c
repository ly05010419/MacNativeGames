#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "hid_device_lifetime.h"

enum { SESSIONS = 4 };
typedef struct {
    HidDeviceLifetime device;
    unsigned char phase[SESSIONS];   /* unopened, bound, retired with pending output, drained */
    unsigned opens, closes;
} Schedule;
static uint64_t histories;

static void enumerate(Schedule s) {
    int complete = 1;
    for (int i = 0; i < SESSIONS; i++) {
        if (s.phase[i] == 3) continue;
        complete = 0;
        Schedule next = s;
        if (next.phase[i] == 0) {
            if (next.device.closed) { next.device = (HidDeviceLifetime){0}; ++next.opens; }
            assert(hid_lifetime_bind(&next.device));
        } else if (next.phase[i] == 1) {
            assert(hid_lifetime_retire(&next.device));
        } else assert(hid_lifetime_finish(&next.device));
        ++next.phase[i];

        unsigned bound = 0, pending = 0;
        for (int j = 0; j < SESSIONS; j++) { bound += next.phase[j] == 1; pending += next.phase[j] == 2; }
        assert(next.device.bindings == bound && next.device.retiring == pending);
        if (!bound && !pending) {
            assert(hid_lifetime_close(&next.device)); ++next.closes;
            assert(!hid_lifetime_close(&next.device));   /* never close twice */
        } else assert(!hid_lifetime_close(&next.device));   /* no close while any session needs the object */
        assert(next.closes <= next.opens);
        enumerate(next);
    }
    if (complete) {
        assert(s.device.closed && s.opens == s.closes);
        ++histories;
    }
}

int main(void) {
    HidDeviceLifetime s = {0};
    assert(!hid_lifetime_retire(&s) && !hid_lifetime_finish(&s));
    assert(hid_lifetime_bind(&s) && hid_lifetime_retire(&s));
    assert(hid_lifetime_bind(&s) && hid_lifetime_retire(&s));
    assert(hid_lifetime_finish(&s) && !hid_lifetime_close(&s));   /* the exact overlapping-retirement regression */
    assert(hid_lifetime_finish(&s) && hid_lifetime_close(&s));
    assert(!hid_lifetime_bind(&s) && !hid_lifetime_finish(&s) && !hid_lifetime_close(&s));
    s = (HidDeviceLifetime){.bindings = UINT_MAX}; assert(!hid_lifetime_bind(&s));
    s = (HidDeviceLifetime){.bindings = 1, .retiring = UINT_MAX};
    assert(!hid_lifetime_retire(&s) && s.bindings == 1 && s.retiring == UINT_MAX);

    enumerate((Schedule){.device.closed = 1});   /* no open object until the first bind */
    assert(histories == 369600);   /* 12! / (3!)^4: every legal ordering of four bind/retire/drain sequences */
    printf("PASS: %" PRIu64 " lifecycle schedules, no premature/double close, balanced reopen, invalid transitions\n", histories);
    return 0;
}

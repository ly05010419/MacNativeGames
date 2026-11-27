/* The room around Ocarina of Time's screens (zeldaVR/runtime/oot_vr_passthrough.h): the key colour, the dark border, the push out of
   the key's reach, the link payload and when the key goes on and off. ALVR's mask is the one of its 20.14.0 stream.wgsl. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_passthrough.h"

static float mask_of(int r, int g, int b, const uint8_t key[3], int threshold) {
    const float c[3] = { r / 255.0f, g / 255.0f, b / 255.0f };
    return oot_pt_alvr_mask(c, key, threshold, OOT_PT_FEATHER_PCT);
}

static void key_colour(void) {
    uint8_t key[3]; oot_pt_key(key);
    assert(key[0] == 255 && key[1] == 0 && key[2] == 255);
    assert(mask_of(255, 0, 255, key, OOT_PT_THRESHOLD) == 0);            /* the key itself: the room */
    assert(mask_of(250, 6, 249, key, OOT_PT_THRESHOLD) == 0);            /* and the key after a few levels of codec noise */
    assert(mask_of(5, 5, 6, key, OOT_PT_THRESHOLD) == 1);                /* the dark border and clear colour: the picture */
    /* the codec smears the key into the dark border at its edge: any mix of the two is darker than the key, never keyed */
    for (int i = 1; i <= 100; i++) {
        const float t = i / 100.0f, d = 5 / 255.0f;
        const float c[3] = { 1 - t * (1 - d), d * t, 1 - t * (1 - 6 / 255.0f) };
        const float m = oot_pt_alvr_mask(c, key, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT);
        if (t >= 0.2f) assert(m == 1);
    }
    /* why not ALVR's green: bright greens of the game (Kokiri Forest's grass, Hyrule Field in the sun) are keyed by ALVR's default
       green key (0, 255, 0, threshold 85) after its sRGB decode, and not by our magenta */
    const uint8_t green[3] = { 0, 255, 0 };
    const int grass[][3] = { { 110, 230, 70 }, { 90, 220, 40 }, { 130, 240, 100 } };
    for (int i = 0; i < 3; i++) {
        assert(mask_of(grass[i][0], grass[i][1], grass[i][2], green, 85) < 1);
        assert(mask_of(grass[i][0], grass[i][1], grass[i][2], key, OOT_PT_THRESHOLD) == 1);
    }
}

static void guard(void) {
    uint8_t key[3]; oot_pt_key(key);
    int moved = 0, n = 0;
    for (int r = 0; r <= 255; r += 5) for (int g = 0; g <= 255; g += 5) for (int b = 0; b <= 255; b += 5) {
        float c[3] = { r / 255.0f, g / 255.0f, b / 255.0f }; const float was[3] = { c[0], c[1], c[2] };
        const float before = oot_pt_alvr_mask(c, key, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT);
        const int m = oot_pt_guard(c); moved += m; n++;
        /* after the push nothing of the picture is keyed, not even partly */
        assert(oot_pt_alvr_mask(c, key, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT) == 1);
        if (!m) assert(c[0] == was[0] && c[1] == was[1] && c[2] == was[2]);
        if (before < 1) assert(m);                                        /* everything the key reaches is pushed */
        if (m) {                                                          /* one channel moved, the others kept */
            int same = 0; for (int k = 0; k < 3; k++) same += c[k] == was[k];
            assert(same == 2);
        }
    }
    assert(moved > 0 && moved < n / 20);                                  /* a corner of the colours: under 5% of them */
    float pink[3] = { 250 / 255.0f, 120 / 255.0f, 250 / 255.0f };       /* a light pink-violet: out of reach after the push */
    assert(oot_pt_alvr_mask(pink, key, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT) < 1);
    assert(oot_pt_guard(pink) && oot_pt_alvr_mask(pink, key, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT) == 1);
    float grey[3] = { 0.5f, 0.5f, 0.5f }; assert(!oot_pt_guard(grey));
}

static void payload(void) {
    uint8_t p[8];
    oot_pt_payload(1, p);
    const uint8_t on[8] = { 1, 255, 0, 255, 64, 5, 0, 0 };
    for (int i = 0; i < 8; i++) assert(p[i] == on[i]);
    oot_pt_payload(0, p);
    assert(p[0] == 0 && p[1] == 255 && p[4] == 64 && p[6] == 0 && p[7] == 0);
}

static void steps(void) {
    const uint64_t ms = 1000000ull;
    oot_pt_state s; oot_pt_init(&s);
    oot_pt_step_out o = oot_pt_step(&s, 0, 1, 1, 0);                     /* option off: nothing sent, nothing keyed */
    assert(!o.send && !o.fill_key && !o.guard);
    o = oot_pt_step(&s, 1, 0, 1, 1 * ms);                                 /* an older streamer: nothing ever */
    assert(!o.send && !o.fill_key && !o.guard);
    o = oot_pt_step(&s, 1, 1, 0, 2 * ms);                                 /* the head view: the key stays off */
    assert(!o.send && !o.fill_key && !o.guard);
    o = oot_pt_step(&s, 1, 1, 1, 10 * ms);                                /* a screen: on, at once */
    assert(o.send && o.send_on && o.fill_key && o.guard);
    o = oot_pt_step(&s, 1, 1, 1, 20 * ms); assert(!o.send && o.fill_key);
    o = oot_pt_step(&s, 1, 1, -1, 30 * ms); assert(o.fill_key);         /* no picture this frame: as it was */
    o = oot_pt_step(&s, 1, 1, 1, 1010 * ms); assert(o.send && o.send_on); /* said again each second */
    o = oot_pt_step(&s, 1, 1, 0, 1100 * ms);                              /* the head view again: no key fill, the push, still on */
    assert(!o.fill_key && o.guard && !o.send);
    o = oot_pt_step(&s, 1, 1, 0, 1300 * ms); assert(o.guard && !o.send);
    o = oot_pt_step(&s, 1, 1, 0, 1420 * ms);                              /* OOT_PT_HOLD_NS after the last keyed frame: off */
    assert(o.send && !o.send_on && !o.fill_key);
    o = oot_pt_step(&s, 1, 1, 0, 1500 * ms); assert(!o.send && !o.guard);
    o = oot_pt_step(&s, 1, 1, 0, 9000 * ms); assert(!o.send);             /* off is not said again */
    o = oot_pt_step(&s, 1, 1, 1, 9100 * ms); assert(o.send && o.send_on);
    o = oot_pt_step(&s, 0, 1, 1, 9200 * ms);                              /* the option off on a screen: the fill stops, then off */
    assert(!o.fill_key && o.guard && !o.send);
    o = oot_pt_step(&s, 0, 1, 1, 9600 * ms); assert(o.send && !o.send_on);
    o = oot_pt_step(&s, 1, 1, 1, 9700 * ms); assert(o.send && o.send_on);
    o = oot_pt_step(&s, 1, 0, 1, 9800 * ms); assert(!o.send && !o.fill_key);   /* the link went down */
    o = oot_pt_step(&s, 1, 1, 1, 9900 * ms); assert(o.send && o.send_on);       /* a new link: said again */
}

int main(void) {
    key_colour(); guard(); payload(); steps();
    puts("oot_vr_passthrough: ok");
    return 0;
}

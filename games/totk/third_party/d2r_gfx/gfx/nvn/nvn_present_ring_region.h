#ifndef NVN_PRESENT_RING_REGION_H
#define NVN_PRESENT_RING_REGION_H
#include "nvn_capture_region.h"

/* What a present-ring slot (TOTK_PRESENT_RING) holds of the game's window image, and where the present finds it.
   The display shows the present's crop only, so the slot is the crop alone: Ocarina of Time's window images are
   3840x2160 (the 4K canvas) while a 1080p canvas shows 0,0 1920x1080, and copying the whole image took 0.36-0.45 ms
   of GPU time a frame and four 33 MB slots (2026-11-18). The whole image when there is no crop, when the crop is the
   image, and when the crop is invalid: the present then gets the game's own crop and refuses it as before. */
typedef struct { size_t x, y, w, h; int picture; } nvn_ring_region;   // picture: the slot is the crop alone

static inline nvn_ring_region nvn_ring_region_for(size_t width, size_t height, const int crop[4]) {
    nvn_capture_region r;
    if (crop && nvn_capture_source_region(width, height, crop, &r) && (r.w < width || r.h < height))
        return (nvn_ring_region){r.x, r.y, r.w, r.h, 1};
    return (nvn_ring_region){0, 0, width, height, 0};
}

/* The crop a present reads a slot with: all of a slot that is the picture, the game's own crop of a whole image. */
static inline const int *nvn_ring_present_crop(int picture, size_t slot_w, size_t slot_h, const int *crop, int out[4]) {
    if (!picture) return crop;
    out[0] = 0; out[1] = 0; out[2] = (int)slot_w; out[3] = (int)slot_h;
    return out;
}
#endif

/* SMAA 1x preset controls. Reference LOW / MEDIUM / HIGH / ULTRA search limits, with scaled edge thresholds.
   User-tested controls are fixed at 400% edge sensitivity and 100% blend; old slider files cannot override them. */
#pragma once
#include <stdatomic.h>
#include <stdint.h>
enum { SMAA_LOW, SMAA_MEDIUM, SMAA_HIGH, SMAA_ULTRA };
typedef struct {
    float threshold;
    uint32_t search_steps, diagonal_steps;
    float corner_rounding, blend;
    uint32_t srgb, padding[2];
} SmaaParams;
_Static_assert(sizeof(SmaaParams) == 32, "SMAA Metal constant layout");
static _Atomic int g_smaa_quality = SMAA_HIGH;
static inline SmaaParams smaa_config(int quality, unsigned srgb) {
    static const float threshold[] = {.15f,.1f,.1f,.05f};
    static const uint32_t steps[] = {4,8,16,32}, diagonal[] = {0,0,8,16};
    if (quality < SMAA_LOW || quality > SMAA_ULTRA) quality = SMAA_HIGH;
    return (SmaaParams){.threshold=threshold[quality]*.25f,
        .search_steps=steps[quality],.diagonal_steps=diagonal[quality],.corner_rounding=.25f,
        .blend=1.f,.srgb=!!srgb};
}

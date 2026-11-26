/* Deterministic one-device-per-port assignment. Explicit reservations precede
   automatic slots; absent reserved devices never fall back to another pad.
   Inputs are opaque identities, not vendor/product names. */
#pragma once
#include <string.h>
static inline void controller_assignment_resolve(int ports, const char *const *binding,
        int devices, const char *const *identity, const int *previous, int *result) {
    for (int s = 0; s < ports; ++s) result[s] = -1;
    for (int s = 0; s < ports; ++s) {
        if (!strcmp(binding[s], "auto") || !strcmp(binding[s], "none")) continue;
        int match = -1, matches = 0;
        for (int d = 0; d < devices; ++d)
            if (!strcmp(binding[s], identity[d])) { match = d; ++matches; }
        if (matches != 1) continue; /* ambiguous identity must not pick a device */
        int used = 0;
        for (int p = 0; p < s; ++p) used |= result[p] == match;
        if (!used) result[s] = match;
    }
    /* Keep automatic owners where possible; unrelated connects cannot reshuffle. */
    for (int s = 0; s < ports; ++s) {
        if (strcmp(binding[s], "auto")) continue;
        int d = previous[s], used = 0;
        if (d < 0 || d >= devices) continue;
        for (int p = 0; p < ports; ++p) used |= result[p] == d;
        if (!used) result[s] = d;
    }
    for (int s = 0; s < ports; ++s) {
        if (strcmp(binding[s], "auto") || result[s] >= 0) continue;
        for (int d = 0; d < devices; ++d) {
            int used = 0;
            for (int p = 0; p < ports; ++p) used |= result[p] == d;
            if (!used) { result[s] = d; break; }
        }
    }
}

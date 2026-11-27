/* Match a D16 attachment load-clear to a fragment's depth conversion.
 * Metal's load-clear truncates on Apple GPUs, while a fragment write rounds.
 * In particular 0.5 clears to 32767 but a fragment writes 32768. An EQUAL
 * classification pass then exposes its coarse tile boundaries.
 * Pick the nearest D16 value, then a point inside both its truncation and
 * rounding bins. Dividing by 65535 alone can round below the integer when
 * Metal converts the descriptor value to float, so keep a quarter-unit margin.
 * Other depth formats and fragment-based masked clears do not need this. */
#pragma once
#include <math.h>

static inline double nvn_depth16_clear_value(double depth) {
    if (!isfinite(depth) || depth <= 0.0 || depth >= 1.0) return depth;
    double q = round(depth * 65535.0);
    return q >= 65535.0 ? 1.0 : (q + 0.25) / 65535.0;
}

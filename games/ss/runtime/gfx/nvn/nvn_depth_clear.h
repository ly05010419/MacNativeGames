// A Depth16Unorm load-clear value that lands where a fragment writing the same depth lands. Apple GPUs truncate a load-clear but round a fragment's depth: 0.5 clears
// to 32767 and a fragment writes 32768, so an EQUAL test after a clear failed in whole tiles (MacNativeGames 3d90e41: a tile grid in Tears of the Kingdom's Ultrahand
// and Recall highlights). Take the nearest D16 value, then a point inside both its truncation and its rounding bin: dividing by 65535 alone can fall below the integer
// when Metal converts the descriptor's double to float, hence the quarter-unit margin. Other depth formats and the in-pass clears (fragments, clear_ds_tri) need no change
#pragma once
#include <math.h>

static inline double nvn_depth16_clear_value(double depth) {
    if (!isfinite(depth) || depth <= 0.0 || depth >= 1.0) return depth;
    double q = round(depth * 65535.0);
    return q >= 65535.0 ? 1.0 : (q + 0.25) / 65535.0;
}

/* Key 4 / TOTK_ASPECT (runtime/aspect_patch.h) for the UI, in the graphics layer: the game lays its UI out for 16:9, so
   with the picture shown at ratio a it comes out stretched by a / (16/9) however wide the canvas is (a 16:9 canvas
   stretched at the present, or one as wide as the screen, TOTK_OUTPUT_RES=3440x1440). nvn_mtl.m (uw_ui_squeeze) narrows
   UI draws by *ux (> 1 wider than 16:9) or flattens them by *uy (> 1 below it) about the canvas centre. The ratio comes
   from runtime_aspect_now; nothing here depends on the runtime. Tested in tests/test_aspect.c. */
#pragma once

/* Both 1 when a is 0 or within 0.1% of 16:9. */
static inline void totk_ui_squeeze(double a, double *ux, double *uy) {
    double f = a > 0 ? a / (16.0 / 9.0) : 1;
    *ux = f > 1.001 ? f : 1;
    *uy = f < 0.999 ? 1 / f : 1;
}

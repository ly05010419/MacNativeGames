/* Host replacement for Labo's lens mesh. Keep the game's fragment shader
 * (including its FXAA), colour format, viewport and subsequent HUD draws.
 * The Quest compositor applies the headset's lens distortion itself. */
#pragma once
#import <Metal/Metal.h>
#include "vr_labo.h"
id<MTLFunction> vr_labo_quad_vertex(id<MTLDevice> device, int tone);

id<MTLFunction> vr_native_output_fragment(id<MTLDevice> device);

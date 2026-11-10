FXAA 3.11 Quality reference: NVIDIA, Timothy Lottes, 2014. Source consulted:
https://github.com/GameTechDev/CMAA2/blob/071c6b0857559f4e36f614362e6d2aab1b61938a/Projects/CMAA2/FXAA/Fxaa3_11.h

The original NVIDIA sample URL is recorded in that header. The NVIDIA copyright,
redistribution conditions and disclaimer are preserved in LICENSE.txt and copied
into the app by build_app.place_runtime (also used by update_runtime.py).

../d2r_gfx/gfx/nvn/totk_fxaa_shader.h adapts the Quality path, preset 39, to Metal.
It computes perceptual RGB luminance rather than taking luma from alpha or green,
uses subpixel factor .75, relative threshold .125 and absolute threshold .0312,
and preserves the input alpha. Intel's separate-R8-luma modification is not used.

SMAA lookup tables and license, unchanged from iryoku/smaa at
71c806a838bdd7d517df19192a20f0c61b3ca29d (MIT):
https://github.com/iryoku/smaa

AreaTex.h, SearchTex.h and LICENSE.txt were copied from games/odyssey/third_party/smaa
at MacNativeGames e3f7e9d4, with CRLF line endings normalized to LF.
TOTK builds and runs without reading Odyssey's folder.

../d2r_gfx/gfx/nvn/totk_smaa_shader.h ports SMAA.hlsl's spatial presets to Metal;
totk_smaa_config.h supplies the reference Low, Medium, High and Ultra constants.
Its orthogonal search and neighbourhood blend started from Odyssey's
odyssey_gfx_present.inc at e3f7e9d4; diagonal and corner detection follow the
reference SMAA.hlsl above. The adapter is totk_smaa.h. Edge thresholds are fixed
at one quarter of the reference values (400% sensitivity), with the full result
used (100% blend). All four presets use luminance edge detection.
No temporal sampling is used.

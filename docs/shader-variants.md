# Shader variants from the code, not from play

A game shader is translated for the bind state of a draw (`deps`: texture kinds, image formats, vertex input and
fragment output types). Until 2026-10-22 almost every bound texture slot was a dependency, so one shader had up to nine
variants, and the ones a build did not carry had to be met in play and collected in the shared list
([shader-list.md](shader-list.md)). An iPhone or iPad, which cannot translate, skips draws whose variant it lacks.

## What the variants differed in (measured 2026-10-22)

The shared list then held 51,077 variants of 43,651 shaders; 9,917 were not the default bind state. Comparing the MSL of
every pair of variants of one shader in a Mac translation cache (20,039 variants, SSA numbers normalised), no pair
differed in its logic, only in declarations:

| Difference | Variants | What the MSL shows |
|---|---|---|
| Texture dimension (2D / array / 3D / cube) | 3,804 (38 %) | Nothing: the translator reads the dimension from the sampling instruction; only the recorded deps differed (861 identical pairs) |
| Depth-format texture | 3,620 (37 %) | `depth2d…` declaration. 1,248 of 1,316 depth slots are only compare-sampled (shadow maps) |
| Integer vertex input | 2,364 (24 %) | `uint4` input; 834 of 837 are, in the float variant too, only bitcast to integers or used as indices |
| Integer colour output | 130 (1 %) | `uint4` output |

## What the translator does now (`games/totk/tools/totk_xlate/Service.cs`)

- A texture slot whose kind it did not query (Ryujinx queries it only for size queries, 1D / buffer and bindless) is
  recorded as `"*"`, or `"*_depth"` when a depth-format texture is bound there. The MSL is the one the old translator
  wrote for the same answers, byte for byte.
- The reflection lists the slots the code compare-samples (`compare_slots`, shadow maps). The game binds depth
  textures to all of a shader's compare slots or to none (1,248 and 126 Mac-translated variants, never some).
- A vertex input the request does not answer is typed by its use: `uint` when every value loaded from it is only bitcast
  to an integer, converted to one or used as an index. Measured on the variants the game translated with real answers:
  834 of 837 integer inputs found, no float input taken for an integer one. An answered input keeps its answer.

The runtime (`totk_shader_rt.m deps_match`) and the seed (`prepare_shader_seed.py deps_match`) match `"*"` to any
non-depth answer and `"*_depth"` to any depth one. Vertex input types are not compared at all (next section).

## Vertex inputs take their bound format's type (2026-10-25)

On the Switch a vertex shader has no input types: the hardware's vertex fetch converts each attribute by its format
into 32-bit registers (integer bits for a pure integer format such as `R8_UI` or `RGBA16_I`, float bits for every
other one: float, half, normalized, `*_UI2F` / `*_I2F`), and the code reads the registers untyped. Metal declares
each input's type in the function, so the translated MSL had to guess it, and the guess decided the variant: of the
2,069 non-default 1.4.3 variants in the shared list, 1,876 differed from a built-in one only in a vertex input type
(1,873 inputs typed `uint` from their use had float formats bound). Breath of the Wild's list was 88 % such rows.

The pipeline now gives each input the type of the format bound to it (`nvn_mtl.m vs_vin`): an input whose declared
type class differs is redeclared with the format's type and read back as the declared type with `as_type`
(`msl_retype_inputs`, a patched vertex function like the varyings and border patches), so the code sees exactly the
bits the hardware's fetch gives. Any variant serves any vertex formats, and the list rows that differed only there
were pruned (3,675 of Tears of the Kingdom, 3,289 of Breath of the Wild). `make probe-gpu-vin` checks the bits for a
float input with `RGBA8_UI` / `RGBA8_I` bound and a `uint` input with `RGBA32_F` bound (`TOTK_PROBE_NEGATIVE=16`
skips the retyping: Metal then refuses the pipeline). Function constants could not do this: Metal requires every
function with function constants to be specialized before a pipeline takes it, which would add a compile per vertex
shader at load.

## The seed

Tears of the Kingdom's seed now translates every shader the build finds for the default bind state (28,771 in 1.4.3,
up from the 12,472 of the startup packages), and for a shader with compare slots also the variant with all of them
bound to depth textures. A shared-list row is translated only when no variant already made serves its bind state. The
packaging names each warm-up row's variants as the seed serves their bind states (`shader_list.py resolve`, through the
bind states the list keeps in its `A` rows), since rows recorded on a player's Mac name the variant by the bind state
it was made for.

Offline check (2026-10-22): of 2,681 variants the game translated on this Mac, the new translator gives identical MSL
for every one with the same answers, and 2,641 (98.5 %) are served by the default or the depth-bound variant with
identical MSL.

Shaders a translation cannot tell apart are translated once (2026-10-30). The key hashes the whole control section, but
a translation reads only the stage, the code, where its constants start, a compute shader's group size and the early
fragment tests flag (`prepare_shader_seed.py translation_input`; `totk_shader_rt.m variant_request` reads the same).
3,416 of 1.4.3's 28,771 shaders differ from another only in control bytes nothing reads, nearly all at +0x7d0..+0x7df
(3,266 effects, 79 agl, 55 `.bfsha`, 16 layout); in the Ryujinx seed all 3,348 of them with a default variant had MSL
and reflection byte-identical to that shader's. The seed translates the first by key; each other one gets a `.variants`
index naming the first one's variants, through which packaging names warm-up rows recorded with its own variant names
(`shader_list.py resolve`). The runtime loads a variant by the name its row gives, so these shaders share one
translation, one Metal library and their pipelines.

Colour outputs work the same way (2026-10-26): on the Switch the fragment shader writes untyped registers and the
colour unit takes their bits as integers for a pure integer render target and as floats otherwise; Metal refuses a
pipeline whose output type does not match its attachment. `pso_desc` redeclares such an output on whichever fragment
function it took (plain, border, tile-reading, no-depth, MetalFX: each patched function keeps the MSL it was compiled
from), and the code's value is stored with `as_type` (`fn_retype_outputs`, `make probe-gpu-fout`,
`TOTK_PROBE_NEGATIVE=17`). The 61 1.4.3 list rows with an integer colour output (and 8 of the older versions, 3 of
Breath of the Wild) were pruned.

What still needs its own variant: a depth texture read without a compare and a queried texture kind other than 2D.
The shared list keeps naming those.

The translation chain went from `totk-native-v6-unverified` to `totk-native-v7-unverified` with this change: a v6 cache
indexes the default variants under the same names with the old, stricter deps, and the runtime takes a cache's index
rows before the seed's, so v6 caches would keep translating states the seed serves.

Render pipelines (`R` / `B` rows) are not covered: they combine two shaders with render target formats, blending and the
vertex layout, which the game's materials decide, not the shader code. A missing pipeline costs one Metal pipeline build
when first drawn; it never leaves a draw out.

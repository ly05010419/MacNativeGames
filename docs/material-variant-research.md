# Shader coverage from the game's own data

What was done, what it measures, and what it does not prove. Written for the
maintainer of `MacNativeGames` (upstream: <https://github.com/ly05010419/MacNativeGames>).

Every number below was measured on the user's own verified 1.4.3 data on an Apple M4
(macOS 26.6.2, Xcode 27.1, Metal toolchain 27A266a).

## Summary

1. The seed now carries **every shader the game data holds**, not only the startup ones:
   28,687 of 28,771 distinct shaders, 31,790 variants (before: 18,662 and 20,961).
2. The game data also says more than the shared list does: **739 additional vertex
   variants** were derived from the material and mesh metadata, translated and compiled.
3. The remaining gap is **not** reachable from the data: the texture types, image
   formats and output formats a fragment shader needs come from the engine's runtime
   binding state, not from the game files. The shared list stays the mechanism for those.
4. The practical effect is that an effect shader nobody had met yet no longer has to be
   translated while playing.

## Why the coverage was incomplete

`prepare_shader_seed.py` translated the *startup* shaders (Shader/, SDK, boot) for the
default bind state, plus every bind state the shared list names. The game data holds
more than that:

| | count |
|---|---:|
| distinct shaders in the 1.4.3 RomFS and game modules | 28,771 |
| prepared by the old seed | 18,662 |
| never prepared, and absent from the shared list | 10,109 |

The 10,109 are almost all effect shaders: 9,943 in `Effect/`, 82 in `UI/`, 60 in `Lib/`,
24 in `Shader/`. By stage: 6,970 vertex, 3,097 fragment, 42 compute. A player who met
one of them got a translation in the middle of a frame.

The change: `prepare_shader_seed.py` now translates the whole inventory for the default
bind state. `--startup-only` keeps the old behaviour. The measured preparation is
**133 seconds** with 8 workers, 84 failures (0.3%, almost all agl vertex shaders where
the translator's array access goes out of range — a translator limit, not missing data).

All 10,025 newly prepared shaders have a dependency set that uses only default answers,
so the default preparation is the right guess for them. In the whole seed, 17,858 of the
20,961 recorded variants (85%) use only default answers; the other 3,103 need something
specific (1,912 `2darray`, 1,288 `3d`, 1,227 `cube`, 1,058 `2darray_depth`, 731 `uint`
attributes, 66 `2d_depth`, 33 `uint` outputs).

## What the material metadata adds

The shared list records bind states that players met. The game data describes the
materials and meshes, and that is enough to derive some bind states on its own.

`read_material_metadata.py` reads the BFRES v10 metadata bodies (no geometry decode, no
game process, no GPU) and `read_shader_archives.py` reads the BFSHA v9 shader archives:
FMAT shader assignments, compact boolean/string option assignments, sampler and
attribute aliases, render-info tables, FSHP/FVTX skin counts and vertex formats. The
material's own settings and the known render-info and skin values select programs in
the BFSHA key table; unknown pass and system settings stay unconstrained, so the result
is a *candidate* set, not the exact draw the game will select.

The vertex input types then come from the mesh's vertex formats and the BFSHA attribute
locations. The shader code is read from the game's own BNSH and hashed; the cache is
never an input, only a comparison afterwards.

Measured over all 12,395 `Model/*.bfres.mc` files (about 19 seconds):

- 55,778 mesh records, deduplicated to 8,019 metadata contexts.
- 2,421 unique vertex dependency configurations could be inferred under the strict,
  resource-free rule.
- 1,682 were already usable from the production seed.
- **739 additional candidates**, over 730 raw vertex shader keys, were absent from both
  the usable seed and the shared list.

`verify_material_variants.py` reread the game shader bytes, checked each candidate key
and the native tool hashes against the production seed, translated the 739 requests with
the v6 translator and compiled the output with the safe-math Metal compiler: **739/739
translated, 739/739 matched their predicted dependency dictionary, 739/739 remained fully
material-derived and 739/739 compiled successfully.**

A nontrivial example: body shader `d44f4fda4b47509ebfe770607062c99531e29c6e34ea1ecc3577c2ddefb10268`
needs vertex location 6 as `uint` (bone indices), with locations 0/1/2/4/8/9 as `float`.
The all-float default is not equivalent, so the default preparation would not have
covered it.

The 739 rows are in `/Users/mattia/Documents/totk/research/material-derived-variants.tsv`
(identifiers and dependencies only) and can be merged into the shared list like any
other contribution. **They are candidates, not proven draws**: the game may never select
some of the compatible pass combinations.

## What cannot come from the data

This is the negative result that shapes the work, and it is why the method stops at
vertex inputs.

- A slot's texture type is the type of the texture the **engine** binds at draw time.
  A program binds up to 29 fragment texture slots, and a material's own three to five
  textures cover only a few. The rest are engine resources: G-buffer targets, shadow
  cascades, cubemaps, prefiltered maps.
- **1,205 of the 4,700 fragment shaders in the archives have more than one recorded
  dependency set** (up to four), so even a correctly identified pass does not fix a
  fragment shader's texture types.
- The output formats follow the pass only loosely: the whole seed holds 37 distinct
  `fout` sets, and only 33 of 18,662 shaders have more than one. By pass: material 5,
  gbuffer 8, dynamic0 7, dynamic1 8, dynamic2 4, dynamic3 3, zonly 2, cubemap 1,
  depthshadow 1. The engine also writes to G-buffer and engine-provided targets whose
  formats the material files do not describe.
- The `_depth` suffix on a texture type is decided by the NVN format of the bound
  texture at runtime, not by the file name.
- Texture metadata *is* readable where it is stored: BNTX containers carry the image
  dimension, the storage dimension, the format and the size. But of 44,061 files in
  `TexToGo/`, only **216 are BNTX**; the other **29,140 are TXTG**, whose header has no
  dimension field.
- The compute shaders are not in the top-level archives but in the game's own shader
  packages. Their threadgroup sizes are in the shader's control section, which the
  preparer already reads; their texture sets come from the engine, like the fragment
  ones.

A complete offline preparation would need the engine's render-graph state, which is the
game's code rather than its data.

## How the Switch avoids all of this

On the Switch the NVIDIA driver compiles the Maxwell shader when the game binds it, with
the real bound state already known (the texture formats, the attribute formats, the
render targets). The result goes into the driver's cache, so the second use is
immediate. The game prepares nothing; the driver resolves each variant when it is
needed, and there is no first-use stutter because the compilation is the driver's
ordinary work.

On the Mac there is no such compiler: the shader is translated to MSL and then compiled
by Metal. The obstacle is not the translation speed but the missing knowledge of the
bound state. The driver knows it because it is there when the texture is bound; we only
know it if somebody records it.

## Files

Research tooling, outside the build path:

- `games/totk/scripts/read_shader_archives.py` — BFSHA v9 metadata reader.
- `games/totk/scripts/read_material_metadata.py` — BFRES v10 / MCPK metadata reader.
- `games/totk/scripts/find_material_variants.py` — the material scan (the counts above).
- `games/totk/scripts/verify_material_variants.py` — translates and compiles the
  candidates, and writes the `V` rows.
- `tests/test_material_variants.py` — synthetic fixtures, no game data.

The build change:

- `games/totk/scripts/prepare_shader_seed.py` — `plan()` takes `complete`; the whole
  inventory is prepared by default, `--startup-only` restores the old behaviour. The
  seed manifest records `complete`, so a reused seed is not mistaken for a complete one.
- `tests/test_shader_seed.py` — covers the new plan.

Format references consulted (MIT), not vendored: `KillzXGaming/ShaderLibrary`
(`Switch/BfshaLoader.cs`, `Structs.cs`, `Helpers/ShaderOptionSearcher.cs`),
`KillzXGaming/BfresLibrary` (`Switch/Model/MaterialParserV10.cs`, `ModelParser.cs`,
`ShapeParser.cs`, `VertexBufferParser.cs`, `Shared/Models/VertexAttrib.cs`),
`EPD-Libraries/BntxLibrary` (`TextureInfo`, `ImageDimension`, `ImageFormat`),
`UltimateBomb/totk-meshcodec` (MCPK framing).

## Verification notes

- The program reader was checked against reality: **all 23,463 vertex keys and all 23,463
  fragment keys** it computes from the archives are present in the production seed. A
  wrong offset interpretation would produce keys nobody has.
- The coverage numbers use the runtime's own matching rule (`deps_match`, with its
  defaults and its asymmetric dependency sets), not exact JSON equality.
- The seed's files are checked against its manifest; the user's shader cache was
  fingerprinted before and after a read-only comparison (67,351 files, unchanged).
- **151 source tests pass.** The production seed, the shared list, the user cache and
  the profile were never modified by the research; the app was rebuilt with the new
  seed afterwards.

## Reproduce

```sh
# the coverage scan
python3.14 games/totk/scripts/find_material_variants.py \
  --romfs /path/to/TOTK_build/totk/extracted/v143/romfs \
  --seed /path/to/TOTK_build/totk/build/shader-seed/v143 \
  --output /path/outside/source/material-variants.json

# translate and compile the candidates, and write the V rows
python3.14 games/totk/scripts/verify_material_variants.py \
  --report /path/outside/source/material-variants.json \
  --romfs /path/to/TOTK_build/totk/extracted/v143/romfs \
  --seed /path/to/TOTK_build/totk/build/shader-seed/v143 \
  --translator /path/to/TOTK_build/totk/prebuilt/macos-arm64/totk_xlate \
  --spirv-cross /path/to/TOTK_build/totk/build/spirv-cross/spirv-cross \
  --output /new/path/outside/source/verified --metal
```

Both refuse to write inside the source tree, the seed, the RomFS or the user cache.

## What is still open

- The 739 material-derived variants are candidates until someone plays and sees them.
- The effect shaders are prepared, but the *variants* an effect needs with a non-default
  texture type still come from the shared list.
- The translation itself is still synchronous (PR #30 makes the library and pipeline
  compilation asynchronous, not the translation). Making the translation asynchronous
  would move the remaining work out of the frame, at the cost of objects appearing a
  moment late.

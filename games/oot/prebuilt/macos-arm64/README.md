# NativeAOT shader service

`totk_xlate` translates GPU shader instructions to SPIR-V, Metal Shading Language and reflection. It is an ahead-of-time .NET build for macOS arm64; the game needs neither a .NET runtime nor Python. It is not a CPU emulator.

This title owns its complete shader toolchain: `tools/totk_xlate`, patches `0001` through `0023`, the MIT shader-only sources in `third_party/ryujinx`, and the Khronos SPIRV-Cross revision pinned in `tools/shader_pins.py`. No build reads another game's directory. See `third_party/COPIES.md` for provenance.

Rebuild using an external workspace and an installed .NET 10 SDK:

```sh
games/oot/tools/totk_xlate/publish_native.sh /external/oot-shader-build /path/to/dotnet
```

The builder records source fingerprints, the pinned SPIRV-Cross commit and the output SHA-256. `totk_xlate.sha256` is checked before App packaging. NativeAOT output hashes can differ between build locations; compare translated MSL, SPIR-V and reflection for identical requests before replacing a build intended to be equivalent.

The current service adds SDK 22 task/mesh stage memory, tensor shader lowering, typed descriptor pools, packed FP32-to-U8/S8 conversion, global compare-and-swap and shared atomic offsets. Validation includes 192 production-backend GPU conversion cases. All 42 shader variants observed before the previous opening-to-scene crash were freshly replayed with the actual game's depth mode and produced byte-identical MSL to their previous translation. Newly supported instructions change their affected shaders deliberately. Real-game scene validation is tracked separately in the title README.

Surface reduction supports 32-bit integer add, min, max, and, or and xor with byte/pixel addressing and verified weak/CTA/GPU ordering modes. Increment/decrement and other unverified modes fail explicitly. Its native GPU regression checks 864 pixels, including concurrent operations and out-of-range coordinates. `--check-ampere <manifest>` reports unregistered opcodes without translating or executing shaders; successful coverage does not validate operand modes or a game scene.

Chain `oot-native-v10-pool-map-live-inputs` adds dynamic raw-pool table capacity and preserves live inputs of multi-result producers during dead-code elimination. Independent GPU negative controls expose both previous failures. The fixed translator passes 14 raw-pool cases and 640 multi-result input/output checks. Replay of 468 actual requests produced 393 identical and 75 changed MSL outputs; all changed outputs passed SPIR-V validation and Metal compilation, with one restored read-only storage-buffer reflection reviewed separately. The binary shipped here matches the recorded NativeAOT build used for those checks.


Chain `oot-native-v13-raw-read-range-cache` includes the optional exact per-invocation raw-read mapping cache in patch `0023`. Reflection declares `raw_read_range_cache_abi: 1` so the host may use the opt-in header bit; the host never sends it to legacy reflection. The formal NativeAOT build reproduces the GPU-validated prototype's translated outputs for the 468 recorded requests. A further 966 variants were accepted only after the old service reproduced their recorded outputs exactly; 19 unreconstructable variants retain their old code. Production-backend mixed-ABI tests pass 420 cases / 1,050 checks. The title README describes the limited real-game performance evidence and the old-code control supporting its enabled default.

Chain `oot-native-v21-predicated-deferred-writes` (patch `0031`) makes a predicated variable-latency write land only when its guard was true at issue. A later predicated write of the same register no longer discards an earlier pending write; the earlier one instead yields when the later one executes. `tests/ampere_predicated_writes_gpu.py` runs 48 production-backend cases: the previous translator returns the register's stale value on the first case, and this build passes all 48. The existing scoreboard regression passes 48 cases and the 17 unit tests pass. Static scan of 843 cached shaders: 35 contain this pattern. Real-game check: the Kokiri forest tree mesh shader `604134a5`, whose twigs read a stale texture layer, renders correctly (user-confirmed on the Mac, 2026-11-09). The bundled shaders must be rebuilt for this chain; a build that only swaps the translator recompiles during play.

Chain `oot-native-v22-one-translation-per-2d-slot` stops making separate translations for depth-format textures. A slot keeps its plain `texture2d` declaration whatever is bound; the runtime reads depth textures through a `(R, 0, 0, 1)` view and a point sampler (`games/oot/tests/depth_unified_gpu.m`). A queried slot answers `*2d` when translating it again with the other of 2D / 2D array gives the same MSL.

Chain `oot-native-v23-txd-derivative-order` (patch `0032`) reorders Ampere TXD's interleaved derivatives (ddx.u, ddy.u, ddx.v, ddy.v) into the dPdx, dPdy the texture operation takes; `games/oot/tests/ampere_txd_gradient_order.py` checks the translated order.

Chain `oot-native-v24-depth-register` (patch `0033`) reads a pixel shader's depth from R(4 x enabled colour targets + 1), as the colours are read four registers per enabled target; counting enabled components read R6 instead of R9 for OmapTargets 0x10f, and Ocarina of Time's half-resolution depth downsample wrote the constant 0. `games/oot/tests/ampere_depth_register.py` checks the register.

Chain `oot-native-v25-z-snap-early-tests-gather-offsets` takes three fixes from seedds/nxnative (MIT, commit 0cae4830): `FragZSnap.cs` (a pixel shader's `gl_FragCoord.z` compared with a texture value takes that value when it lies within one ulp and a fraction of the pixel's depth step under z: Apple's GPU stores a depth up to that much under the z the fragment reads, NVIDIA stores exactly it; Ocarina of Time's ground mist lost scattered pixels of its own surface), `GatherOffsets.cs` (`textureGatherOffsets` with its four offsets written out, one gather for a 2x2 square, instead of spirv-cross's loop over an offset Metal takes only as a constant), and the Switch 2 early fragment tests flag (control byte +0x5b, `totk_shader_rt.m`: no NVN2 pixel shader got `[[early_fragment_tests]]` before). The Metal libraries are also compiled with `-fwrapv` now (`scripts/build_shader_libs.py`). Checks: `games/oot/tests/frag_z_snap.py`, `frag_z_snap_gpu.m`, `gather_offsets.py`, `gather_offsets_gpu.m`, `ampere_early_fragment_tests.py`.

Chain `oot-native-v28-mesh-primitive-output-blocks` (PR #2166) reads complete vertex blocks before primitive blocks.
The checked-in tools were rebuilt from the PR's sources using the pinned .NET SDK 10.0.401 and the shader-service builder.
970 reconstructed Ocarina requests (500 mesh, 20 task, 150 each vertex/fragment/compute) produced 3,880 byte-identical
MSL, SPIR-V and reflection files compared with the contributor's build. The proof library passed 36,872 independent
comparisons and returned 144 byte-identical replies on matching inputs. `tests/test_oot_mesh_output_layout.py` checks
both depth exports against independent block contents for 128/64, 32/32, 128/64 without primitive outputs, and 64/128.
These are translator checks; they do not replace a Forest Temple gameplay check.

## Licences of what is compiled in

`totk_xlate` contains, besides this repository's own MIT code and Ryujinx's shader recompiler (MIT; see `THIRD_PARTY_NOTICES.md` at the repository root):

- SPIRV-Cross (Apache-2.0): `SPIRV-Cross.LICENSE` in this folder.
- The .NET runtime of the NativeAOT build (MIT): `dotnet-runtime.LICENSE` in this folder.

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

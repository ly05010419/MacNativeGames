# ITT compute-memory translator candidate

Game-local copy of `games/totk/tools/totk_xlate` at **947d4c0**. This is the
source snapshot whose packaged SHA-256 matches ITT's inherited service
(`6dd53d6486c96e3a95ae5fd49fdc35f75f6e603ff8ededa3d7250fd2e4fe1220`).
Do not replace it with current TOTK/MCD2 sources: those include unrelated vertex,
texture/depth and descriptor-array changes. Vendor Ryujinx is unchanged between
that snapshot and the current repository; only the four original patches apply.

The compute-memory translation change is explicit, nonnegative, 4-byte
aligned bytes from guest NVN control +0x754, forwarded unchanged to the accessor
and published as `deps.shared`. Runtime dependencies and request hashing include
this field and reject legacy translations or replies that omit it. Zero is valid.
No array limits are increased and no cached MSL/reflection is edited.

Build (requires .NET 10 SDK):

```
tools/totk_xlate/publish_native.sh /path/to/dotnet /path/to/ITT_build
```

Output is `<work>/build/shader-service-candidate`; generated sources and NuGet
state remain in `<work>/src`. The script does not overwrite the packaged prebuilt,
active service, or formal App, and does not change the GameBuilder pipeline.
The repository prebuilt now matches the metadata-aware service used in the
validated test sessions. Its digest is recorded in `prebuilt/macos-arm64`.
Always package runtime and service together. This repository update does not
replace an installed formal App. The raw runtime keeps depth-sampling continuation opt-in with
`ITT_DEPTH_SAMPLE_CONTINUE=1`. The user-approved formal launcher now supplies this
default (and `TOTK_ASYNC_SHADERS=0`), while honoring explicit environment overrides. User reports improved stability in the opt-in session,
not a measured guarantee of sustained 120 FPS.

Validation: 16 identical-request vertex/fragment translations match the old
service byte-for-byte for SPIR-V/MSL and as parsed reflection JSON. 24 actual
compute shaders with captured guest control bytes retranslate successfully with
exact allocations and compile under Metal safe math. Invalid/missing shared
metadata is rejected. These checks are not proof of visual correctness or dynamic
shared-array bounds. See `docs/startup-performance-investigation.md`.

## Dynamic texture-offset correction (2026-11-01, packaged on user request)

ITT's accessor now returns false for `QueryHostSupportsNonConstantTextureOffset`.
The inherited default was true, but this SPIR-V backend emits only
`ConstOffset` / `ConstOffsets`: runtime loads became illegal constituents of
`OpConstantComposite`, causing SPIRV-Cross `nullptr` exceptions. The existing
Ryujinx `TexturePass` now legalizes these offsets before codegen. Integer texel
fetches add signed offsets to spatial coordinates, preserving array layer,
LOD and multisample index; constant offsets remain unchanged. No shared vendor
code or AST patch is changed, so the existing `.patched` staging tree is valid;
`publish_native.sh` always recopies the changed game-local `Program.cs`.

Offline evidence in `<work>/analysis/user-regression-20261101`: all 118 failed
variants (28 shader hashes) fail on the old service and translate on the
candidate, then compile with `macos-metal2.3 -fno-fast-math`. Failed request
answers were not persisted: replay reconstructs texture dimensions from resource
reflection, not exact original depth/attribute/c1 answers. 32 successful graphics
control cases produce byte-identical SPIR-V/MSL/reflection on both services.
Synthetic production-IR tests cover constant/dynamic 1D/2D/3D offsets, arrays,
LOD and 2D multisample fetches, with an old-capability negative control.
This is not visual/FPS acceptance or a complete sample/gather semantics test.

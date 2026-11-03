# Age of Calamity: repository analysis and port decision

Analysis date: 2026-10-27. Repository examined: `8c9c71b9503f9b7d9643a4f45aa8fbcb1aff1f02`.

**Decision: build the macOS port from `games/jamboree`'s isolated nxnative runtime and Mac packaging approach. Restore and validate the Age of Calamity profile from the exact nxnative revision that Jamboree imported.** Use Bayonetta's staged bring-up method and the other ports' verification and shader-preparation lessons.

This is the analysis written before the port was made (kept as its record); the port itself and what it does now are in [README.md](README.md) and [VALIDATION.md](VALIDATION.md).

## Scope and strength of evidence

The review covered all 13 existing game directories, root build/update tooling, contributor rules, tests and CI, shared technical documentation, per-game status/work logs, and representative implementation paths for extraction, rewriting, loading, services, audio, shaders, resources, presentation and packaging. File hashes were compared to identify actual fork divergence. This is an architectural and implementation audit, not a claim to have proved every line or played every game.

Three kinds of evidence are kept separate:

- **Inspected locally:** code and metadata present at the repository commit above.
- **Reported historically:** gameplay and performance observations in the repository's documents. Their private logs, dumps and saves were not available for independent replay in this analysis.
- **Newly checked:** the source tests and synthetic Metal probes listed near the end of this report. These contain no original game data and do not establish Age of Calamity gameplay compatibility.

No target game dump, update, keys, firmware or save location was provided. No private target assets were searched for outside the repository. Target-specific upstream source was checked online at a fixed revision; no upstream code was imported into this checkout during the analysis.

## The repository has two runtime families

Most folders descend from the d2r-mobile-derived TOTK host. They rewrite incompatible ARM64 instructions ahead of time, load and relocate the original modules, answer Horizon services, and record/replay NVN operations on Metal. This avoids CPU emulation and preserves most original game execution.

Jamboree independently imports the sibling **nxnative** implementation. It has the same broad native-execution model but different host interfaces, rewriter output, profile system and shader pipeline. Its build stages a private `dm/` source tree, then generates a host for one title profile. It already carries explicit Age of Calamity mechanisms. This is the decisive distinction; the game name on the folder is less useful than the capabilities of its runtime.

The TOTK-derived copies are not interchangeable. Hash comparison found different `loader.c`, `threads.c` and `nvn_mtl.m` implementations across most ports; Wonder and Mario vs. Donkey Kong share several identical core files. A fix in TOTK is not automatically present in Bayonetta, Odyssey, or another copied runtime.

The root [build_app.py](../../build_app.py) and [update_runtime.py](../../update_runtime.py) are specifically wired to TOTK/BotW identities, data layouts and manifests. They are not general new-game builders. The [root tests](../../tests) predominantly cover that family. Jamboree's tests must be run separately.

The [contributor rules](../../CONTRIBUTING.md) require an independent game directory containing everything it reads at build and runtime, recorded provenance, original-input and patch checks, minimal game changes, correct pictures before performance, and no committed game content. Copy dependencies explicitly into `games/aoc`; do not build through another game's directory. The documents' references to removed README history sections or absent AGENTS.md files are historical context, not additional instructions present in this checkout.

## What each game contributes

| Existing directory | Approach and documented scope | Lesson for Age of Calamity; reason it is or is not the base |
|---|---|---|
| [totk](../totk) | Primary mature native host; version-gated loader/patches; verified extraction, offline shader seeds, binding variants, pipeline archives, extensive CPU/GPU diagnostics, app lifecycle, Mac and mobile builds. | Best reference for integrity, cold-start work, resource correctness and measurement. Too much Zelda-specific camera, frame, actor, resolution and shader behavior to inherit indiscriminately. |
| [botw](../botw) | Uses TOTK's runtime rather than a separate host. Its own manifests, extraction/shader scripts and old/new SDK support live here. | A real example of sharing a host when the engine and platform assumptions align. SDK age, RELR/GNU hash handling, atomics and frame-rate details still need per-build validation. Zelda branding does not establish an engine match with KTGL. See [1.9.0 notes](../../docs/botw-v1.9.0-port.md). |
| [by3](../by3/CHANGELOG.md) | Deliberately stripped TOTK fork; sparse-base/BKTR importer; fail-on-unknown services; staged progress from loader probe to `nnMain`, first present and play. Later corrected GPU-written indirect counts. | Best bring-up narrative. Remove predecessor addresses and features first, then fix each platform gap from evidence. Early CPU indirect-count reads produced missing geometry; changing depth/LOD was a false lead. Its older runtime lacks the strongest existing KTGL path. |
| [dread](../dread/CHANGELOG.md) | Bayonetta fork; large BSS and stacks in module data; texture-mode movies; scheduling fixes; direct region texture writes; explicit autorelease lifetime management. Title/prologue/first-area play is documented. | Test full movie completion and scene transitions, not just skip paths. Correct scheduling and end-of-stream behavior fixed use-after-free symptoms. Partial uploads and autorelease pools removed large load costs and memory growth. Mercury-specific fixes are references, not a KTGL foundation. |
| [eow](../eow/CHANGELOG.md) | Bayonetta fork with four modules and recent SDK; NCA compression atop update layers; RomFS C stdio, lower-left NVN origin, sRGB presentation, texture-mode movies, audio fixes, offline libraries. Prologue play is documented. | Diagnose API semantics: an upright logo does not prove render-to-texture orientation; a correct presented texture does not prove window gamma. Distinguish verified compressed storage from decompressed output. Recent dirty-tracker fixes have limited causal evidence. No stronger target-specific base than Jamboree. |
| [jamboree](../jamboree/README.md) | Pinned nxnative import, profile-generated host, whole-game shader extraction/translation, self-contained signed Mac app, external writable state, synthetic geometry tests. Local observations cover title, character selection and plaza. | **Selected base.** Includes KTGL/Age of Calamity shader extraction, renderer, audio and rewriter adaptations. It also exposes clear work to do: replace title gates/profile, remove Jamboree-specific behavior, compile real Metal libraries offline, and validate the target. Its experimental game status is not evidence that its shared runtime is unsuitable. |
| [mvdk](../mvdk/README.md) | Fork of Wonder because the engine/resource approach matches. Adds H.264/AAC movie handling and two-player input. First-level play and co-op are reported. | Choose by engine/runtime compatibility. Exact audio output-buffer sizes matter. Local multiplayer must exercise physical devices, reconnect and independent player state. Lacks a startup shader set; do not pick it merely for a short build script. |
| [odyssey](../odyssey/README.md) | Bayonetta lineage plus many explicit backports; inventory-driven offline shader seed, title warm-up, pipeline caching, scoped warm-up, latency studies and game-specific graphics options. Several kingdoms are documented. | Richest record of cold-cache vs warm-cache mistakes and measurement confounds. Fix shader identity before optimizing cache hits. Keep pipeline/library warm-up bounded. The documented session limit and historical GPU panics make it a poor wholesale base. Its camera/TAA/frame-generation hooks do not transfer to KTGL. |
| [ori](../ori/README.md) | TOTK through an external Luigi's Mansion fork; Unity/IL2CPP; GC thread suspension/context capture; extra x18/jump-table forms; code-data unwind support; shader-control ownership and compute shared-memory fixes. Early play on multiple Macs is reported. | Managed-engine requirements can force real host changes. The program's control metadata must survive caller deallocation; compute shared-memory size affects pixels. Rebuild-on-play shader collection is helpful but weaker than extracting the complete target shader set. No engine match here. |
| [ssbu](../ssbu/CHANGELOG.md) | TOTK through a separate external Odyssey fork; pre-rewritten NROs, lazy binding and unload/rebind, ARC shader extraction, offline shaders, typed SDK override audit, DLC and long-session logs. Long adventure/versus sessions are reported. | Best service-ABI lesson: bool is not `nn::Result`; false success can hang game logic. Strong diagnostic and dynamic-module references. Its failed-shader draw skipping preserves process survival but is not acceptable rendering-completeness evidence. Avoid inheriting Smash-specific resource defaults. |
| [unicorn](../unicorn/VALIDATION.md) | Bayonetta fork, exact regional/version pins, optional verified DLC, weak-symbol allocator precedence, multistream Opus, independent offline build and save/restart validation. | Best reproducibility and save-validation example. Reuse concepts for multichannel audio checks and owned-DLC enumeration. Its GPU-address alignment fix prevented pool exhaustion without enlarging game pools. Audio ABI differs from Age of Calamity's class-based interface. |
| [wonder](../wonder/README.md) | Recent direct TOTK fork, old/new SDK handling, offline services, bindless uniform handles, fixed VI event cadence, guarded real-time frame patch. First-level play is documented. | Platform display refresh and game simulation must be distinguished. Its same-engine descendants are economical; its frame patch is not a general 60/120 fps solution. No startup shader set, and no KTGL-specific advantage. |
| [acnh](../acnh/README.md) | Separate prepare/preflight/host/package stages, input identities, relocation audit, self-contained signed bundle, collected-shader precompilation and save backups. Two asserted main-code patches serialize a graphics-context list and guard UI behavior. | Useful build-stage separation and dependency inventory. The checked-in approach is not patch-free. Those patches are evidence of that game's workaround needs, not candidates for Age of Calamity. Its custom host and observed shader set offer less direct target coverage. |

Several README snapshots lag their own work logs. For example, Bayonetta's README still describes early stages as next steps after its changelog records gameplay. Prioritize current source, dated checks and explicit limitations over a headline status. References to external games such as Luigi's Mansion or Donkey Kong Country Returns do not mean their directories exist here.

## Why Jamboree wins on implementation evidence

The imported revision is **nxnative `7861e7c1a9913e020600b1cc7f65132427e9cc50`**, documented in [COPIES.md](../jamboree/third_party/COPIES.md). The following code is already present locally, independently of fetching the missing target profile:

| Target requirement | Local evidence | What it avoids rediscovering |
|---|---|---|
| KTGL shader containers | [shader_split.py](../jamboree/runtime/gfx/tools/shader_split.py), `idrk_entries`, `idrk_data`, `idrk_shaders`, `glslc_shaders` | RDB/IDRK chunked-zlib scanning, `_S2G3000` materials, `_DOK` system shaders, embedded main shaders, short controls and headerless compute. A Nintendo BNSH-only scanner misses this content. |
| Integer shader inputs with mixed mesh formats | [dmsh_vin.py](../jamboree/runtime/gfx/tools/dmsh_vin.py), `nvn_mtl.m` shadow-input and conversion paths | KTGL soldiers mix Half2, Short2 and integer attributes. GPU shadow inputs preserve Maxwell fetch bits without repeatedly converting entire mesh pools on the CPU. |
| Lighting and decal texture buffers | [nvn_mtl.m](../jamboree/runtime/gfx/nvn/nvn_mtl.m), `tbuf_as_2d`; [translator modification record](../jamboree/third_party/licenses/ryujinx-build.md) | 1D integer fetches can actually address a large texture buffer. A dummy 2D texture makes light/decal lists read zero. Translator and host support must remain paired. |
| Frequent texture registrations | `nvn_mtl.m`, `vc_get`, `vc_put`, `vc_drop_pool` | A texture-serial/view cache serves KTGL's repeated ring-ID registrations and retires resources with their texture/pool. ID-only caching creates excessive views. |
| GPU-driven effects | [hle_nvn.c](../jamboree/runtime/gfx/nvn/hle_nvn.c), indirect records; `nvn_mtl.m`, indirect conversion/masking | GPU-written arguments, quads and index conversions are handled at GPU execution time where supported. CPU readback fallbacks remain visible limitations to measure. |
| Many compute dispatches per battle frame | `nvn_mtl.m`, `dispatch_enc`, `dispatch_own` | Small dispatches share a compute/copy encoder with ordering barriers. The comments document a target workload that regressed with one fully drained encoder per dispatch. Per-dispatch GPU timing itself can restore the slow path. |
| Large shader constants | [nvn_refl.h](../jamboree/runtime/gfx/nvn/nvn_refl.h), dynamically allocated `c1`; `c1_guard.py` and `cb_extent.py` | Age of Calamity soldier shaders exceed the earlier fixed constant-storage size. Proof tables also reduce unnecessary uniform copies. |
| Old SDK movie/audio services | [hle_codec.c](../jamboree/runtime/host/hle_codec.c), class-based multistream decoder; [hle_audren.c](../jamboree/runtime/host/hle_audren.c) | The host routes the hardware decoder interface to the game's software codec. It runs the SDK's own CPU audio renderer instead of the TOTK family's custom mixer. |
| Rewriter edge cases | [patch_nso.py](../jamboree/runtime/tools/patch_nso.py), x30/lr jump-table handling and registered frame steps | Source comments identify an Age of Calamity jump-table pattern already understood by this rewriter. |
| Screenshot copyright overlay and fonts | [hle_oe.c](../jamboree/runtime/host/hle_oe.c), [hle_sdk16.c](../jamboree/runtime/host/hle_sdk16.c) | Additional SDK 9-era behavior has already been encountered on the target. |

These are concrete semantic overlaps. A game about Link is not necessarily implemented like TOTK or BotW; an action game is not necessarily implemented like Bayonetta. Starting from either would require reconstructing several paths already present here.

This choice also preserves Jamboree's generic fix for vertex storage writes with rasterizer discard and no transform feedback. Its [rendering-fix note](../jamboree/docs/rendering-fix.md) explains why suppressing extreme triangles would have hidden the missing producer. Keep that platform correction even if the target's need for it is not yet established.

## Target facts and the missing profile

At the matching pinned revision, the [upstream Age of Calamity profile](https://github.com/seedds/nxnative/blob/7861e7c1a9913e020600b1cc7f65132427e9cc50/dm/games/01002B00111A2000/game.tsv) identifies title `01002B00111A2000`, update **1.3.0 / v327680**, KTGL on NVN, SDK **9.6.0**, and modules `main subsdk0 sdk`. Its registered build-ID prefixes are:

```text
main     c3cf52bf2b05d7310884971b50bcccb8
subsdk0  3ab92c3444d4ece2f52417b5d8ee86eb
sdk      95832210b36121e88b4f9b7b66239dad
```

It supplies `mem_used=0x4800000`, `tex_flush_upload=on`, `vconv_range=on`, full controller layout, and frame-step configuration. Treat all of these as starting evidence; compare identity and memory use with the local original input before accepting them. Its comments explicitly retain some inherited, unmeasured memory choices. These identifiers are not full SHA-256 input pins or proof of dump integrity.

The [matching upstream patches](https://github.com/seedds/nxnative/blob/7861e7c1a9913e020600b1cc7f65132427e9cc50/dm/games/01002B00111A2000/patches.py) include two original-byte-asserted audio-heap reallocations and a build-gated frame-step implementation. The audio change transfers 4 MiB between two sound subheaps while preserving their total. The documented underlying hardware/work-buffer discrepancy is unresolved. Record it as a compatibility workaround and investigate; do not present it as a proven generic fix. Frame stepping coordinates the task clock, battle step and device/event rate. Merely changing presentation interval or one wait can double game speed. Import code with its MIT provenance and verify every original instruction locally.

The pinned [development notes](https://github.com/seedds/nxnative/blob/7861e7c1a9913e020600b1cc7f65132427e9cc50/docs/development.md) are an additional first-party development reference. Their aggregate game status is historical evidence, not local macOS acceptance.

There is **no** `runtime/games/01002B00111A2000` in the current Jamboree folder. Only Jamboree's game profile was retained. Its wrapper and vendored builder also explicitly reject other titles/versions. Changing `DM_GAME` alone cannot make this checkout build the target.

### An important profile trap: system-resource memory

The pinned target profile describes no NPDM system resource but omits an explicit `sys_resource` row. The local [profile generator](../jamboree/runtime/games/game.py) defaults an absent row to `0x2000000`; [hle_svc.c](../jamboree/runtime/host/hle_svc.c) reports that value to the SDK. That can change whether the SDK selects virtual-address allocation or `SetHeapSize`.

Read `main.npdm` and set `sys_resource` explicitly from it. If the target dump confirms zero, use `sys_resource=0`. Do not inherit Jamboree's `0x1000000` or silently rely on the missing-key default. Likewise, the used-memory figure must cover the actual loaded modules. Increasing the whole application pool can conceal incorrect accounting and is not the first fix.

## What must change in a Jamboree-derived target

1. **Isolate the copy.** Copy required host, graphics, tools, source dependencies, binary translator, licenses and Mac wrapper into `games/aoc`. Record the repository commit, nxnative revision, copied paths and adaptations. Copy source, not private generated outputs. Do not import a newer moving upstream runtime without an explicit reason and a fresh validation plan.
2. **Replace the game profile and all build gates.** Remove the Jamboree profile, main patch and movie-continuation build exception from the target copy. Replace title/version checks in both the outer wrapper and `third_party/nxnative/build_ipa_from_nsp.py`, expected manifest paths, package version paths, app names, bundle ID, save/log folders and tests. Retain generic rendering fixes and build-ID-gated SDK infrastructure only where appropriate.
3. **Restore target metadata carefully.** Start with the pinned target `game.tsv` and `patches.py`. Regenerate map/facts/pairs from the user's original files using this fork's four-column public-map format. Do not blindly import upstream path-bearing shader maps or Jamboree's shader/binding/pipeline lists. `c1_full.tsv` and `cb_extent.tsv` are private generated outputs, stamped against this title's chain and profile inputs.
4. **Pin local originals.** Validate container title/content metadata and base/update composition, complete integrity checks, NSO build IDs, unmodified ExeFS hashes and resulting data identity. Produce an `expected-1.3.0.json` from verified inputs; no fabricated or donor hashes. Cache reuse must depend on actual inputs, rules, options and tools. Keep unsupported versions rejected before rewriting or packaging.
5. **Separate baseline and optional frame behavior.** Start with the original 30 fps simulation setting (`fps_target=0`), then validate the coordinated 60 fps path as an optional feature. The upstream frame stubs also affect device configuration in the off path; inspect that emitted code rather than assuming a zero setting removes all stubs. Compare movement, battle/event timing, animation, cutscenes and audio against wall time. There is no basis yet for a 120 fps promise or target frame generation.
6. **Build real offline Metal libraries.** Fix both build-time and launch-time shader mode, as described below. Package only validated matching chain/profile artifacts.
7. **Validate the complete Mac flow.** Native loader and `nnMain`, first meaningful image, new game, movie natural end and skip, active battle, menus, audio/input, save, orderly exit and fresh-process reload. Re-run the signed app from its final path and verify its signature after play.

The independent entry point should be `games/aoc/scripts/build_aoc.py`. Keep the root builder unchanged unless a separately justified integration is requested. Do not make a documentation-only folder look like an already supported port by adding a success row to the root README.

## Shader work is the highest-value performance transfer

There are distinct stages: extract/identify Maxwell shaders; translate to SPIR-V/MSL and reflection; compile MSL into Metal libraries; create GPU pipelines for actual render state; warm selected pipelines. Completing one stage does not prove the later stages are cached or cheap.

Jamboree translates shader source offline but deliberately defaults to **runtime Metal compilation**. Its wrapper's `environment()` and app launcher both set `DM_METAL_RUNTIME=1`. In [dmsh_build.py](../jamboree/runtime/gfx/tools/dmsh_build.py), that mode writes the literal `RUNTIME_METAL` placeholder to files named `.metallib`. In `nvn_mtl.m`, the same setting compiles adjacent `.metal` source before attempting to load library data. A successful package with those filenames is not proof of offline compiled libraries. Jamboree's [validation record](../jamboree/docs/validation.md) explicitly records substantial first-visit stalls.

For the target, compile with the real Metal toolchain and remove the launch-time force as well. Keep offline/runtime math and invariance settings consistent, fingerprint the chosen options, invalidate placeholder-bearing cached outputs, and test rejection of placeholders. Changing only the builder or only the launcher leaves the problem partly intact. Xcode's Metal compiler is available on the analysis Mac; source translation prerequisites still need setup.

Also inspect [own_compilers.py](../jamboree/runtime/gfx/tools/own_compilers.py): it intentionally tolerates local compiler differences without changing the shared chain. Its comments note that an existing per-shader cache can retain libraries from an earlier Metal compiler. Record actual compiler identity in the target's stage/cache fingerprints and verify a clean offline rebuild; a matching shared chain alone does not establish identical compile provenance.

Then apply the other ports' lessons within this runtime's APIs:

- **Inventory the correct formats and identity.** Scan RDB IDRK entries, G2S, `_DOK`, main and supplied content. Support the target's actual control lengths and headerless compute. Compare runtime-used shader keys against extracted keys. Odyssey's earlier BotW-style control rewrite made an apparently complete inventory miss the runtime shaders.
- **Generate real variants and matching reflection.** Binding, integer attributes, texture type, compute local/shared memory and vertex/fragment linkage affect execution. A shader count alone is not coverage. Preserve shadow-input libraries and texture-buffer support from the same translator chain.
- **Compile ahead of first draw, without loading everything forever.** Ship derived libraries privately in the app, prewarm a measured title/battle list, and load the remaining libraries on demand. Whole-game eager object retention can cost startup time and memory even after translation is offline.
- **Treat pipeline caching separately.** The TOTK binary-archive mechanism is useful design evidence, not a header to drop into this sibling backend. First measure Jamboree's existing prediction/prewarm behavior and actual PSO misses, then adapt only necessary mechanisms with tests.
- **Keep failures visible.** Report invalid records, untranslated shaders, library failures, PSO failures, missing dependencies and dropped draws separately. A surviving process, high FPS, or black scene does not pass. Do not copy Smash's failure-tolerant draw skipping and then call incomplete effects correct.
- **Do not carry opaque old cache fixes.** Bayonetta's log still identifies a hand-edited cached shader in historical testing. The target must regenerate every accepted shader from the pinned translator and the user's original bytes. No private cache edits as implementation.

No fixed shader-count threshold from TOTK, Jamboree or historical upstream notes should be used to accept this game's build. Establish counts from this verified title/version and explain deduplication, empty slots, failed translation and reached-shader coverage.

## Correctness and efficiency lessons to preserve

**Fix producers and ordering first.** Jamboree's geometry and Bayonetta's indirect-count defects demonstrate that a wrong picture often comes from missing upstream work. Validate GPU-written counts/arguments when executed, texture flushes, per-stage program bindings, residency, alias visibility, attachment feedback and resource completion lifetimes. Do not hide missing work by removing geometry, relaxing depth, forcing LOD or substituting zero textures.

**Do not transplant an audio implementation across ABIs.** Unicorn's C-interface multistream decoder and the target's class-based interface are different. Jamboree already provides the relevant target path and uses the game's CPU renderer. EoW/Odyssey's end-of-stream and depop findings are useful behavioral test cases; copying their custom mixer into this host would create a second architecture unnecessarily. Validate actual multi-channel music, transitions, clips and underruns, not just nonzero PCM.

**Model scheduling and memory honestly.** Dread demonstrated that native host threads do not automatically reproduce guest wake/preemption behavior. Ori demonstrated suspension/context and exception-unwind requirements. Add only target-proven corrections with focused tests. Distinguish reserved virtual address space from committed pages, guest-used memory, Metal allocations, residency and process footprint.

**Keep game clocks separate from display clocks.** Wonder and Smash exposed high-refresh speed errors; Odyssey needed its own simulation and latency study. Use the target's coordinated frame-step logic, original pacing baseline and real elapsed-time tests. Upscaling, internal render resolution, presentation resolution and generated frames must be reported separately.

**Make every repeated build incremental and verifiable.** Keep separate stage fingerprints for extraction, rewrite, shader split, translation/compile and host/package. Use a temporary app and atomic publication after signature/integrity checks. Preserve user saves and writable caches outside signed resources; a rebuild must not overwrite a live game or profile. Carry the single-game-instance discipline into testing.

**Measure the target.** Preserve Jamboree's view reuse, GPU vertex fetch, bounded conversion, encoder sharing and synchronization. They are especially relevant here. Further optimizations must follow Age of Calamity measurements, not another title's FPS. Heavy diagnostics can change the measured mechanism: `DM_NVN_PASSGPU` makes dispatches use individual encoders. Disable it during production timing and annotate diagnostic windows.

The [performance methodology](../../docs/performance-followups.md), [cache/wait accounting](../../docs/performance-caches-and-waits.md), [Metal diagnostics](../../docs/metal-performance-diagnostics.md), [Odyssey techniques](../odyssey/TECHNIQUES.md) and [Unicorn validation](../unicorn/VALIDATION.md) provide the right discipline: fixed source/binary and route, isolated save/cache copies, cold and warm runs, balanced A/B windows, full frame-time distributions, output cadence, footprint and thermal state. Never benchmark while rebuilding that binary, assume an idle scene represents a battle, or add overlapping CPU/GPU timing buckets together.

## Proposed acceptance sequence

| Gate | Evidence required before claiming success |
|---|---|
| A. Inputs and source | Owned base/update paths; secure local key-file handling; confirmed target version/modules/NPDM; source provenance; no cross-game build reads; tool preflight; original hashes and integrity verified. |
| B. Native execution | Deterministic rewrite from originals; declared/asserted per-build patches; generated profile; correct relocations/imports, TLS, constructors and original `nnMain`; no CPU interpreter/JIT/runtime CPU translation. Use this rewriter's own output/tests, not incompatible d2r metadata tools. |
| C. Meaningful picture | Correct movie and title/UI, fonts and sRGB/origin; first battle with visible soldiers, skinning, lighting, decals and GPU-driven effects; shader/resource errors classified. A clear-only frame is not enough. |
| D. Usable game | Keyboard plus one physical controller; attacks, movement, pause and scene transition; audible continuous game/movie sound; natural movie completion and skip; save and new-process reload using a disposable profile. |
| E. Packaged result | Self-contained arm64 macOS app, no build-tool runtime dependency, strict signatures before/after play, clean output path, caches/saves outside signed resources, rollback/preservation of previous app and data. |
| F. Performance and stability | Separate cold/warm startup and first battle; complete frame distributions and present cadence; CPU submit/encode and GPU costs, shader/PSO activity, memory/thermal timeline; repeatable route and longer play beyond a static scene. 60 fps only after its simulation-timing checks pass. |

DLC is optional and should be a later independent gate. Report only installed and verified owned content; test its enumeration, mounting and game-visible behavior separately. Split-screen co-op is also a separate workload, not implied by seeing two controllers. Mobile builds, arbitrary mods, 120 fps, aspect/render-resolution patches and frame generation are outside the first Mac milestone.

## Checks performed during this analysis

Environment: Apple M5, arm64, macOS 27.0.1; Xcode selected at `/Applications/Xcode.app/Contents/Developer`, macOS SDK 27.0. The Metal compiler reports `32023.921`, matching Jamboree's recorded compiler version. Only .NET SDK 7.0.201 and .NET runtime 7.0.3 were listed; Jamboree requires .NET 10. `spirv-cross` was absent from the current PATH. Do not infer a full target build is ready from passing C/Metal probes.

Python test dependencies were installed in a temporary virtual environment outside the checkout. No production code was changed. Initial missing-Python-package errors were corrected before the final results below.

| Check | New result | Limit |
|---|---|---|
| Jamboree target/source-tool tests | 12 packaging/input/tool tests and 4 shader tests passed. | Synthetic inputs and public identifiers; no target assets. |
| Jamboree `t_vbuf` | 43 checks, zero failures, Metal API validation enabled. | Shared vertex-buffer behavior. |
| Jamboree `t_geometry` | 127 checks, zero failures, Metal API validation enabled. | Synthetic geometry, feedback, discard and indirect behavior. |
| Existing `t_vin` | 14 checks, zero failures, Metal API validation enabled. | Synthetic mixed-class vertex fetch relevant to KTGL. |
| Existing `t_tbuf2d` | 37 checks, zero failures, Metal API validation enabled. | Synthetic wrapped texture-buffer reads relevant to KTGL lighting. |
| Root Python suite | Ran 460 tests; failed in eight subcases of the settings-panel test; one skipped. No remaining import errors. | All eight assert `window 1511x850, expected height 851 at 16:9`. This is a baseline failure in unchanged TOTK settings code on this display; cause not fixed or fully diagnosed here. Root `make check` exits nonzero and its later C recipe is not reached. |
| Six CI C probes, invoked separately with UBSan | RomFS paths, feedback settings, controller layout, language, audio ramp and bow state all passed. | Synthetic source checks only. |

Logs from the final runs are local temporary artifacts: `/private/tmp/macnative-aoc-root-check.log`, `/private/tmp/macnative-aoc-gpu-analysis.log`, and `/private/tmp/macnative-aoc-target-probes.log`. Jamboree staging/build outputs are under `/private/tmp/macnative-aoc-gpu-analysis`, not the checkout. These test results do not substitute for a full build, graphics comparison, or playing Age of Calamity.

## Decision and next action

Use **Jamboree's pinned nxnative runtime plus its isolated Mac build/package structure**, restore the matching target profile, and explicitly correct the shader mode and target memory/profile gates. Use Bayonetta's fail-at-the-first-real-gap method, TOTK/Odyssey's offline shader and measured-cache discipline, Smash's ABI scrutiny, and Unicorn's independent-build/save-validation standard.

The next thread should implement and validate this target, beginning with available original-input paths and dependency preflight. It should not repeat a general port survey, rewrite an emulator, copy Zelda game addresses, or spend its first milestone on enhancement settings.

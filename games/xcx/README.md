# Xenoblade Chronicles X: Definitive Edition — diagnostic Mac host

**Gameplay acceptance remains open.** Base 1.0.0 (`0100453019AA8000`)
builds as a native arm64 host and local diagnostic `.app`. The opt-in
`XCX_EXTENDED_VA=1` layout lets the original SDK reserve its 60 GiB region,
complete constructors and reach `nnMain`. Identity-checked SDK encoder masks
preserve bit 39 in IPC pointers; input/rewrite integrity checks remain enabled.
Local save mounting, Core Audio and Metal presentation are running. Indexed
QUADS render readable text/menu UI, and the local `prepo:u` session query lets
new game reach avatar customization. The user confirmed USB Pro Controller
menu operation and audible audio.

The 2026-10-29 renderer correction preserves vertex shader storage writes
under rasterizer discard and orders producer/consumer draws at NVN barriers.
The native window now shows intact title characters, a complete avatar and
both characters in the following forest sequence at 1920x1080, about 30 FPS.
The user subsequently confirmed visible gameplay, USB Pro Controller input and
audible audio in this copied profile. This is scoped early-field acceptance,
not whole-game compatibility. The later `ngc:u` path exposes
an unavailable dictionary backend through its documented DataAccessError;
the original game handles that error. The isolated diagnostic reaches field
movement/camera and saves, quits, restarts and reloads the same manual slot.
No profanity-check success or whole-game compatibility is fabricated.
The engine retains its 60 Hz base / frame-step interval 2; its window requests
present interval 1 and the engine's separate wait holds 30 FPS. No complete
60 FPS game patch is claimed. CoreVideo -6661 indicates an inactive display;
activate/unlock the local display before a windowed run.
This is an unfinished port, not a CPU emulator or a generic ROM converter.

All source dependencies live in this directory. See
[source provenance](third_party/COPIES.md). No Zelda/Bayonetta game patches
or addresses are used. Player data, generated headers, rewrites, keys,
shader caches and saves stay outside the checkout.

## Inputs and preparation

Use your own base NSP and console keys. The inspected 1.0.1 update has an
invalid program partition and is not an input for this target. Exact checked
identities are pinned in `assets/expected-v1.0.0.json`; do not replace the
hashes merely to accept a different build.

With the existing Python environment satisfying `requirements.txt`, put the
work directory on local APFS (`WORK` below), then:

```sh
PY=/path/to/python
WORK=/absolute/local/work
$PY scripts/extract_exefs.py /path/to/base.nsp --keys /path/to/prod.keys \
  --title-id 0100453019AA8000 --output "$WORK/extracted/v100/exefs"
$PY scripts/extract_romfs.py --keys /path/to/prod.keys \
  --manifest "$WORK/extracted/v100/extraction-manifest.json" \
  --output "$WORK/extracted/v100/romfs"
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 "$PY" third_party/d2r/patch_nso.py \
    "$WORK/extracted/v100/exefs/$m" "$WORK/rewritten/v100" "$m"
done
make prepare WORK="$WORK" PY="$PY"
make -j4 runtime WORK="$WORK"
make shader-tools WORK="$WORK"
```

Place the six verified system fonts and their manifest under
`$WORK/extracted/firmware/shared-fonts` before preparation. Missing fonts
remain a named failure if the guest requests them. Preparation verifies
the pinned ExeFS, rewritten images, relocation/gate metadata, RomFS content
hash and fonts. It generates all 206,653 relocations locally.

Before execution, run `third_party/d2r/structck.py` and `equiv.py` for all
three rewritten modules, as documented in those tools. Unicorn is a
test-only dependency; it is not linked into or called by the runtime.

## Mac checks and local bundle

```sh
cd "$WORK"
./build/xcx-native rewritten/v100 probe
./build/xcx-native rewritten/v100 probe-identity-negative # must fail
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-address
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-sdk-ipc
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-memory
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-touch
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-gfx
XCX_EXTENDED_VA=1 MTL_DEBUG_LAYER=1 ./build/xcx-native rewritten/v100 probe-gpu-ds-clear
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-fs
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 probe-save
XCX_EXTENDED_VA=1 ./build/xcx-native rewritten/v100 boot
```

The current probe passes 4,000 original cross-module clock calls on four
threads. The identity negative test fails as intended. A fresh rewrite of
all three NSOs exactly matches all 21 pinned image/metadata hashes and the
previously structurally/semantically checked output (36,687 sites,
900 mutation controls detected). The six-level RomFS check passes for
14,326,985,704 bytes and 53 files. These are not gameplay tests.

From this source directory:

```sh
make app WORK="$WORK" PY="$PY" APP_EXTENDED_VA=1 APP_GPU_SUBMIT=1 \
  APP="$WORK/Xenoblade Chronicles X Device ID Diagnostic.app"
```

The ad-hoc signed `Xenoblade Chronicles X Device ID Diagnostic.app` is a **local
diagnostic launcher**, dependent on `WORK` (recorded in `Info.plist`). It
logs to `WORK/logs/app-launch.log` and uses its own `WORK/user-profile`.
It is not a standalone distributable game. Existing bundle paths are never
overwritten by the packager.

The local shader translator and pinned SPIRV-Cross build are running. Startup
presentation and shader compilation are reached; complete shader/NVN coverage
and broader gameplay acceptance remain. Do not
return a false allocation success, overlap occupied host mappings, shrink
the game's request silently, or disable input/data checks.

Mac only: no XCX iOS build or device acceptance has been performed.

## Diagnostic history and implementation details

The notes below preserve the order of experiments and their original acceptance
limits. Later observations supersede earlier open rendering/field-control
items; the current scoped status is summarized above and in the final section.

### Extended native address space

`XCX_EXTENDED_VA=1` is an explicit macOS-only experiment. It reserves one
owned 70 GiB `PROT_NONE` region between 2^39 and 2^40 for the existing
stack/heap areas and a 64 GiB alias range. Mach FIXED allocations never use
OVERWRITE; occupied ranges are skipped. The SDK receives the actual ranges
through GetInfo. Physical page commits still obey the normal application
pool budget. No request is shrunk or falsely reported as successful.
The legacy layout remains the default; the packager stores the opt-in in
`XCXExtendedVA` and the launcher sets it explicitly.

The earlier blanket claim that HIPC addresses stop at 39 bits was incorrect.
The [official libnx HIPC definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/hipc.h)
encode 42-bit static, 58-bit mapped and 48-bit receive-pointer addresses.
`probe-address` exercises the actual decoder widths and real sparse mappings
at the ends of a 60 GiB region; it is not evidence of gameplay compatibility.
The original SDK also independently completes its own 60 GiB reservation.

The FSP control plane handles PID initialization and access-log mode query
1005 only; unknown commands still stop. Current-title save-existence
functions use the real local profile with their distinct bool/Result ABIs.
The guest libc++ system clock receives host Unix microseconds. Nested exit
sections defer a native window close until the guest leaves the section.
The first protected close sends one `AppletMessage_ExitRequest` (4), as defined
by [libnx](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/applet.h).
One queue slot is reserved for that request so focus notifications cannot
discard it. Retries do not force a section to leave or duplicate the request.
These host mappings do not repair the rejected update or bypass data checks.

Run the focused source checks with `python -m unittest discover -s games/xcx/tests`
from the repository root. All 13 pass. On the contribution base
`032a05dff7215177912edfb2d01c9e20089d92a7`, the full root suite ran 1,246 tests:
163 skipped, one failure in
`test_builder_ports.Commands.test_5_timeout_kills_the_whole_group_and_keeps_partial_output`
(shell exit 137 instead of -9). The same failure reproduces in an unchanged
archive of that public base; XCX does not change the root test or runner.
The earlier 466-test run and settings-panel subcase failures mentioned in the
chronological notes below describe the older base, not this contribution run.
The curated tree also builds successfully with `make -j4 runtime` using the
previously verified generated headers in a separate local work directory.
No XCX iOS build has been performed. The active display and native window are verified.
A later guarded close exposed a missing guest notification; after the fix,
the real app's Quit menu closes the native run with exit 0. Closing during an
active game save, correct rendering and gameplay acceptance remain open.

### Window follow-up, 2026-10-28

The touch probe checks the real plural SDK override, zero contacts, increasing
sample numbers and exact write bounds. The memory probe now includes 8/32 KiB
static module stacks and rejects text/locked RELRO sources. The graphics object
probe and Metal-validated depth/stencil probe pass (9,220 checks, including
integer GPU reads through inherited/explicit stencil views and a depth override).
Optional pipeline prediction stops before future program bindings: resolving
those during speculation read absent control headers in the existing cold-program
probe. Actual replay still resolves all bindings normally.

`bsd:s` uses the existing `bsd:u` offline backend; socket creation returns
`ENETDOWN`, with no host socket or network grant. Unknown commands still fail.
The eight focused source tests pass; no new full-root pass is claimed (the prior
root run had eight unrelated Zelda settings-panel subcase failures).

`--gpu-submit` / `APP_GPU_SUBMIT=1` explicitly enables GPU submission and requires
both local shader executables before packaging. The launcher sets `XCX_SHADER_ROOT`
to the external work directory, keeping shader tools/caches outside the signed
bundle. Default diagnostic bundles still leave GPU submission disabled.
The pinned SPIRV-Cross commit is `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`;
this test rebuilt it from an existing clean local git object (no configured
remote), and checked XCX's own NativeAOT translator SHA256. No new dependency
or game downloads were needed. Bundles remain local diagnostics, not playable
or standalone packages.

### SDK encoder compatibility

The public HIPC format and the host decoder allow 40-bit addresses, but XCX's
original SDK encoder narrows mapped/static addresses to 39 bits. Under the
experimental layout, this truncated an AudioOut name buffer and crashed the
host. `runtime/sdk_ipc_compat.c` extends precisely seven `AND` masks in the
pinned 1.0.0 SDK from `0x1c`/`0x1c0` to `0x3c`/`0x3c0`, preserving bit 39.
All seven original words, text bounds and the complete input image identity
must match before any word is changed. Only the loaded copy is adapted before
RX sealing; ExeFS, rewritten files, expected hashes and relocation identity
checks are unchanged. Legacy layout does not apply this adaptation.

`probe-sdk-ipc` calls the actual rewritten SDK encoder at its verified entry.
It checks 35 mapped/static/automatic buffer cases around the 39/40-bit boundary,
sizes, modes and write canaries. The unadapted SDK fails at bit 39; the adapted
SDK passes. Source tests also verify fail-before-write for a mismatched final
site, wrong image identity and truncated text, plus disabled/other-module cases.
A 45-second window run with Metal API validation advanced beyond AudioOut and
translated over 300 shaders before the bounded test was stopped. A running
shader compiler and open Core Audio stream are not audible/gameplay acceptance.

The next observed null-service crash was `nn::oe::GetPseudoDeviceId`. It now
returns a stable local per-profile UUID, derived with a separate namespace
from the existing persisted random local account identity. It does not query
Mac hardware identifiers or Nintendo services. The UUID test runs the actual
account persistence across processes, checks different profiles, version/variant
bits and exact 16-byte output bounds. This does not establish a Nintendo device
identity or online compatibility. The follow-up window run advanced past this
crash and translated 749 shaders, but the visible initial 3D sequence has severe
graphics errors and the next screen remains black. An initial Command-Q exit
did not establish reliable guarded shutdown: a later run remained open until
its 240-second test timeout. The exit-request fix above has a red/green test
against the production applet queue, including nested sections, concurrent
close requests and a focus-notification flood. All eight focused tests pass.
The rebuilt diagnostic reached 30 FPS/751 shaders and then exited normally
through its Quit menu; its signature still verifies afterward.

The USB Pro Controller is confirmed by the user and freshly recognized by the
native runtime, including motion samples and haptics capability. This does not
accept in-game input, motion use or felt rumble. A captured render frame has no
dummy texture bindings and all 1,109 captured CPU/GPU uniform pairs match.
The sampled font atlas contains legible glyphs despite unreadable on-screen
text. These scoped observations narrow the investigation; they do not explain
or fix the incorrect rendering. This is not gameplay acceptance.

### Indexed quads (2026-10-28)

The font atlas, captured vertex streams and translated shaders were intact:
replaying those inputs offscreen produced readable text. The production layer
instead explicitly discarded every indexed `QUADS` draw. The XCX-only copy now
converts 8/16/32-bit source indices to triangles on the submission's GPU timeline,
including unaligned and GPU-written sources. It records primitive-restart state,
drops incomplete quads, preserves the default last-corner flat value and carries
base vertex, instance count and base instance in GPU indirect arguments.
Each outer execution reconverts; nested deferred replay reuses that execution.
Non-quad custom restart and nondefault provoking modes remain existing limits.

`probe-gpu-indexed-quads` uses synthetic inputs and actual GPU readback with
Metal validation: index/argument comparisons, restart enabled/disabled/custom,
incomplete input, GPU-produced data, and pixel-level winding/flat-color/instance
checks (218,640 checked values on M3 Max with Metal validation). `TOTK_PROBE_NEGATIVE=1` omits the draw; the pixel assertion must fail.
Run one native runtime at a time through the existing game-slot guard.

The new diagnostic bundle displays a readable 1.0.0 title menu, logo and menu
labels (17 FPS initially, then 30 FPS at a 3600x2024 window). The user confirms
USB Pro Controller menu operation and audible sound. Gameplay rendering,
in-game controller/motion/rumble and a real save/quit/restart/reload remain open.
This local diagnostic still depends on its external WORK directory.

### Session-id service and presentation size (2026-10-28)

The following new-game transition stopped at `prepo:u` service acquisition.
The original SDK's `AddSessionId` requests `GetSystemSessionId` (10400): the
XCX host now returns a process-local, random, nonzero 64-bit session id, stable
across sessions and threads. Report encoding stays in the guest; the existing
offline save hooks do not upload reports. Other service commands still fail
with their name and id. The regression checks eight concurrent callers and
rejects an unsupported report command. Ten focused XCX source tests pass.

The window starts at 1280x720 points. Its Metal presentation drawable is capped
at 1920x1080 pixels, including Retina and full screen, preserving its aspect
ratio. The native title menu has been checked at 1920x1080/30 FPS. This changes
presentation work only; guest render targets and timing remain untouched.
The host deliberately keeps a 60 Hz base and the game's present interval
(2 means 30 FPS). Lower presentation resolution is not a 60-FPS timing patch.

Original upstream `ly05010419/MacNativeGames` was checked through
`f0e76c2b2d76f24ba5eb4f2f178401fd2910396f`. Recent changes include guarded
texture reads, write-back waits and quad-strip support. Its indexed-quads
path still explicitly rejects primitive 7; it does not replace the XCX fix
above or provide a ready-made XCX port. No broad upstream merge was applied
to this diagnostic. The new run opens prepo:u and reaches avatar customization.
The user confirms completing their character and the game continuing; the next
sequence is visible with subtitles at 30 FPS/1920x1080. The avatar model was
not visible at an earlier customization observation, so correct 3D rendering,
controllable gameplay and real save/restart/reload acceptance remain open.

### Character rendering follow-up, 2026-10-29

Two independent NVN-to-Metal defects caused missing or stretched characters:

- `SetRasterizerDiscard(true)` skipped the complete draw, although vertex
  shader storage writes still have to execute. XCX uses those writes to build
  its skinning matrices. The captured consumer referenced 459 distinct 3x4
  matrices, all zero. Discard now selects a distinct Metal PSO with no fragment
  function and a void vertex entry retaining the generated storage operations.
  Full culling and an empty scissor cannot suppress that vertex work; explicit
  render-disable still does. The shader patch cache uses one shared lock.
- NVN `OP_BARRIER` was ignored while successive models reused one transform
  buffer. Nonzero barriers now close active render/blit/compute encoders, using
  the existing device-visible Metal stage barriers. This is conservative;
  precise stage optimization is future work. Zero-bit barriers remain no-ops.

`probe-gpu-raster-discard` executes real production replay/PSO/buffer bindings,
without game assets. It checks discard toggles, fragments, culling, scissor,
vertex-only programs, render-disable and attachment-free draws, followed by 32
producer/consumer reuse rounds of 8,192 words in both direct and render-stream
modes: **535,296 checked values** under Metal validation. Before the barrier
fix it failed on the first saved word; negative controls independently drop
discard work or omit barriers and must fail. Run with no other game process:

```sh
XCX_EXTENDED_VA=1 MTL_DEBUG_LAYER=1 ./build/xcx-native rewritten/v100 probe-gpu-raster-discard
XCX_EXTENDED_VA=1 TOTK_PROBE_NEGATIVE=1 ./build/xcx-native rewritten/v100 probe-gpu-raster-discard # must fail
XCX_EXTENDED_VA=1 TOTK_PROBE_NEGATIVE=2 ./build/xcx-native rewritten/v100 probe-gpu-raster-discard # must fail
```

An opt-in geometry readback now also handles directly bound vertex storage
buffers. For the same byte-identical captured avatar vertices/indices and the
same shader pair, all 459 referenced matrices are nonzero and finite after
the correction. This supports the native-window observation; it does not prove
all materials/scenes or whole-game compatibility. Test profiles and earlier
bundles are retained separately from the user's original profile.

### Unavailable NGC dictionary backend, 2026-10-29

The host registers `ngc:u` commands 0..3 (GetContentVersion, Check, Mask,
Reload) and returns **DataAccessError `0xcc92`** (module 146, description
102). It does not implement a dictionary reader, check any text, report a
content version or write a masked result. Newer commands remain explicitly
unsupported. This is an honest service failure, not a clean-filter stub.

The original SDK reads output values only on success. In the identity-pinned
1.0.0 main module, the helper at `+0x3af1e0` handles initialization/masking
errors and returns 1; its caller at `+0x54b300` selects the original text on
that path. Neither guest branches nor filter results are patched. The ABI and
error definition were checked against the open-source
[NGC service](https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/main/src/Ryujinx.Horizon/Ngc/Ipc/Service.cs)
and [Result definitions](https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/main/src/Ryujinx.Horizon/Sdk/Ngc/NgcResult.cs).

The source regression exercises 32,000 concurrent command replies, checks the
error Result, empty output descriptors and untouched input/output buffers,
and rejects unknown command 4. A mutated blanket-success backend must fail
the same harness. All eleven XCX source tests pass. The runtime and isolated
NGC Diagnostic bundle build without new dependencies.

The live copied-profile run logs command 2 returning `0xcc92`, dismisses the
previously fatal first tutorial and reaches field control. Keyboard movement
reduces the objective distance from 132 m to 118 m; the camera rotates and both
models remain visible. Manual slot 1 (Prologue / Awakening / Primordia) reports
save completion. After regular app Quit, a fresh process loads that exact slot
and resumes movement/camera at about 30 FPS, 1280x720 in the compact window.
New shaders can briefly stall; save thumbnails are black. The user subsequently
confirmed visible figures, audible audio and the original USB Pro Controller.
Later scenes/combat still need acceptance.
The original profile is unchanged. Keep the copied diagnostic progress rather
than replacing the user's profile; no standalone/NAS package is claimed.

### Grass flicker investigation and shader warm-up (2026-10-29)

The initial attribution of black grass to the renderer's default
`preserveInvariance` setting was withdrawn on follow-up audit. Both baseline
and comparison logs show invariance enabled: the existing host constructor
already set `DM_PRESERVE_INVARIANCE=1`. The observed image-series differences
therefore do not establish an invariance fix. The cause of the user's strong
ground flicker remains open.

The renderer now also defaults this option on, matching the existing host
configuration. The regression compiles the production options function and
verifies 1,024 concurrent cold calls, safe math and explicit diagnostic
controls. It tests that API contract, not the unresolved visual defect. All
twelve source tests and four GPU probes (535,296 / 218,640 / 9,220 / 768 checks)
pass with Metal validation. Metal's invariance requirements are documented by
[Apple](https://developer.apple.com/documentation/metal/mtlcompileoptions/preserveinvariance).

Persistent shader/pipeline warm-up is already enabled: this forest restart
prepared 1,142 shader libraries and 1,613 render pipelines in about 25 seconds.
The same scene then reported zero new shader variants. This is observed-cache
coverage, not full-game offline precompilation; four manifest entries had no
translated source yet. A separate 39.4-second window without diagnostic dumps
still logged three adaptive frame outliers (47.1, 71.7 and 53.9 ms), each with
zero new libraries/pipelines and zero compilation time. Remaining hitch causes
are unresolved; do not promise that cache warm-up removes every stall.

### Pacing and diagnostic follow-up (2026-10-29)

VSync is already enabled on the Metal layer. A separate copied-profile
experiment that changes the engine interval to 1 reaches 60 FPS in this forest,
but a 300-ms camera input rotates markedly farther than the same input at
30 FPS. A second timing constant alone does not establish correct camera,
UI, effect or cutscene speed. Neither temporary game patch is shipped.
The public FPS reference targets a different executable build (1.0.1); its
offsets cannot safely be applied to this pinned 1.0.0 input.

NVN slow-frame and presentation summaries now compare delayed feedback with
the originating frame's budget, rather than always using 25 ms. The optional
`DM_NVN_DIAG_FPS=30` labels a **known fixed-rate measurement** when an engine
waits separately from its window interval, as XCX does. Without that override,
the budget follows the window's requested period. It does not infer the engine
cap, and changes neither VSync nor pacing nor game time. Do not retain a
30-FPS override while testing 60 FPS. `@budget_ms` records the chosen budget.
The regression covers 30/60/120-FPS budgets, delayed cap transitions, missing
callbacks, discarded frames and valid/invalid overrides; all 13 focused
source tests pass. This is measurement correctness, not a stutter fix.

A valid invariance-off comparison and the original invariance-on control both
still show black grass. Forcing the two vertex programs to share one position
function also leaves defects. An isolated Metal replay of 92 captured grass
draw pairs matches 1,135,969 depth-EQUAL coverage pixels, but uses Metal 2,
white textures and simplified attachments; it does not validate the actual
Metal 4 path, alpha masks or live buffer ordering. That replay alone is no terrain fix.

### Opt-in grass wind snapshot (29 October)

`XCX_GRASS_WIND_SNAPSHOT=1` enables a narrowly scoped **experimental workaround**
for the two captured 1.0.0 grass vertex shaders and their four-vertex shape.
It GPU-copies the four wind vectors once per source address/frame and binds
that immutable copy in both depth and color passes. One 16 KiB buffer holds
up to 64 addresses; previous frame buffers retire through the existing GPU
completion fence. Unsupported shapes/capacity fail explicitly. The default
remains off; this is not a general vertex-buffer synchronization repair.

A same-process A/B/A diagnostic kept GPU copies/barriers unchanged and only
switched the bound stream: 30 frames per leg showed clean snapshot grass,
black silhouettes with the live input, then cleaner snapshot grass again.
This implicates live wind input/versioning; the original producer/write or
barrier defect is not established. On 29 October, the user confirmed that
flicker was gone while moving and rotating the camera in the forest. The
active Fenced Wind Test app and runtime hash match the snapshot build,
despite the acceptance prompt's older Terrain Fix label. This accepts that
scene only; later scenes remain unverified. The earlier bounded diagnostic
allocation is not used here.

`probe-gpu-xcx-wind` exercises the actual snapshot helpers with Metal API
validation: source overwrite between depth/color submissions, same-frame
reuse, next-frame refresh, other-stream isolation and completed retirement.
`XCX_WIND_PROBE_NEGATIVE=1` deliberately reads the overwritten live source
and must fail. Use the local Fenced Wind Test app with its copied profile;
no game-time/FPS patch or change to the original save is part of this test.

Final checks: runtime build and 13 focused source tests pass. The Metal-validated
wind probe passes 3,265 checks, including no live snapshot objects after
completed retirement; its live-source negative control fails as intended.
The existing vertex-stream GPU probe also passes with the option off. Two
30-frame field series and a partial 14-frame series show cleaner grass; a
roughly six-minute app run includes movement/camera, then regular quit.
Capture bursts lower FPS and are excluded from performance conclusions.
RSS samples were about 1,579,200 and 1,580,464 KiB, not a full leak benchmark.
The original profile's three file hashes remain unchanged. Higher time-correct
FPS, residual hitches and broader gameplay acceptance remain open.

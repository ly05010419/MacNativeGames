# How it works

The game is not emulated. Its own machine code runs on the Apple chip; this project supplies what the Switch would:
a loader, the operating system's services, and a graphics driver.

## The code (games/totk/third_party/d2r, games/totk/runtime/loader.c)

The Switch and Apple silicon are both ARM64, so almost every instruction of the game runs as it is. A few things
macOS does not allow are rewritten ahead of time (`patch_nso.py`): the platform register x18, the thread pointer
register, system calls (`svc`), and the counter registers. Each one becomes a short jump into a stub that does the same
thing for macOS. The rewrites of versions 1.0.0, 1.1.2 and 1.4.3 were verified site by site (every rewritten site executed with random
inputs against the original in an emulator, with negative controls); the build checks that your rewrite is byte for
byte the selected version (`games/totk/assets/expected-v1.0.0.json` / `expected-v1.1.2.json` / `expected-v1.4.3.json`).

The rewrites are what makes the code the app's own: it is bound by SHA-256, so a patch cannot be applied while the game
runs. A player mod that changes the code therefore merges its main-module patches *before* that step
(`--mod <mod folder>`, `games/totk/scripts/mod_exefs.py`): an IPS / IPS32 / pchtxt patch is applied to the verified
retail main (a `.pchtxt` says in `@nsobid-` which build of main it is for, with `@enabled` / `@disabled` switching the
sections on and off and `@flag offset_shift 0x100` required before the first patch line; an `.ips` carries no id, so it counts as a
main patch only when it is named `main` or after main's build id), refused if it lands on a site the app patches itself (the 60 FPS, resolution, ultrawide and cloth sites,
and the rewriter's registered cuts - `games/totk/scripts/patch_sites.py` reads them out of the C sources, and the signed
jump tables out of `patch_nso.py`), and the
merged module is then rewritten and bound like the retail one. Extra code modules a mod ships are not loaded: the app
loads the three modules below. The same build replaces the version's player mod folder with the mod's RomFS files only
when `--install-mod-romfs` is given (`games/totk/scripts/mod_folder.py`): that folder may hold files of the player's own.

The loader maps the three rewritten modules (main, sdk, subsdk0) at their original layout, resolves their imports
against each other, and starts the game's own `nn::init::Start`.

## The operating system (games/totk/runtime)

The game talks to Horizon (the Switch OS) through system calls and IPC. The host answers what the game uses: threads
and synchronization, memory, the file system (the RomFS straight from the extracted image; saves in your profile
folder), time, settings, the account, input (keyboard and game controllers, with rumble and motion), audio (the
audio renderer with its effects, mixed and played through Core Audio), the software keyboard (the Mac keyboard), and
the movie decoder (VP9 through libvpx). A small number of fixed patches change the game itself, each checked against
the original instructions first: frame timing by real time, the 60 FPS present interval, the output resolution.

Guest memory must lie below 2^39 (the SDK encodes IPC buffer addresses in 39 bits). On macOS 26 the host reserves the
64 GiB alias region at `0x70_0000_0000` and the 6 GiB stack and heap below it. macOS 27 no longer leaves room for
either: the shared cache fills `0x1_8000_0000..0xF_C000_0000`, and the system reserves about 24 GiB at a randomized
place in `0x70_0000_0000..2^39`. When the fixed layout does not fit, the host places stack, heap and alias region side
by side in one free hole (as the iOS layout does) and shrinks the alias region in 4 GiB steps, down to 8 GiB, until it
fits; the game uses less than 3 GiB of it. The `guest layout` log line shows the result. macOS 26 keeps the fixed layout.

The pool the game is told it owns is `app_pool()` in `runtime/services.c`: one retail figure for every version, 3,285 MiB (`0xcd500000`), plus the page-rounded amount that the
`TOTK_OUTPUT_RES` / `TOTK_RENDER_RES` HeapDef overlay hands to the texture heaps. `TOTK_APP_POOL` replaces that whole sum rather than adding to it, so a value chosen for a big resolution overlay has to carry its
own margin; `GetInfo` 6 and 21 report whatever the two give.

The game does not use the pool directly: during startup it asks for a main heap of the pool minus about 1,170-1,200 MiB of images and system margin, and that heap is what the game sizes itself
by. Three points measured on 1.2.1 with the 2560x1440 overlay: pool 3,373 MiB gives the usual 2,204 MiB heap (`0x89c00000`), 5,120 MiB gives 3,924 MiB (`0xf5400000`), 5,248 MiB gives
4,052 MiB (`0xfd400000`). Two bounds apply. `svcSetHeapSize` refuses a request above `HEAP_CAP` (4 GiB) or above the pool minus the mapped images, so the heap cannot be raised
past 4,096 MiB without raising the cap too. And the heap sits in the low reservation after the stack (6 GiB on the Mac; on iOS the reservation ends right at the
heap cap, with no slack). `TOTK_MEM_LOG=1` logs every `SetHeapSize` and `GetInfo` call.

That margin is worth knowing about because a mod that sorts "game memory" into tiers by measuring that heap reads the retail 2,204 MiB as its smallest tier, and can then refuse a resolution the
host serves without any trouble - warning about not enough memory while nothing is short of RAM. Raising the pool moves the game heap into the next tier, and an app bundle can carry the setting
without a code change, as a LaunchServices environment entry (re-sign the bundle afterwards):

```sh
P="Tears of the Kingdom.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :LSEnvironment dict" "$P"
/usr/libexec/PlistBuddy -c "Add :LSEnvironment:TOTK_APP_POOL string 0x148000000" "$P"
codesign -f -s - "Tears of the Kingdom.app"
```

The next packaging writes a new Info.plist, so the entry has to be added again after each build.

The memory is real, though: the committed heap grows by the same amount, so on a 16 GiB Mac expect more paging, not a free upgrade.

## The graphics (games/totk/third_party/d2r_gfx/gfx/nvn)

The game records NVN commands (Nintendo's graphics API) into command buffers. The NVN layer implements the API on the
host and replays each submitted command buffer on Metal 4: textures and buffers in the game's memory pools become
Metal resources (with dirty-page tracking for what the CPU changes), render targets become render passes, and draws
become Metal draws. Shaders are Maxwell machine code: the first time one is used it is translated by the Ryujinx shader
translator (to SPIR-V), SPIRV-Cross (to Metal Shading Language) and Metal, and the result is cached
(`~/Library/Application Support/MacNativeGames/TOTK/shader-cache`). The translator runs as one native program,
`totk_xlate --service` (`games/totk/tools/totk_xlate`: the Ryujinx translator with this project's patches and the
service that turns each request into the MSL and its reflection file), compiled ahead of time with .NET NativeAOT; it
ships ready-made in `games/totk/prebuilt/macos-arm64`, so neither building nor playing needs .NET, and playing needs no Python. Building still requires Python 3.14. The build also prepares a selected-version startup shader seed from the local RomFS: MSL, reflection and dependency indexes, not GPU-specific compiled binaries. The first launch imports it into the user cache and warms it before releasing the first frame. A pipeline identifier list alone cannot do this when its shaders are absent. New binding-dependent variants still use runtime translation. MetalFX upscales the game's 1600x900 scene into its
1080p or 1440p canvas and can generate in-between frames.

## Texture storage layout

Automatic block height follows NVN's integer rounding: truncate `height_in_blocks * 1.5 / 8` before choosing the GOB power of two. The deko3d heuristic rounds that division up, which differs at several dimension boundaries. For the shrine warp pad's 160×160 BC3-sRGB texture, the wrong H3 layout reads padding as black texels; NVN uses H2. The allocation size is 57,344 bytes in both cases, so a size check alone cannot catch this error. Explicit packaged layouts continue to override the automatic choice.

The correction was compared against the original SDK's CPU metadata functions for 40 uncompressed/BC3 boundary cases, including every mip's offset and block height: 16 mismatches before, none after. The asset's base mip and the running 1.1.2 shrine scene also confirmed the correction. These scoped checks do not establish whole-game rendering correctness. Run the game-data-free regression with `make -C games/totk probe-texture-auto-layout`; its expected sizes and block heights are independent SDK query results.

The same regression was reproduced and corrected in TOTK 1.4.3 on Apple M4 Pro against upstream `41e1e0c`: the baseline leaves half of the blue warp pad black, while the patched layout restores its complete pattern. Both runtimes used the same original save copy and camera input, upstream picture settings, and the same minimal host address-layout compatibility fix (not included in this rendering change). Startup prewarming was disabled equally for this pixel comparison; it is not a performance benchmark. Capture times and weather animation differ.

Historical TOTK 1.1.2 shrine captures and the fresh 1.4.3 pair are hosted as [separate evidence attachments](https://github.com/huahuahuahuahuahuahuahua/MacNativeGames/releases/tag/rendering-evidence-20261018), with visual comparisons in [PR #3](https://github.com/ly05010419/MacNativeGames/pull/3). Screenshot files are excluded from the code commits. This targeted comparison does not imply whole-game rendering coverage.

## Depth classification clears

The skill effects classify pixels into a D16 depth attachment and select their colour passes with an EQUAL depth test. On the tested Apple GPU, a load-clear of `0.5` becomes 32767 while a fragment writing `0.5` becomes 32768. The difference exposes the 64-pixel classification tiles as coloured squares when using Ultrahand or Recall. D16 attachment and texture load-clears now select the nearest 16-bit value, then provide a value inside both its truncation and rounding bins. A quarter-unit margin also avoids rounding below the intended integer when Metal converts the descriptor to float. Other depth formats retain their original clear values; fragment-based masked clears already use the fragment conversion.

`make -C games/totk probe-depth16-clear` takes the shared game slot and compares load-clears with an independent fragment-depth reference through both Metal 3 and Metal 4. The game-free fixture covers 777 values, including all 256 skill classification encodings, endpoints, nearby floating-point values and a deterministic random sample. On the local M4, each API had 292 mismatches before the correction and none after.

The live TOTK 1.4.3 comparison on Apple M4 Pro also reproduced the grid with Recall and removed it with this correction, using the same save and camera sequence described above. This is a targeted Recall check, not a claim that every skill or area was tested.

Historical 1.1.2 Ultrahand captures and the current 1.4.3 Recall pair are hosted in the same [separate evidence attachments](https://github.com/huahuahuahuahuahuahuahua/MacNativeGames/releases/tag/rendering-evidence-20261018) and linked from PR #3. No screenshots, game binaries, extracted assets or saves are included in the code commits.

## Vertex outputs a shader does not write

On Maxwell, a fragment shader reads `(0, 0, 0, 1)` for every component of an input its vertex shader never writes. The Ryujinx translator gives a vertex shader those defaults only for the inputs the next stage reads, which it learns from `SetNextStage`; the service translates every shader on its own, so no next stage is set and nothing was initialised. SPIRV-Cross starts the output struct at zero (`main0_out out = {};`), so the unwritten components reached the fragment shader as 0. The drop marker of a held item depends on them: its vertex shader writes only `xyz` of one output and its fragment shader multiplies the alpha by that output's `w`, so the marker comes out fully transparent. Patch `0005` initialises every user output a vertex shader writes to `(0, 0, 0, 1)` when no next stage is set; the shader's own stores then replace the components it writes.

Over the 18,064 startup variants of a 1.4.3 build, the patched translator changed 6,679 of the 7,000 vertex functions, each only by these initial stores (the variable numbering of the output struct shifts with them); none of the 10,849 fragment and 215 compute shaders changed. With the patch, the marker and its arrow show under a held material on 1.4.3 (Apple M2 Max). The title background gets slightly darker (mean luminance 61.2 -> 57.0): in another native port of this game, the same change was traced to the volumetric light pass, which multiplies the unwritten `w` into the translation column of its shadow matrix, so with `w = 0` the translation was lost. An existing shader cache keeps the vertex functions it already has (the seed import preserves shaders collected on the Mac), so the fix reaches an existing install once its cache is cleared or the chain name changes.

## Early fragment tests

Many surfaces are drawn twice: a depth prepass (alpha test, depth written) and then the G-buffer pass, which colours only the samples whose depth is EQUAL to the prepass's and writes no depth. The material fragment shaders of the G-buffer pass also store a per-tile flag to memory. A Metal fragment function that stores to memory has its depth test after it runs, unless it declares `[[early_fragment_tests]]`, and on Apple GPUs that late test saw a depth an ulp away from the one the prepass wrote along contour lines: the G-buffer kept its clear value there, and the stone lattice in front of the Great Sky Island start was covered in rows of dark dots (every resolution). The two vertex functions compute bit-identical positions, in the MSL and in the compiled AIR, and `preserveInvariance` changed nothing.

On the Switch these shaders test depth first. Byte `+0x718` of a fragment shader's NVN control section is its early-fragment-tests flag: the game's NVN driver reads it with the other fragment flags at `+0x719..+0x71d`, and Ryujinx receives it as the 3D engine's `EarlyZForce`. The runtime's translation requests and `prepare_shader_seed.py` pass it as `early_fragment_tests` (0x880-byte sections only; Breath of the Wild's shorter ones are not read), and `totk_xlate` gives it to the Ryujinx translator, whose SPIR-V then declares `EarlyFragmentTests` and the MSL `[[early_fragment_tests]]`. Of the 13,645 fragment shaders of 1.4.3, 1,677 set the flag, among them 775 of the 840 that store without discarding; none of them writes depth or a sample mask. The flagged shaders' MSL changes under the same variant names, so the translation chain went from `totk-native-v7-unverified` to `totk-native-v8-unverified`: a v7 cache would keep serving them without the attribute.

## Buffer copies through aliased memory

The same game memory can be reached through two Metal buffers, each with its own GPU virtual address: a virtual memory pool maps a physical pool's pages a second time (`mach_vm_remap`), and two memory pools can wrap the same CPU memory. A barrier with only device visibility does not make a write through one buffer visible to a read through the other. Every buffer-to-buffer copy (`mtl_sub_copy`, and the copy fallback of inline uniform updates) therefore carries `MTL4VisibilityOptionResourceAlias` in the barriers the copy encoder already places and at its end; no barriers are added. The fix comes from the original port (`d309e62`, by Codex); its copy-batch experiment, off by default there, is not included.

`make -C games/totk probe-gpu-copy-alias` replays 256 dependent copies and fills over 64 KiB through two no-copy buffers over one allocation and through a virtual pool's view, across encoder boundaries, and compares every byte with the same steps done on the CPU (524,288 checks). With `TOTK_PROBE_NEGATIVE=1` the copies lose the alias visibility: on an M4 every run read a stale byte (byte 512 read 125 instead of 207). TOTK 1.0.0 makes no buffer copies in the world; 1.1.2 and 1.4.3 make one per frame. Whether that copy reaches its memory through two buffers has not been traced, and no visible defect has been attributed to the missing visibility.

## Decals that read the G-buffer they blend into

Ground decals and the terrain material blend into the G-buffer by hand: in the pass that holds the albedo, normal and R32F
attachments, each draw samples those attachments at its own pixel, mixes by the material id in the albedo's alpha and
writes them back. The Switch renders straight to memory, so a decal sees the decals drawn before it. An Apple GPU keeps
the pass in tile memory, and a sample reads memory, which holds the pass's start except in tiles the GPU happened to store
early. The store-before-sample (`att_unstored_read`) makes the first decal of a run see the pass's earlier draws but lets
the run share one store, so overlapping decals blended differently tile by tile: 32 x 32 squares on the floor under the
Ultrahand close-up in Ukouh Shrine (1.2.1, M4 Max), already present in the G-buffer albedo and normals. With a store
before every such decal the squares go, at 96 stores in that frame (84 of them inside the terrain material's draws).

Such a draw now reads those attachments from the tile (`[[color(n)]]`, programmable blending) whenever the sampled texel
is the fragment's own pixel, through a rewritten fragment function (`nvn_fbread.h`, `att_fb_take`); other texels are sampled
from memory as before. The attachments it reads this way need no store first. A shader that also gathers, reads or queries
such a texture keeps the old path, as does a draw in a merged or fused pass, with a border-colour variant, a depth output or
a MetalFX role. `TOTK_ATT_FB=0` turns the rewrite off. The variant is compiled from MSL on the submit thread the first time
its shader is met (about 90 ms cold on an M4 Max), never in the background: the draw has given up the store before it, so it
cannot be drawn with the plain function meanwhile. Like the other patched variants its source is kept, and the next
packaging compiles it into the app; in Ukouh Shrine 16 shaders got one.

`make -C games/totk probe-fbread` draws four overlapping decals in SPIRV-Cross form in one pass over memory holding 0, each
adding 0.25 to what it samples at its own pixel. Rewritten, all 49,152 pixels reach 1.0; the original function, the
negative control, reaches 1.0 in none. The neighbouring-texel case draws only even columns and samples untouched odd
columns: drawn pixels stay at 0.25 and untouched pixels at zero, independent of when a tile is stored. A texture that
is also `read()` is refused. The original game's 1.2.1 report found the squares gone with no store left before the decals.

## The build (build_app.py)

1. Detect XCI (HFS0 secure partition) or NSP (PFS0) by its header. Read the base Program NCA in place, extract ExeFS,
   then rewrite and check the three modules against the verified 1.0.0 hashes. NSP common tickets supply encrypted
   title keys in memory; `prod.keys` supplies the matching titlekek. Rights-free NCAs use their key area, as before.
   The importer follows nxnative's container/common-ticket approach without its iOS packaging or .NET dependency.
2. The RomFS (17 GB) decrypted and verified through all six IVFC levels. The shared fonts come from the XCI's
   system update partition, `--firmware` (an unpacked firmware NCA directory), or `--shared-fonts` (six extracted
   BFTTF files). Actual font bytes and the complete manifest are verified before reuse. Then the
   RomFS overlays (small data patches: quick-menu capture timing at high frame rates, the game-over retry, the heap
   sizes the 1440p output needs) are made from your own files. The game-over retry patch only matters on 1.0.0:
   1.1.2 and 1.4.3 read Nintendo's own fixed flow (`DmF_SY_GameOver.110`), so there the patch of the old file is made
   and checked but never read. At start the runtime also loads player mods from
   `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>` in the same manifest format, after the build's
   overlays. Every listed file is hashed at start, and each one is opened only when the game opens it. A line whose
   original SHA-256 is `-` adds a file the RomFS lacks (numbered after the RomFS's own files; its folders exist for the
   game's entry-type queries); `scripts/make_mod_manifest.py` writes such lines for new files.
3. SPIRV-Cross compiled with clang; the prebuilt shader translator checked against its SHA-256. `prepare_shader_seed.py`
   reads dictionary-compressed startup packages from the selected, verified RomFS, parses supported NVN BNSH programs
   and translates default-binding variants. The version, translation chain, translator/preparer hashes and every
   output file are checked before cache reuse and packaging. No prior user shader cache is read.
4. The game binary built against your extracted files (it checks their hashes at every start) and everything put into
   an app bundle; the app's launcher points the game at your profile, cache and log folders. The builder seals and
   strictly verifies the complete bundle before replacing the previous app, then refreshes its LaunchServices record.

The optional `games/totk/tools/install_desktop_entry.py` installer keeps one desktop link to that bundle. When replacing
the older development launcher, it verifies a copy of its profile in the standard user-data folder, keeps an upgrade
backup and leaves the original profile intact. The game slot, shader cache and RomFS verification records also live
outside the app, so playing does not change the signed resources. A full verification record is reused only while the
image's file identity and modification metadata match; `TOTK_ROMFS_VERIFY=full` forces a fresh SHA-256 check.

The output is macOS-only: Apple silicon, macOS 26 or later, and an AppKit launcher. There is no iPhone/iOS build option.
The separate cumulative 1.1.2 and 1.4.3 updates are accepted with `--update` (NSP or NSZ). NSZ updates are decompressed and their NCA hashes verified locally. Other versions, DLC, combined packages, personalized tickets and compressed base games (NSZ/XCZ) remain unsupported.
Changing an input path, size, or modification time invalidates the extraction manifest, including its NCA offsets;
verified version-specific output can still be reused after the new input's code passes the hash checks.

Container tests use generated encrypted fixtures with no game data or real keys:

```sh
/path/to/TOTK_build/venv/bin/python -m unittest discover -s tests -v
```

## Cumulative update path

`--update` validates the base code and selects the update version by its exact ExeFS hashes, then checks its rewritten modules against that version's fixed manifest. An update's Program NCA can have the base title ID; the BKTR section marker identifies the input type, while exact ExeFS and merged RomFS hashes identify the supported version. Renaming a file cannot change the selected version or make an unsupported version pass.

The pinned hactool commit `1d64a83450e025622f3468c28fc4164dad2c5ef6` merges the base and update Program NCAs. The build adds a small explicit title-key-file hook (`MNG_TITLE_KEYS`): temporary common tickets for both inputs stay in a 0600 file under a private temporary directory, and are removed on success or failure. It does not edit the player's key store or pass keys in process arguments. A bounded-input hook (`MNG_BASE_RANGE_OFFSET` / `MNG_BASE_RANGE_SIZE`) reads the original container's base NCA in place, preventing a second temporary 17 GB base copy; only the update NCA is extracted. All six IVFC levels must verify, then the final image must match the selected version's SHA-256. The hactool output uses `--sectionN`, because `--romfs` does not export this BKTR section.

Runtime addresses, resource overlays, generated headers and modules are selected together. The app contains one executable and one selected data set; its launcher is compiled for that same version and `--check` verifies the bundle's version metadata. The normal window title reports the version. Saves keep their existing location, with an atomic full pre-upgrade backup before the first 1.1.2 or 1.4.3 launch.

The exact 1.1.2 visibility-counter vertex shader needs a targeted lowering: each passing vertex atomically adds one instead of using a subgroup ballot/reduction. The original atomic side effect is retained, and the code SHA-256 limits this transformation to the analysed shader. This is not a general subgroup fallback. The native prebuilt translator is rebuilt from the checked-in source and has an updated SHA-256.

The 1.4.3 compiler also emits packed RELR relocations, GNU-hash-only SDK modules, and additional x18/atomic instruction windows. Its runtime uses a separately guarded frame/output patch table, the new SDK's logical permissions and locks, and REV15 audio wire layouts with floating biquads. Its instance-based visibility counter has a separate exact-hash lowering; both old/new variants pass 96 native GPU cases. The new EASU shader is separately checked before admitting it to the manual MetalFX HDR path. Current validation and remaining gameplay tests are recorded in [the 1.4.3 port notes](v1.4.3-port.md).

<a id="112-validation"></a>
## 1.1.2 validation

The development runtime passed 55,018 rewrite-site checks and 600 negative controls, native loader/boot/resource probes, 96 real GPU vertex-counter cases with an intentionally wrong-count negative control, and basic Mac gameplay. These are scoped checks, not full-game or performance acceptance.

The package importer also fixes the ticket signature-type byte order: real Switch ticket headers encode `0x10004` as `04 00 01 00`. The old synthetic fixtures mirrored an incorrect big-endian reader; they did not establish retail-ticket compatibility. Tests now use the real header order and reject the reversed header.

Automated container/update tests cover update selection, unsupported version hashes, private ticket handoff and cleanup, and a bundle containing only the selected version. The synthetic merge helper verifies plumbing only; real BKTR cryptography is checked by the local full build.

Local packaging run (2026-10-17, M4 Mac): base XCI + the actual 1.1.2 NSP completed BKTR integrity verification and produced 304,471 indexed files. Merged SHA-256: `d98fde0833a57378f3436275f641a920a29e95500d5667e1ac2ee9ba32f30c43`. The output bundle contains only v112 code/data, and its game executable matches the compiled v112 binary. Temporary NCAs and ticket files were removed. The prebuilt translator's SPIR-V/MSL bytes and reflection JSON match the development shader used by the 96-case GPU probe. The 16 automated tests passed; both launcher versions passed `--check`, and deliberately wrong bundle version metadata was rejected.

Build evidence remains local at `<work>/logs/v112-*`; game images, keys, saves and generated game bytes are not committed. This packaging run does not claim a fresh full gameplay test of the packaged App or an independent retail base-NSP test.

The complete update build and actual packaged launcher `--check` passed (`ok: game version 1.1.2`), taking about 11 minutes with base data cached; 153 seconds were the macOS first-launch scan. Both versions passed 4-thread / 4,000-call native loader probes and rejected altered images. The new native RomFS probe matched independent reads of 65 files, verified three overlays and rejected a corrupted index. No additional GPU/gameplay run was made for this packaging check.

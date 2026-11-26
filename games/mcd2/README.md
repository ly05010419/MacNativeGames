# Minecraft Dungeons 2 — experimental macOS native port

Apple Silicon runtime for title `0100A7C01B792000`, update `0100A7C01B792800` (1.1.3.0).
**Experimental, not gameplay-validated:** startup reaches Metal command replay
and swapchain initialization. Direct compute dispatches with a zero group count
are now treated as NVN no-ops (instead of causing a Metal validation assertion).
The remaining known shader blocker is dynamic texture-buffer/sampler array
reflection and binding. Its generated indices address descriptor-pool IDs, not
ordinary texture slots. The active prebuilt translator still declares 32-element
arrays and clamps those IDs; the isolated candidate removes that fixed length.
Accepting array JSON alone would not provide correct binding.
This path remains unsupported rather than silently binding dummy resources.
Full rendering/gameplay and final packaging/signature/data-layout validation are
still pending; development runtime changes have not been installed into the app.

## Descriptor-array development checks

The synthetic `tests/dungeons_descriptor_array_gpu_probe.m` fixture passes on
Metal 4 with API Validation: texture-pool IDs `7,31,32,63,95,127`, sampler IDs
`0,1,31,32,47,63`, 206 contiguous argument slots (texture array at ID 12),
raw resource IDs compared byte-for-byte with `MTLArgumentEncoder`, descriptor
replacement, and changed buffer contents. A separate old-`min(ID,31)` shader
must produce incorrect results, verifying that the fixture detects truncation.
This is a **capability test**, not production descriptor-registry/cache or game
rendering validation. Run explicitly on an idle GPU; it acquires the production
game slot as `claude` and never preempts a user's game:

```sh
DUNGEONS_DESCRIPTOR_GPU=1 MTL_DEBUG_LAYER=1 /path/to/python \
  -m unittest discover -s tests -p test_dungeons_descriptors.py -v
```

A bounded no-submit startup diagnostic observed **created** texture/sampler pool
capacities of `524288` / `4096`; these are observations, not hardcoded runtime
answers. The candidate retranslated the retained shader with those capacities
(index bounds `524287` / `4095`), and offline Metal compilation passed with one
unused-variable warning. Three captured runtime translation requests also carried
`texture_pool=524288` / `sampler_pool=4096` from bound replay state. No game draw
was submitted by that diagnostic.

A second synthetic Metal 4 fixture uses those capacities and `528398` argument
slots, including texture IDs `65535`, `65536`, `524287` and sampler ID `4095`.
GPU readback, official-encoder/raw-RID layout comparison, descriptor replacements,
content changes and the old-clamp negative control pass. Enable it with
`DUNGEONS_DESCRIPTOR_GPU=1 DUNGEONS_DESCRIPTOR_LARGE_GPU=1`. It still does not
exercise production descriptor lookup, GPU-written handles or game rendering.
Production argument IDs now use 32 bits, separately from the 96 binding-record
limit. A checked range planner covers array extents, overlaps, overflow and queried
pool-length metadata. The shader loader permits raw RID layout only for dense
ranges with matching official Metal encoder byte length; sparse IDs remain
unsupported rather than assuming `8 * ID` offsets. This does not enable arrays:
the scalar replay parser and encoder still enforce their explicit limits.

The production argument arena now sizes an allocation for each request (ordinary
requests retain 4 MiB blocks). Oversized requests no longer receive a too-small
block; reuse checks capacity as well as GPU completion and existing pending-CPU
reference protection. CPU tests compile the actual allocator/reuse code, and the
production driver GPU probe reads both sides of the 4 MiB boundary and the
`528398`-slot extent's tail in two submissions. These are allocator/layout checks,
not game-array binding or rendering validation. Current validation: 31 Dungeons
regression tests pass with both Metal fixtures and offline translator tests enabled;
the production driver probe passes 117 checks, and the UBO snapshot/lifetime probe
passes 16408 checks with Metal API validation. Logs are under
`<work>/logs/descriptor-argument-layout-final-*`. The runtime build succeeds with
64 existing warnings in `nvn_mtl.m`; this is not a warning-free build.

Production reflection now rejects overlapping/out-of-range scalar argument IDs
and rejects array declarations explicitly. The texture-slot cache checks the
argument-slot extent and encoded byte length, rather than the number of binding
records. Production array declarations still fail closed. Pool-length queries
and Direct-array reflection are implemented in the isolated MCD2 translator
candidate; runtime dependency matching checks queried lengths. Pool-binding commands
now update independent texture/sampler domain copies in replay state, including
prediction/snapshot restore. Translation answers and graphics/compute variant
selection include their capacities. This is state/specialization plumbing, not
multi-pool descriptor resolution: registrations still use global ID tables.
Cache-index loading excludes legacy arrays without length provenance and checks
index/reflection length agreement without rewriting cache assets. Pool-scoped
resource lookup, resource preparation, buffer-texture synchronization,
residency/lifetime and hazards still require
integration. Production array reflection remains rejected.

With user authorization, the official .NET 10.0.403 ARM64 SDK was downloaded,
SHA-512 verified and installed only under `<work>/src/dotnet-sdk`. The NativeAOT
candidate is built under `<work>/build/shader-service-candidate` (with existing
AOT trimming warnings). Retranslating a locally retained failing shader verifies
that missing lengths fail and explicit lengths change array counts and index
bounds. The small test lengths are synthetic; the separate large case uses the
created capacities observed above. No active service,
repository prebuilt, other game's translator or signed App was replaced.

The source-level translator tests use the prepared external SDK/worktree and do
not install or download tools:

```sh
DUNGEONS_TRANSLATOR_WORK=/path/to/MCD2_build /path/to/python \
  -m unittest discover -s tests -p test_dungeons_translator.py -v
```

Optionally set `DUNGEONS_TRANSLATOR_REQUEST` to a locally retained service request
for real-shader retranslation. Shader code and request data are not test assets.

## Build and Package

Run the standalone build inside `games/mcd2` using your own dumps, `prod.keys` and Switch firmware shared fonts (`scripts/extract_code.py`, `scripts/merge_romfs.py`, `scripts/extract_shared_fonts.py`, `scripts/prepare_modules.py` and the rewriter into `MCD2_build/`), then build and package:

```sh
cd games/mcd2
make prepare runtime app WORK=MCD2_build \
  GAME="/path/to/update.nsp" KEYS="/path/to/prod.keys" \
  APP="Minecraft Dungeons 2.app"
```

All intermediates stay under `MCD2_build/`. After packaging, the main RomFS exists only inside the signed app data bundle; the work directory uses a symlink. No internal-disk data staging is used.

User data: `~/Library/Application Support/MacNativeGames/MCD2`.
Log: `~/Library/Logs/MacNativeGames/mcd2.log`.
Networking is offline; host CSRNG uses macOS secure randomness. TLS and online
multiplayer are not implemented.

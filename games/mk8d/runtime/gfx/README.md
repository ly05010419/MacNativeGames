# dm/gfx — graphics layer (NVN → Metal)

The Switch version of the game draws with NVN (the Switch's native graphics interface). The graphics layer reimplements this interface on top of Apple's Metal: every NVN call the game makes lands here, is recorded as a command, and is replayed as Metal commands at submit time. The game's shaders are not translated at run time; they are translated offline, once, on a computer, into Metal shader libraries (when the computer has no Apple Metal compiler, the final compile step moves to the phone; see below).

For the build steps of the whole project see [README.md](../../README.md) and [docs/development.md](../../docs/development.md) (the tools they need: Xcode, xcodegen, .NET SDK, zstd, and the Python packages lz4 / capstone / fonttools; spirv-cross is the in-repo build `tools/spirv_cross_bin`); this page covers only the graphics layer's own matters.

## How it works

### Taking over the NVN interface

- The game's main program imports only one NVN symbol, `nvnBootstrapLoader`; all other NVN functions are obtained through it by name. The host forcibly takes over this import (`nvn/hle_nvn.c`), and not a single instruction of the original graphics driver is executed.
- The game fetches the addresses of 523 names in total (`nvn/names.txt`). Each name has a thunk (`nvn/nvn_thunks.S`, a generated file): it first increments that function's call count and then jumps to the implementation.
- The functions games call have implementations, according to NVN semantics or empty (`g_nops` in `hle_nvn.c`); the rest are connected to a red-reporting stub that, when called, prints `[RED] NVN unimplemented function called: <name>` in the log.
- Every implementation gets a compile-time signature check against `nvn/nvn_protos.h` (a generated file). The calling conventions of Apple arm64 and the Switch differ for integer parameters narrower than 32 bits, so on the host side `NVNboolean` parameters are always received as `uint32_t` and only the low 8 bits are examined.

### Two layers: record and replay

- **Interface layer `nvn/hle_nvn.c`**: host-side objects are stored in place in the memory the game allocates for NVN objects; the layouts are in `nvn/nvn_obj.h`, and each has a static assertion guaranteeing it does not exceed NVN's reserved size. Commands are recorded into the command memory provided by the game, in the record format of `nvn_obj.h`.
- **Replay layer `nvn/nvn_mtl.m`**: at submit time it replays the record stream as Metal commands.
  - Memory pools are created as zero-copy `MTLBuffer`s, with the GPU address equal to the CPU address, so only unified-memory devices are supported.
  - The visible effects of "GPU completion" (sync objects becoming ready, events writing their value, counters) happen only after the Metal command buffer has actually completed.
  - Pipeline states and depth-stencil states are cached by state hash; at startup, shaders and pipelines are pre-built according to the warm-up manifests (the bundled seed `pso_manifest.tsv`, then this device's `pso_user.tsv`; see below): with a window, on every core before the game's first frame behind a "Preparing shaders" screen (docs/development.md, "The prepare step"), else in the background. A pipeline that fails to build is cached as failed and reported once with its shader keys.
  - Render pipelines are made through `MTL4Compiler` from a base whose colour state is left unspecialized, one base per pipeline less its colour state, specialized per colour state; the bases are kept in the per-game pipeline archive (`pso_archive/` beside `pso_user.tsv`) and taken from it at the next start (`pso_build`; see docs/development.md, "Pipeline bases and the pipeline archive"). A draw whose pipeline is not built yet draws with its shader pair's pulling pipeline (no vertex descriptor) while its own is built in the background (docs/development.md, "Hybrid pipelines").
  - The game's packaged textures are in Tegra block-linear layout and are deswizzled before upload (see `tex/README.md`).
  - Presentation: the game's swapchain texture is drawn to a `CAMetalLayer`, and presentation timing is scheduled by the pacer in `nvn/nvn_pace.h`.

For which Metal object each NVN object and command lands on, see [nvn_metal_mapping.md](nvn_metal_mapping.md).

### Shader translation chain

The game's shaders are GPU machine code for the Tegra X1 (the Maxwell instruction set). The translation from this machine code to Metal Shading Language is not done on the phone; all of it happens offline:

```
Original Switch shader package
  → tools/dmsh_xlate        produces SPIR-V with Ryujinx's shader translation library (a C# tool)
  → spirv-cross             converts SPIR-V to Metal Shading Language (MSL); built at packaging into tools/spirv_cross_bin
  → xcrun metal / metallib  compiles into Metal shader libraries
```

The whole chain is driven by `tools/dmsh_build.py`; `scripts/build_ipa_from_nsp.py` runs it as part of packing an IPA. Key points:

- **Key**: each shader is keyed by the first 16 hexadecimal digits of the sha256 of its control section (the first 0x880 bytes, or the section's own length at control +0x20 when that is shorter, as in SDK 10's 0x850-byte sections) followed by its code area (many of Mario's shaders share a control section and differ in code). At run time the game hands both to `nvnProgramSetShaders`, and the replay layer computes the same key to find the library.
- **Build outputs**: `<key>_b.metallib` (the shader library), `<key>_b.json` (reflection used by replay: each binding's kind, slot number, index within the argument buffer and so on), `<key>_b.metal` (the MSL source). Some fragment shaders additionally have `<key>_bf.metallib`, a "readable tile" version used for pass merging (see the file header of `tools/dmsh_fetch.py`). `_b_build.json` in the directory records the success or failure of each key in this run.
- **`dm/games/<id>/shader_map.tsv`**: the shader map, one row per shader variant. At run time only its "key → stage (vertex / fragment / compute)" is used.
- **`dm/games/<id>/shader_facts.tsv`**: the answer table for translation queries. The translator needs to know some facts that cannot be read from the machine code (the dimension type of a texture, image formats, the component types of vertex attributes and fragment outputs, a compute shader's thread group size and shared memory byte count), and this table gives the answers by key. The table ships with the repository, and building the shader libraries only reads it; the table is maintained by `tools/dmsh_facts.py`, whose file header has the details.
- **Translation chain version and the `b-<V>/` directory**: the version V is a string of 12 hexadecimal digits, obtained by hashing together the contents of the source files involved in the translation chain, the translation options and the tool fingerprints (`tools/dmsh_chain.lock`); `tools/dmsh_chain.py` computes it. Shader libraries are written into `<output directory>/b-<V>/`, and every reflection json also records V. When the host is compiled it takes V from `nvn/dmsh_chain.h`, one version for every game, loads only from `b-<V>/`, and checks the version in each json one by one; on a mismatch it reports red and does not load. So after changing any file on the chain, or changing a tool, the libraries must be rebuilt and the host recompiled. A profile's inputs (its shader tables and `shader_*` / `nvn_*` keys) are not part of V: they are in each shader's cache key, so a change rebuilds the shaders it affects, and a full build stamps the directory with the game and their digest (`b-<V>/inputs.tsv`, `dmsh_chain.py inputs`). A full build starts from no shader outputs and a partial one from none of the keys it builds, so a directory holds only what its last build made; it refuses a directory stamped for another game, so each game needs its own output directory.
  - `dmsh_chain.py`: prints V.
  - `dmsh_chain.py --check-tools`: checks that the local tools match the lock file.
  - `dmsh_chain.py --update-lock`: rewrites the lock file according to the local tools.
  - `dmsh_chain.py --header`: rewrites `nvn/dmsh_chain.h` (commit it with the chain change; `dm/tools/tests/test_nsp_chain.py` fails on a stale one).
  - `dmsh_chain.py --inputs`: prints the digest of the current profile's inputs (`DM_GAME`), as a full build stamps it.
  - The converter is `tools/spirv_cross_bin/spirv-cross`, which `scripts/build_mk8d.py` builds at packaging from a pinned upstream release; the lock pins its `--revision`. `SPIRV_CROSS=` selects another executable, which must match the lock for NVN builds. Changing the pin requires the new release in `build_mk8d.py` (`SPIRV_CROSS`, `SPIRV_CROSS_REVISION`), `dmsh_chain.py --update-lock` and `--header`. Moving to another Xcode also requires updating the documented tested version.
  - `tools/own_compilers.py <dmsh_build.py arguments>` requires the translator, SPIRV-Cross and both Metal compilers to match the lock. Another Metal compiler stops the build with installation instructions; `DM_ALLOW_OTHER_METAL=1` permits it and prints the actual version. Issue #242 reported corruption with an untested compiler, but the compiler's role remains unconfirmed. The override changes cache keys without changing the lock or chain version.
- **`dm/games/<id>/c1_full.tsv`**: the index list for the shaders' built-in constant section (c1), generated by `tools/c1_guard.py` scanning the translated MSL; it lists the shader keys whose "run-time index could not be proven bounded". An index counts as proven only when interval arithmetic over the whole expression (every variable followed to its single definition; literals, `& K`, `% K`, shifts, `min`, `max`, `clamp` and casts bound it) keeps it non-negative and inside the span a shared bank gives that shader: its c1 data rounded up to 256 bytes plus the 256-byte zero gap. Every stage counts (`vp_`, `fp_`, `cp_` …). Its first line records the `b-<V>` and the inputs stamp of the library it was made from. For the shaders on the list the replay layer allocates a separate whole 64 KiB buffer for c1; when the list is missing or does not name the loaded library's version and stamp, it reports red and conservatively treats all shaders this way. The list is not committed (git ignores it): each checkout makes it from its own build's Mac outputs (a full `dmsh_build.py` run does).
- **`dm/games/<id>/cb_extent.tsv`**: how far into each uniform slot (cbuf binding) every shader can read, generated by `tools/cb_extent.py` from the translated MSL with `c1_guard.py`'s interval arithmetic: per shader key, `reg:bytes` for each slot whose reads are all proven to stay inside its first bytes, and `s<reg>` for each storage slot the shader never writes (SPIRV-Cross types every storage buffer Ryujinx emits as writable, so the host counts the others as written). When the game updates part of a bound uniform block inside a render pass, the replay layer copies only that much of the binding to give the draws after the update their data without ending the pass (`nvn_mtl.m` `ubp_get`; Breath of the Wild binds a 64 KiB block and its vertex shaders read at most 544 bytes of it). Its first line records the `b-<V>` and inputs stamp, like `c1_full.tsv`'s; when it is missing or stale, bindings are copied whole (up to 64 KiB). Like `c1_full.tsv`, it is not committed, and a full `dmsh_build.py` run makes it.
- **`dm/games/<id>/shader_pairs.tsv` and `pso_pairs.tsv`**: the (vertex key, pixel key) of every BNSH variation (Unity: of every program) and agl BAHS program variation, written by `tools/shader_split.py`. Metal only builds a pipeline when every pixel shader input is a vertex shader output, while Maxwell gives an unwritten attribute a default; `dmsh_build.py` therefore adds the outputs a vertex shader's partners read but it doesn't write, set to (0, 0, 0, 1) (`vs_pad`). The profile tables are part of its inputs digest and the per-shader cache keys. A profile without `pso_pairs.tsv` takes the generated table beside the shader package.
- **Several game versions**: `shader_split.py` takes comma-separated RomFS and `main` lists (base game first) and writes the union; the map, facts and pairs tables currently cover Mario v0 and 1.2.1.
- **`dm/games/<id>/pso_manifest.tsv`**: the warm-up seed manifest bundled with the app: shaders (`S`), render pipelines (`R`, the hex of `pso_skey` in `nvn/nvn_pso_skey.h`) and compute pipelines (`C`) to pre-build at startup. The row order is the pre-build order (first use); do not sort it. Its companion is `pso_user.tsv`, which the host appends next to the save directory whenever it has to build something on the spot, and which is pre-built after the seed at the next launch. Both name shaders by key and describe pipeline settings only, so a list recorded on one device works on any other; lines a build cannot use are skipped. Recorded lines can be appended to the seed below its header (currently empty).
- **Shaders embedded in the program itself**: the main program additionally contains 4 compute shaders that are not in the shader package; they were hand-written as MSL from the disassembly and are in `nvn/nvn_builtin.h`.

Binding conventions followed during translation (the NVN driver's layout, the same in every game: `tools/dmsh_build.py`, filled at run time by `nvn/nvn_mtl.m`):

- uniform buffer r is constant section `c[3 + r]` in the machine code;
- the handle of combined texture r (`nvnCommandBufferBindTexture`) is at `c[2][0x20 + 8r]`, of image r at `c[2][0x120 + 8r]`, of separate texture r at `c[2][0x168 + 8r]` (the host's texture slot 128 + r) and of separate sampler r at `c[2][0x568 + 8r]`;
- the address of storage buffer r is at `c[0][base + 16r]`, with base: vertex 0x110, compute 0x310, fragment 0x510;
- the window origin is at the top left and the depth range is 0 to 1, the same as Metal, so nothing is flipped during translation.

## Directory structure

| Location | Contents |
|---|---|
| `nvn/hle_nvn.c` | Interface layer: NVN takeover, object implementations, command recording, presentation pacing |
| `nvn/nvn_mtl.m`, `nvn/nvn_mtl.h` | Replay layer: Metal device, memory pools, textures, shader loading, pipelines, drawing, presentation |
| `nvn/nvn_obj.h` | Host-side object layouts and the command record format, shared by both layers |
| `nvn/nvn_entry.S`, `nvn/nvn_thunks.S` | Entries for the unimplemented stub and the timing wrapper, per-function thunks |
| `nvn/nvn_tex.c`, `nvn/nvn_tex.h` | Storage size, alignment and per-level offsets of block-linear textures (ported from deko3d) |
| `nvn/nvn_dirty.c`, `nvn/nvn_dirty.h` | Tracks dirty pages of game memory with write protection, to decide whether texture contents have changed and need re-uploading |
| `nvn/nvn_refl.m`, `nvn/nvn_refl.h` | Converts shader reflection json into fixed-length C arrays, so that drawing only walks arrays |
| `nvn/nvn_hmap.h`, `nvn/nvn_cover.h`, `nvn/nvn_pace.h`, `nvn/nvn_pso_skey.h`, `nvn/nvn_primitive.h` | Hash table, geometric proof of full-coverage draws, presentation pacer, pipeline manifest key, indexed triangle fans and quads as triangle lists (primitive restart kept) |
| `nvn/nvn_builtin.h` | Hand-written MSL for the 4 compute shaders embedded in the main program |
| `nvn/include/nvn.h` | NVN interface declarations (third party; for the license see the file header) |
| `nvn/names.txt` | The NVN API name list. The per-game tables described above live in the game profile `dm/games/<id>/` (named after the base game's title ID; tools pick it with `DM_GAME`, make with `GAME`; there is no default game) |
| `tex/` | Texture format mapping, deswizzling, BC decompression, see [tex/README.md](tex/README.md) |
| `vk/hle_vk.m` | Vulkan on Metal, for games that render with Vulkan (Hades, Super Mario Galaxy); their SPIR-V shaders are translated by `tools/vk_shaders.py` |
| `gl/hle_gl.m` | OpenGL through EGL on Metal, for games that render with NVIDIA's GL driver (Digimon Story Cyber Sleuth: Complete Edition; Balatro, LÖVE on its core-profile paths); their GLSL sources are translated by `tools/gl_shaders.py` (glslang, then SPIRV-Cross) |
| `metal/` | A standalone minimal Metal offscreen example, not compiled into the host, see [metal/README.md](metal/README.md) |
| `tools/` | Offline tools, see below |
| `tests/` | Unit tests |
| `hle_vi.c`, `hle_nv.c` | Glue layer for display service and graphics driver initialization: returns host static objects or does nothing |

The scripts in `tools/`; usage is written in each file's header:

| Script | Purpose |
|---|---|
| `dmsh_xlate/` | The shader translation tool (C#), which references the translation library in `dm/third_party/ryujinx` |
| `dmsh_build.py` | Drives the whole translation chain to produce the shader libraries; when the environment variable `IOSOUT=directory` is set, it also produces the iOS version |
| `dmsh_chain.py`, `dmsh_chain.lock` | Translation chain version and tool fingerprints |
| `dmsh_facts.py` | Reads the answer table `shader_facts.tsv` (which `shader_split.py` writes) |
| `dmsh_fetch.py` | Generates the "readable tile" versions of fragment shaders |
| `c1_guard.py` | Generates `c1_full.tsv` |
| `cb_extent.py` | Generates `cb_extent.tsv` |
| `dmsh_dryrun.py` | Dry-run conversion of the whole library, measuring only "can it be converted" |
| `gen_protos.py` | Generates `nvn_protos.h` / the thunks |
| `tbdr_budget.py`, `tbdr_load_dead.py`, `tbdr_store_dead.py`, `tbdr_pass_merge.py` | Read the `DM_NVN_PASSDUMP` log and analyze the load / store volume of each render pass's attachments per frame |
| `passgpu_top.py` | Reads the `DM_NVN_PASSGPU` output and lists GPU time by pass |

## Building

The graphics layer has no separate library. `dm/host/Makefile` compiles `dm/gfx/*.c` and the source files under `dm/gfx/nvn/` and `dm/gfx/tex/` into the host `dmhost` by wildcard; the iOS App is built by `dm/ios/tools/build_ipa.sh`, which also copies the map, `pso_manifest.tsv`, `c1_full.tsv` and the `b-<V>/` shader libraries into the App. `tests/` and `metal/` do not go into the host.

The shader libraries are generated by `tools/dmsh_build.py` (the NSP script runs it; for building by hand see [docs/development.md](../../docs/development.md)):

- the Mac version is in `$DM_ROOT/build/cache/metal_shader_cache/b-<V>/`;
- the iOS version is in `$DM_ROOT/build/ios/dmsh_b/b-<V>/`.

After changing a file on the translation chain, the order for rebuilding manually is: `dmsh_chain.py --header` (once, for every game) → `dmsh_build.py` (a full build also makes `c1_full.tsv` and `cb_extent.tsv`, local tables, not committed) → recompile the host.

The per-shader cache key in `dmsh_build.py` (`shader_key`) covers the profile's `shader_*` rows and the shader's pairs, and a paired vertex shader's key also takes its pixel shaders' keys (`pair_keyed`), since `vs_pad` reads their MSL. Before, these were only in the chain version, so a build after changing only them hit the old per-shader entries and filed them under the new version; now the chain version leaves them out altogether. `$DM_ROOT` defaults to `~/dm-work`.

## Tests

```sh
make -C dm/gfx/tests run        # graphics layer unit tests
make -C dm/gfx/tex/tests run    # texture submodule unit tests; the real-data items need texture samples from both the Switch and PC versions, see tex/README.md
make -C dm/gfx/metal run        # offscreen example self-test, see metal/README.md
```

The tests in `dm/gfx/tests` compile the production code in directly and need a Metal device on the local machine; a few of them run under `MTL_DEBUG_LAYER=1` (the Metal validation layer). Most tests do not read the workspace; the exceptions are `t_refl` and `t_rt_range_ro`, which read the translated shader reflection json under `$DM_ROOT/build/cache/metal_shader_cache`, and `test_tbdr_budget.py`, which creates a temporary directory under `$DM_ROOT/run/gfx`. Main contents:

| Test | What it checks |
|---|---|
| `t_nvn` | Interface layer: fetches addresses from `nvnBootstrapLoader` the way the game does, all the way through to presentation |
| `t_builtin` | The 4 embedded compute shaders |
| `t_refl`, `t_hmap`, `t_cover`, `t_pace`, `t_pso_skey`, `t_dirty_slots` | Reflection parsing, hash table, full-coverage proof, pacer, pipeline manifest key, dirty page tracking |
| `t_pso_warm` | Historical manifest normalization and deduplication, draw-time cache reuse and GPU pixels for normal and merged passes, through MTLDevice and Metal 4 |
| `t_deswz_gpu` | GPU deswizzling |
| `t_present_crop` | Presentation cropping |
| `t_mip_extent` | Effective size when rendering to a mip level other than level 0 |
| `t_fuse_check`, `t_reorder_chain`, `t_defer_chain`, `t_zskip`, `t_xv`, `t_ub`, `t_pfv`, `t_rt_range`, `t_rt_range_shape`, `t_rt_range_ro`, `t_rt_depth_ro` | Compile the replay layer `nvn_mtl.m` in directly and check the various optimization and diagnostic logic inside the replay layer |
| `test_tbdr_budget.py` | The parsing in `tools/tbdr_budget.py` |

## Common run-time switches

All are environment variables. The iOS App sets `DM_NVN_SHMAP` and `DM_MSL_DIR` itself at startup.

Resource locations:

| Switch | Effect |
|---|---|
| `DM_NVN_SHMAP=path` | The shader map. The default is `../games/<id>/shader_map.tsv` relative to the executable's directory; `c1_full.tsv` and `pso_manifest.tsv` are taken from the same directory |
| `DM_MSL_DIR=directory` | The parent directory of the shader libraries; the host loads from `b-<V>/` beneath it. When running the host on a Mac, set it to `$DM_ROOT/build/cache/metal_shader_cache` |
| `DM_PSO_MANIFEST=path` or `off` | Use a different warm-up manifest, or turn warm-up off |
| `DM_PSO_PREPARE=0` or `1` | `0`: the warm-up runs in the background while the game starts, as without a window; `1`: the game waits for it before its first frame also without a window (`DM_PSO_PREPARE_THREADS=<n>` workers; docs/development.md, "The prepare step") |
| `DM_PSO_RECORD=path` | Appends the shaders and pipelines used in this run to a file, for re-recording the warm-up manifest |
| `DM_PSO_USER=path` | Where to keep the per-device list of shaders and pipelines built on the spot (default: `pso_user.tsv` in the parent of `DM_SAVE`) |
| `DM_PSO_ARCHIVE=folder` or `off` | Where to keep the per-game pipeline archive of pipeline bases (default: `pso_archive/` beside `pso_user.tsv`), or no archive |
| `DM_PSO_MTL4=0` / `=1` | `0`: build render pipelines through the `MTLDevice` as before, with no unspecialized bases and no archive (the Mac's default is on). `1`: turn them on on iOS, where they are off by default |
| `DM_PSO_HYBRID=0` | A draw whose pipeline is not built yet waits for it, instead of drawing with the pair's pulling pipeline meanwhile (docs/development.md, "Hybrid pipelines") |
| `DM_PSO_PREBUILD=0` or `tool` | No prebuild of the pulling bases into the archive (in the prepare step, or in the background without one); `tool`: run the prebuild alone and exit (`DM_PSO_PREBUILD_DEPTH=zero`, `DM_PSO_PREBUILD_THREADS=<n>`) |
| `DM_PSO_PAIRS=path` | The prebuild's second pair table (default: `pso_pairs.tsv` beside the shader map) |
| `DM_PSO_BUNDLE=folder` or `off` | The pipeline archive made offline (`dm/gfx/tools/pso_offline.py`): `<GPU architecture>.mtl4a` and `prebuilt.tsv`, read only; when it covers the shader pairs the device builds no prebuild bases. The iOS app sets it to its `pso/` folder when it has one (docs/development.md, "The pipeline archive made offline") |
| `DM_PSO_WAITLOG=1` | Log each draw that waited more than 20 ms for its pipeline, with why no stand-in served it |
| `DM_PSO_PULL_ALL=1` | Diagnostic: every draw that has a pulling pipeline draws with it |

Logging and capture:

| Switch | Effect |
|---|---|
| `DM_NVN_LOG=1` | Logs the creation of NVN objects |
| `DM_NVN_FRAMES=N` | Prints the call table frame by frame for the first N frames (default 3) |
| `DM_NVN_TSV=path` | Writes the per-frame call table and timings as TSV; adding `DM_NVN_TSV_CALLS=0` writes only the timing rows |
| `DM_SLOWFRAME=1` | Writes the timing breakdown into the log only for the frames that dropped |
| `DM_NVN_SHOT=frame[,frame…]:path` | Saves the swapchain texture as an image at the specified frames; `%d` in the path is replaced by the frame number |
| `DM_NVN_SHOT_OUT=WxH` | Makes the screenshot above capture the picture after upscaling to this resolution instead |
| `DM_NVN_TEXDUMP=frame:path` | Saves each color render target used in that frame as an image |
| `DM_NVN_PASSDUMP=frame` or `start-end` | Writes each render pass's attachments, samples and presentation for these frames to standard error entry by entry, for analysis by `tools/tbdr_*.py`; additionally setting `DM_NVN_PASSDUMP_ALL` logs every draw |
| `DM_NVN_PASSGPU=file` | Per-pass GPU time (requires device support for counter sampling at stage boundaries) |
| `DM_NVN_PROF=1` | Wraps the implemented NVN functions in a timing wrapper and collects the time each function takes during recording |

Comparison and testing:

| Switch | Effect |
|---|---|
| `DM_NVN_TBDR=1` | Turns on the pass optimizations of the replay layer (store / load elision, pass merging). They are off by default: they infer which textures a draw reads from its bound slots, which bindless textures defeat ([docs/agents/04-graphics.md](../../docs/agents/04-graphics.md) section 4) |
| `DM_NVN_UNWRITTEN_DC=0` | With `DM_NVN_TBDR=1`, turns off the single item "attachments not written in this pass are not stored" |
| `DM_NVN_FUSE_CHECK=1` | Same-frame check of pass merging; logs the comparison result every time |
| `DM_NVN_CS_HOIST=0` | A compute dispatch between the draws of a pass ends it (by default it runs ahead of the pass when that changes nothing the draws before it read: [reference](../../docs/reference/rendering.md#compute-dispatches-between-the-draws-of-a-pass-run-ahead-of-it)) |
| `DM_NVN_SLICE_MERGE=0` | Passes into other layers of one array or cube texture level stay apart (by default they run as one layered pass: [reference](../../docs/reference/rendering.md#passes-into-other-layers-of-one-texture-level-run-as-one-layered-pass)) |
| `DM_FPS=30` or `60` | Pins the target frame rate |
| `DM_NVN_BARRIER=game`, `DM_NVN_CUT` | Pass ordering and segment commits: see [docs/agents/04-graphics.md](../../docs/agents/04-graphics.md) section 7 |
| `DM_PRESENT_HZ=N` | The frequency of presentation pacing (default 60); 0 means no frame limit and is for performance profiling only |
| `DM_PRESENT_UPSCALE=metalfx` or `off` | How the present scales a picture smaller than its rectangle on the screen: `metalfx` through MetalFX's spatial scaler, `off` (the default) bilinearly. An option on both platforms (the iOS launcher's Upscaling; `env.txt` on the Mac); rendering at the screen's resolution stays the reference for sharpness ([reference](../../docs/reference/rendering.md#the-present-upscales-through-metalfx)) |

## Known limitations

The following paths are not implemented and report red in the log when reached (`RED_ONCE` in `nvn/nvn_mtl.m`):

- clears in the middle of a render pass, and color clears with a partial mask or partial scissor;
- indexed quad primitives (8-bit indices are widened to 16-bit at replay);
- some forms of texture-to-texture copy and texture clear.

Functions listed in `g_nops`, such as `MultiDraw*IndirectCount` and `SetDepthBounds`, are empty implementations: calling them neither reports red nor has any effect. `Downsample` / `DownsampleTextureView` are recorded as texture copies with a host flag (`DM_COPY_RESOLVE`) and replayed as a Metal multisample resolve.

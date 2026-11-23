# Changelog: The Witcher 3: Wild Hunt on the Zelda-line runtime

Each entry says what was found, what was done and how it was checked. Version 4.04b (update 0100E67012924800 of the
American release 0100E67012924000: main build ID b151a224…, sdk d5a1029a…; `assets/expected-4.04b.json`).

## 2026-11-23 — from nothing to the first in-engine scene (Mac)

- **Inputs**: the player's base NSP (3.7) and 4.04b update; the update's ExeFS (games/pla `extract_exefs.py`), the merged
  RomFS (games/eow `merge_romfs.py`: 21.4 GB, 795 files, every IVFC block verified). Modules main and sdk only.
- **The rewrite** (`third_party/d2r/patch_nso.py`): two dispatch forms of this compiler. A plain move scheduled between the
  `add x18` and the `br x18` (main+0x135eaec); a table base hoisted out of its loop 50 instructions before the dispatch
  (main+0x13b6f94): the search window is 96 instructions. An unknown form now prints the instructions before it.
- **The runtime**: Persona 5 Royal's (`GAME_W3`), with
  - the NPDM's 4 MB system resource (GetInfo 16): `nn::os` then enables virtual address memory, which a constructor of main
    uses at once (0 left the VammManager uninitialised: a null read in AllocateAddressRegion);
  - `EnableGlobalFileDataCache` / `DisableGlobalFileDataCache` (no-ops, as games/xc3);
  - `nvnQueueBuilderSetQueuePriority` (no-op, as games/totk);
  - nn::hid `BindNpadStyleSetUpdateEvent`, `GetNpadJoyHoldType` (as games/eow), `InitializeGesture`, `GetGestureStates`
    (none, as games/ori); nn::aoc `CheckAddOnContentMountStatus` (mounted: the host's add-on content is files);
  - `nn::account::OpenUser` (shared `account.c`, now for Mario Kart 8 Deluxe and this game);
  - `nn::time::StandardUserSystemClock::now`, `nn::vi::ListFrameHistory` (none, as games/xc3);
  - the shared `audio_out.c`: the audio service never reads AudioOutBuffer's data_offset and this game leaves it
    uninitialised, so the host no longer reads it either;
  - the NVN control section copy reads the section's own size (+0x20) instead of 0x880: one ends at the last byte of an
    address region (the read faulted in `nvnProgramSetShaders`);
  - the crash handler waits up to 10 s for the GPU before exiting (a crash mid-frame had left the GPU busy) and names the
    host function the crash was in.
- **Shaders**: Tears of the Kingdom's translator (`prebuilt` links it; chain `w3-native-v1`). The reflection parser reads
  its packed-bank names for textures and samplers whose handles sit in user constant buffers (`fp_t_cb720898_2000405CSmplr`:
  sampler handle in c10 word 1). The translator and the host judge driver-data reads per word read (`word_offsets`), not
  per vec4: `c2[22].z` is texture handle slot 0 (0x168), next to the image handles; the old rule refused the shader.
- **Checked**: the Saber Interactive logo, the title, the main menu (both expansions), a new game's comic intro, its CGI
  movie and the first in-engine scene with Chinese subtitles; no GPU hang, the GPU idle after every run.
- **The black room**: the game writes render targets as images (compute kernels: its four 1x1 R32F exposure values,
  640x360 R32F / RGBA16F / RGBA8 targets) although they were made without NVN_TEXTURE_FLAGS_IMAGE; the console does not
  check, Metal dropped the writes of textures without ShaderWrite, so exposure and the lit scene stayed black. Every
  uncompressed 2D colour texture of this game is made shader-writable. An image binding to an unwritable texture logs the
  texture's description ([img-unwritable]). Checked: the room, its walls, bed and lights drawn; 30 fps in the scene (frame
  p50 33.33 ms, GPU p50 16 ms).

## 2026-11-23 — gameplay at Kaer Morhen; tessellated terrain as compute (Mac)

- **Gameplay**: the first save (Kaer Morhen, "使用猎魔感官能力寻找卧室钥匙") loads and plays at 30 fps.
- **A tessellated program ended the game** (NVN stage 3, `unsupported NVN stage`): the terrain (mountains, valleys,
  the ground) is drawn with quad tessellation. Metal 4's render encoders have no tessellator (no drawPatches), so a
  tessellated draw now runs as compute (`third_party/d2r_gfx/gfx/nvn/nvn_tess_rt.inc`):
  1. the vertex shader's capture entry (games/totk's geometry capture) writes one 144-word record per vertex;
  2. the control stage runs as a kernel over every patch (Tears of the Kingdom's translator, new `TessService.cs`:
     SPIRV-Cross's multi-patch control kernel, its inputs read from the records);
  3. a tessellator kernel partitions each quad patch from its levels (integer partitioning: the spacing's levels rounded
     up, each edge split by its own level, so neighbouring patches share their edge points exactly);
  4. the evaluation stage runs as an ordinary vertex function over the domain points with the program's fragment shader,
     drawn indirectly (vertex count from the GPU; render stream packet `RS_INDIRECT`).
  The mode comes from the evaluation stage's NVN control section (+0x738 GL_QUADS, +0x73c GL_FRACTIONAL_EVEN, +0x744
  GL_CW); `nvnCommandBufferSetPatchSize` is recorded (OP_PATCH_SIZE). The two stages' interface structs are laid out by
  SPIR-V location on both sides (each declares only what it writes or reads). Found on the way: the capture info block's
  `uint4` is 16-byte aligned in Metal (`tess_capture.pad`; a probe, `W3_TESS_PROBE=<frame>`, showed one record written);
  a tessellation evaluation function's varyings are its `main0_out`; `g_red_once` was 128 entries.
- **Shader-writable textures**: 2D array and 3D colour textures too (a 64x64x5 2D array texture written as an image).
- **Checked** (headless, the Kaer Morhen save): the mountains, valleys, clouds and the tower drawn, no cracks between
  patches; 30 fps (frame p50 33.33 ms, p95 33.5 ms), GPU p50 17 ms (16 ms before tessellation was drawn); about 3,500
  patches a frame through the tessellator; no GPU hang, the GPU idle after the run.
- **Not yet**: triangle and isoline domains, fractional spacing's level blending, indirect tessellated draws; Geralt's
  shirt showed coloured noise once in a window session (not reproduced in four runs since).

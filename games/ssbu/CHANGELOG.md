# Super Smash Bros. Ultimate: changelog

## Status

What works and what does not: [README.md](README.md). This page says what the port needed beyond the host it started
from, why, and how each step was measured.

## Where the folder comes from

`games/ssbu` began as a copy of a Super Mario Odyssey 1.4.1 port that is not in this repository (the repository's own
Odyssey port is `games/odyssey`). That port was itself a copy of `games/totk` with the SDK 7.3.2 branches Breath of the
Wild uses. Comments that name Super Mario Odyssey explain why a piece of code exists; the code is this folder's own copy
and nothing outside `games/ssbu` is read.

## Log

### 2026-10-23: stage 1, code and data

- The 13.0.2 program (SDK 8.2.1) comes from the update; the RomFS is the 1.0.0 card image plus the 13.0.2 update, all six
  IVFC levels verified (17.6 GB). Every input is pinned in `assets/expected-v13.0.2.json` (hashes only).
- The rewriter needed two things Smash's code has and the other games' does not: jump-table bases computed before a loop
  or parked in x30 (a wider search runs only when the old one finds nothing; Mario's and Zelda's rewrites stay
  byte-identical, 6 of 6), and one virtual tail call `br x18` (a site of its own, x17 saved in the red zone).

### 2026-10-23: stage 2, the host for SDK 8.2.1 and the NRO modules

- **NRO modules.** `main` imports 1,045 `lua2cpp::` functions from modules the game loads later. The 98 NRO modules sit
  in data.arc inside zstd frames; `scripts/extract_nros.py` finds them by scanning the frames and keeps only those whose
  SHA-256 is a digest of the game's NRR (no data.arc parser), and `scripts/rewrite_nros.py` rewrites them like `main`.
  The host answers `nn::ro` (`runtime/ro_host.c`: relocation, the RoModule filled as the SDK fills it, DT_INIT), and
  `main`'s imports bind lazily: the PLT hands the GOT slot in x16 (`loader.c`, `lazy_bind.S`).
- **Address space.** The game reserves a 62 GiB region (AllocateAddressRegion). It is an alias region above 2^39; IPC
  addresses in the mirror window get bit 39 back.
- **Services** a fighting game asks for: the oe message event, hid single states, LEDs and the GameCube adapter, friends
  and Mii offline (the default Miis come from the SDK's own Mii library), AudioOut on CoreAudio (Smash's only sound path),
  the cache storage its NACP asks for, thread stacks in .bss.
- **Graphics.** The control section is copied at ProgramSetShaders (Smash frees it right after); the shader translator
  is told the host has no texture gather offsets and no non-constant texture offsets; the window texture is made
  resident before it is presented (Metal 4: a new texture is not resident, the GPU faulted; with presenting skipped the
  faults went to 0).
- **Movie sound.** Vorbis is the movie decoder's type 5, which the movie library sent to Android's MediaCodec: a crash
  after about 3 s. The host decodes it with libvorbis; the library's calls were recorded first (`TOTK_MOVIE_TRACE=1`)
  and the sound proved per second (`SSBU_AUDIO_DUMP`). `TOTK_MOVIE_VORBIS=0` is the old path.
- **Hardware Opus.** SDK 8.2.1 has `GetHardwareOpusDecoderWorkBufferSize(int, int)`, and its HardwareOpusDecoder class
  reads the decoder type inline (session object at +0, rate and channels at +0x18 / +0x1c, a reset byte at +0x28); the
  host fills it as the SDK does (`runtime/opus_codec.c`). Before this the title screen stopped at A.

### 2026-10-23: a black window, then fights

- **Black window.** The graphics layer's drop counter showed 1,500 draws without a pipeline in 300 frames: Smash binds
  one program per stage (BindProgram with mask 0x1, then 0x2), and the replay kept only the last one. Programs now mix
  per stage.
- **Character select.** A draw bound a texture in alias pages the game had already released, SIGBUS in the deswizzle.
  Textures in released guest memory are skipped (`runtime_guest_mapped`).
- **Loading a match** throws a C++ exception, and `nn::ro::detail::GetExceptionInfo` walks to the module start with
  QueryMemory. QueryMemory now knows the NRO modules, module ends, gaps and the host below `main` (`TOTK_QUERY_LOG=1`).
- **Title bar, black match scene, shifted HUD.** Smash binds two texture pools (8,448 and 4,096 entries) and two sampler
  pools (2,304 and 4,096); the layer kept one table per id, so the second pool's registrations overwrote the first's
  (id 481: a swoosh mask became an icon). Each pool has its own range now and the replay converts handles when a pool is
  bound.
- **Colours too dark.** Smash's window textures are sRGB, Mario's and Zelda's linear; the present read the sRGB view into
  the BGRA8Unorm drawable. It reads the raw view now. Measured against the official title picture: panel 214 to 236
  (236 on the console), text 22 to 83 (82), glow 2 to 17 (16.4).
- **Game too fast.** The display link passed every 120 Hz refresh on as a vsync event and Smash's pace follows that
  rate: the match clock ran 1.87 times too fast. The vsync events come at 60 Hz of display-link time now (`TOTK_VSYNC_HZ`,
  0 = the old way): 1.00 at 60 frames per second, measured on the match clock in two screenshots against their present
  times.
- **Whole picture zoomed, HUD included.** The cause was the uniform copies made at submit time (Tears of the Kingdom's
  cliff fix): on, 18 of 20 and 13 of 16 pictures zoomed; off, 0 of 168 over four matches. Off for Smash
  (`TOTK_UBO_SNAPSHOT=1` turns them on). Which copy is read wrong is not measured.

### 2026-10-23: the app, the shader package, 1440p

- `Super Smash Bros. Ultimate.app` with its data in `~/Library/Application Support/MacNativeGames/SSBU` and its log in
  `~/Library/Logs/MacNativeGames/ssbu.log`.
- The translated shaders compiled into Metal libraries ahead of time (`make shader-libs`): a new program path without
  them waited 34 to 40 s on the purple start picture (Metal's cache is keyed by the program's path length), with them
  about 1 s.
- `TOTK_RENDER_RES=2560x1440` at start renders the game at 1440p. Smash makes its render targets once, at boot, so the
  size cannot change while it runs.

### 2026-10-24: album, startup shaders, reloaded modules, scaled copies

- **Album.** The adventure mode starts `nn::album` and lists the extra movies; `runtime/album_service.c` answers caps:su
  and caps:u as an empty album. Before that the SDK asks `nn::fs::detail::IsEnabledAccessLog()`, which would open
  fsp-srv; the host answers it as off.
- **Startup shaders from data.arc** (`scripts/prepare_shader_seed.py --game ssbu`): Smash's shaders are BNSH files in its
  effects and its own SSBH shader files (`HBSS` + `RDHS`) inside zstd frames. Every frame with such a magic is decoded to
  its end (feeding only the first 4 KiB lost two thirds of them). In play, draws dropped for a missing shader fell from
  32,535 to 1,545 over 20,100 frames.
- **Fighters loaded again.** Smash unloads fighter NROs and loads them again at another address; `main`'s PLT slots still
  pointed into the old mapping (SIGSEGV, pc = old base plus offset). Unloading a module now resets every slot that points
  into it to lazy binding, NRO-to-NRO slots included.
- **Scaled texture copies.** The adventure's save picture is the 1920x1080 frame copied down to 384x216 (copy flag
  LINEAR), then into the save as RGB. The host copied only equal sizes, so the game's magenta fill stayed. A compute
  pass now scales color copies; a render pass that writes depth (and stencil where the source has a stencil view) scales
  depth copies, for the half-size depth image a fight makes.
- **Gap report** (`scripts/gap_report.py`): everything the host reported it could not do yet, from its logs, folded per
  kind; `--session <log>` shows only what is new in one session.

### 2026-10-24: SDK replacements answer in the SDK's own types

- **Spirit Board.** Its refill animation never ended. `nn::time::CalculateSpanBetween` returns a bool in the SDK
  (true = the span was stored); the host returned an `nn::Result`, where 0 is success, so the game read every success as
  a failure. The replacement returns the bool now.
- **`scripts/override_audit.py`** checks every SDK function the host replaces against the original: what the SDK's own
  code returns (read back from each `ret`: register copies, `csel`, a `Result` masked to 32 bits) and how the game's call
  sites read the answer. It found three more: two parental-control checks are bools (free communication allowed, VR
  not), `BeginVrMode` returns a Result, and nifm's `HandleNetworkRequestErrorResult` is a bool. 0 mismatches after.

### 2026-10-24: restart after a language change, the Picture menu, the one-command build

- **Language change.** Smash restarts itself after its language option changes (`nn::applet::RequestToLaunchApplication`
  with its own ApplicationId); before, the SDK reached for an applet object the host does not have (SIGSEGV). The game now
  starts again as a new process and this one ends, after the save is on disk. A first version replaced the process in
  place (execv): the warm-up behind the purple start picture took 98.6 s instead of 6.0 s (`runtime/relaunch.c`). In
  the app the restart opens the app again.
- **Picture menu** in the app: 1080p as shipped or 2K (2560x1440), applied at the next start (the render targets are
  made at boot).
- **`scripts/build_ssbu.py`** builds the app in one command and checks every step; it waits while a game runs from its
  work folder, and the Makefile writes the binary and the translator as new files, so a running game keeps its own.
  From an empty work folder on an M1 Max: about 20 minutes, the app's code and data byte-identical to the app the game
  was played with.
- **Shared shader list** (`assets/shader_list.tsv`): Smash's own, 153 variants over 147 shaders met in play beyond the
  startup shaders (the folder had carried the Mario copy's list).

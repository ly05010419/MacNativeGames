# Source provenance

The initial native host, NVN2/Metal 4 backend, static ARM64 rewriter, shader translator and scripts are isolated copies of this repository's games/totk at 5843ee56f3ab6911e32d61d46e96fee4ce8137de, MIT. They include the already supported Bananza and Pokopia platform implementation. No build step reads another game's folder. Original third-party licenses remain beside their sources.

The disabled VR linkage sources are copied from zeldaVR/gfx and zeldaVR/runtime at the same commit; VR is not enabled for this title.

External nxnative and Lichii reports are references for missing SDK/NVN semantics, not replacement runtimes. Every adapted implementation is identified in its source.

NUP reader: a55uka/nup at d40d9cfa50792ba8021d2967d994d1b1c25d62b0, MIT, third_party/nup. One change: `nup/read.py` decompresses zstd members with Python 3.14's standard `compression.zstd` and falls back to `zstandard` only without it (2026-11-10, #2009: the build's environment had no `zstandard`). The ZBIC decoder (zbic 1.0.1.post1, GPLv2) is a separately installed external build utility, not copied into this repository or shipped in the native app.

The bounded AV1 platform decoder hle_movie_av1.c/.h is adapted from seedds/nxnative ee9b7765 (MIT) into our existing ns2_codec.c protocol; the rest of that project's host/runtime is not used. dav1d is a separate BSD-licensed decoder dependency.

GPU shader stages and matrix operations: bounded MIT adaptations from ChiefSZ13/nxnative ee9b7765d579c46e45a7297493565e098f3bda33, `dmsh_meshstage.py`, `InstEmitAmpereMem.cs` task/mesh instructions and `dmsh_build.py` HMMA lowering. MeshStage.cs / TensorMsl.cs run inside our NativeAOT GPU shader service. The native Metal draw is integrated into our existing encoder FIFO; pipelines compile synchronously. No reference CPU/runtime backend is used.

The original MIT Ryujinx shader-only sources are vendored under `third_party/ryujinx`; project patches are applied only to an external build copy. SPIRV-Cross is built from the pinned official Khronos source declared in `tools/shader_pins.py`.

The Mac App links dav1d 1.5.3 statically as the bounded AV1 fallback (BSD-2-Clause; license in dav1d-COPYING). `build_app.py` builds it once per Mac from the VideoLAN release tarball, pinned by SHA-256, with meson and ninja from hash-pinned PyPI wheels; no Homebrew. VideoToolbox remains the primary path.

Ampere opcode semantics were cross-checked against NVIDIA CUDA Binary Utilities and Mesa NAK sm70_encode.rs (official Mesa source, retrieved 2026-11-05). Lichii nx2 at 62ba83427e16fa127acbe21f60b6fa0dd3bb2c5d was consulted for field meanings only; no GPL runtime or shader implementation was copied into this MIT code. UBREV, predicate packing and two-source F2IP U8/S8 conversion/packing use our existing IR/conversion emitters. F2IP conversion and packing were checked against NVIDIA's published SASS example (developer forum topic 255571/4) and the PTX `cvt.pack` specification, then independently tested on the native GPU backend.

The typed pool-array MSL layout follows ChiefSZ13/nxnative ee9b7765 (MIT, dmsh_build.py pool_arrays); immutable Metal descriptor snapshots and their host state integration are implemented in this game's native backend.

Ampere surface reduction uses this project's existing surface atomic IR and native Metal image atomics. Addressing and reduction semantics were checked against NVIDIA PTX and CUDA Binary Utilities; ordering encodings were cross-checked against official Mesa NAK. Concurrent GPU fixtures use an independent CPU oracle. Unsupported increment/decrement modes are rejected rather than assuming their bounded semantics match ordinary atomic add/subtract.

Shared files (2026-11-18): `runtime/playtime.h`, `gamepad_layout.h`, `feedback_settings.h`, `hid_feedback.c` (this
game's answer to `nn::hid::IsOunceVibrationDeviceMounted` is in the shared file, for every game), `motion_fusion.c`,
`swkbd_host.c`, `ipc.{c,h}` (with CMIF domains; `runtime/threads.c` gained `runtime_handle_take_session` for them),
`bindings_diagram.h` (Tears of the Kingdom's words and kinds, as before, in `runtime/bindings_game.h`), `shared_font.c`,
`game_slot.c` and `agent_slot_limit.h` are symlinks into the repository's `shared/` folder (shared/README.md). Except
for the Ounce answer, these copies were older versions of games/totk's. `third_party/d2r_gfx` is still the link to
games/totk's, whose `totk_mfx_up.{m,h}` and `totk_sysload.{c,h}` are now in `shared/` too. Still this game's own:
`runtime/time_host.c` (the POSIX clock providers for the SDK's `clock_gettime` are not proven in the other games yet,
whose SDKs reference the same functions), `keyboard_map.h` and `third_party/d2r` (`controller_map.h` is shared since, below).


Input stack (2026-11-18): `runtime/gamepad.m`, `gamepad_feedback.m`, `controller_map.h`, `hid_imu.h`, `mouse_look.h`,
`switch_left_fallback.h`, the new `gamepad_core.h`, `gamepad_settings.h`, `gamepad_hid_pad.h`, `gamepad_mouse.h` and
`input_file.h`, and `dsu_client.h` and `dsu_protocol.h` (the shared core reads DSU; this game has no DSU rows, so it stays
off) are symlinks into the repository's `shared/` folder (shared/README.md). The copies were games/totk's but for this
game's start of GameController off the main thread (a wedged gamecontrollerd no longer hangs the start), now in the
shared core for every game. This game's own: `runtime/gamepad_game.h` (mouse mode, the pointing while L is held that the
copy had, zeldaVR's linkage, and the input trace `OOT_INPUT_TRACE`), `runtime/feedback_game.h` (no bow triggers: the
settings row says "Not in this version", as before), and in `runtime/input.c` the key table, the HID answers, the
`settings`, `ashot`, `ashotn` and stereo B2 commands and its flags (`input_trace`, the resolution rows, `fps_lock`).

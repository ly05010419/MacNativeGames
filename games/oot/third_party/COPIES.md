# Source provenance

The initial native host, NVN2/Metal 4 backend, static ARM64 rewriter, shader translator and scripts are isolated copies of this repository's games/totk at 5843ee56f3ab6911e32d61d46e96fee4ce8137de, MIT. They include the already supported Bananza and Pokopia platform implementation. No build step reads another game's folder. Original third-party licenses remain beside their sources.

The disabled VR linkage sources are copied from zeldaVR/gfx and zeldaVR/runtime at the same commit; VR is not enabled for this title.

External nxnative and Lichii reports are references for missing SDK/NVN semantics, not replacement runtimes. Every adapted implementation is identified in its source.

NUP reader: a55uka/nup at d40d9cfa50792ba8021d2967d994d1b1c25d62b0, MIT, third_party/nup. The ZBIC decoder (zbic 1.0.1.post1, GPLv2) is a separately installed external build utility, not copied into this repository or shipped in the native app.

The bounded AV1 platform decoder hle_movie_av1.c/.h is adapted from seedds/nxnative ee9b7765 (MIT) into our existing ns2_codec.c protocol; the rest of that project's host/runtime is not used. dav1d is a separate BSD-licensed decoder dependency.

GPU shader stages and matrix operations: bounded MIT adaptations from ChiefSZ13/nxnative ee9b7765d579c46e45a7297493565e098f3bda33, `dmsh_meshstage.py`, `InstEmitAmpereMem.cs` task/mesh instructions and `dmsh_build.py` HMMA lowering. MeshStage.cs / TensorMsl.cs run inside our NativeAOT GPU shader service. The native Metal draw is integrated into our existing encoder FIFO; pipelines compile synchronously. No reference CPU/runtime backend is used.

The original MIT Ryujinx shader-only sources are vendored under `third_party/ryujinx`; project patches are applied only to an external build copy. SPIRV-Cross is built from the pinned official Khronos source declared in `tools/shader_pins.py`.

The Mac App bundles dav1d 1.5.3 as the bounded AV1 fallback (Homebrew's installed build, BSD-2-Clause; license in dav1d-COPYING). VideoToolbox remains the primary path.

Ampere opcode semantics were cross-checked against NVIDIA CUDA Binary Utilities and Mesa NAK sm70_encode.rs (official Mesa source, retrieved 2026-11-05). Lichii nx2 at 62ba83427e16fa127acbe21f60b6fa0dd3bb2c5d was consulted for field meanings only; no GPL runtime or shader implementation was copied into this MIT code. UBREV, predicate packing and two-source F2IP U8/S8 conversion/packing use our existing IR/conversion emitters. F2IP conversion and packing were checked against NVIDIA's published SASS example (developer forum topic 255571/4) and the PTX `cvt.pack` specification, then independently tested on the native GPU backend.

The typed pool-array MSL layout follows ChiefSZ13/nxnative ee9b7765 (MIT, dmsh_build.py pool_arrays); immutable Metal descriptor snapshots and their host state integration are implemented in this game's native backend.

Ampere surface reduction uses this project's existing surface atomic IR and native Metal image atomics. Addressing and reduction semantics were checked against NVIDIA PTX and CUDA Binary Utilities; ordering encodings were cross-checked against official Mesa NAK. Concurrent GPU fixtures use an independent CPU oracle. Unsupported increment/decrement modes are rejected rather than assuming their bounded semantics match ordinary atomic add/subtract.

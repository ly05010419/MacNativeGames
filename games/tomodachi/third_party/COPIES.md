# Copy provenance

The port is self-contained as required by the parent contribution guide.
`COPIES.json` identifies each copied input by its source path and SHA-256.

- Native host runtime, NVN graphics, rewriter, shader service, and prebuilt
  translator were copied from the local `suniyao/MacNativeGames` working tree
  based on commit `5a4f678`. That working tree includes the previously developed
  renderer and native Tomodachi fixes; the commit alone does not describe those
  uncommitted inputs. Use the per-file hashes for exact provenance.
- Keyboard/controller drawings and mapping headers originate from parent
  commit `933b6a0d1438de160208a0a13eb5e8d7a2cb8094`, including PR #1210. The
  title-specific labels and adapter are described in `../docs/upstream-controls.md`.
- Container/extraction tools retain their original provenance in
  `../tools/UPSTREAM.json` and their license in `../tools/LICENSE`.
- Shader translation vendors only Ryujinx's MIT shader/common/SPIR-V components.
  It does not import an emulator CPU backend, Horizon services, IPC, or scheduler.
- The unused Unicorn-based equivalence diagnostic is omitted; the port and title tests do not depend on a CPU emulator.
- Only source, dependency licenses, identity metadata, and the necessary native
  shader translator are copied. No game data, game shaders, keys, firmware,
  fonts, saves, extracted icons, or generated runtime tables are included.

Paths under other games in this file and COPIES.json are provenance only, never
build-time dependencies. See `../THIRD_PARTY_NOTICES.md` for the retained licenses.

Local modifications after the recorded copy inputs include the corrected inline
keyboard state/submit lifecycle and depth-attachment feedback snapshots for Mii
lighting. `nvn_depth_feedback_probe.h` adds synthetic GPU coverage for pending
clears, read-only reuse, depth writes, pass changes, and resource replacement on
D16, D32, and depth-stencil textures. The hashes identify copied inputs, not
post-modification outputs.

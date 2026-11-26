# ITT shader translator prebuilt

`totk_xlate` is the game-local NativeAOT shader service built from
`games/itt/tools/totk_xlate`, based on historical TOTK revision `947d4c0`
with the explicit compute shared-memory request/accessor/dependency change and
ITT's non-constant texture-offset capability correction.
See that directory's README for provenance, build instructions and validation.
Vendor Ryujinx and the four historical patches are retained.

The SHA-256 in `totk_xlate.sha256` identifies the texture-offset-corrected service
validated offline on 118 previously failing variants and 32 unchanged graphics
controls. Gameplay/FPS acceptance remains pending. Package
this service together with the matching ITT runtime; the inherited service lacks
`deps.shared` and is incompatible with the new runtime's fail-closed checks.

Validation includes 16 same-request graphics translations matching the inherited
service, 24 real compute translations using guest-declared shared memory and Metal
safe-math compilation, and five rejected invalid metadata requests. This is not
proof of all shader semantics or dynamic shared-memory bounds. NativeAOT rebuilds
can differ in bytes; validate translation behavior as well as updating the digest.

No game content or decryption keys are included. Updating this repository prebuilt
does not update an already installed App.

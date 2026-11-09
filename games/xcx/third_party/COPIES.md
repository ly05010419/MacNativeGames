# Source copies

Runtime, graphics, rewriter, translator and scripts copied from `games/by3`
at upstream commit `d1966c52ad30c30e133196bc444d28466628f90a`.
The XCX-local rewriter also carries independent decoder-gap validation changes;
these are source changes in `third_party/d2r`, not a build-time dependency on BY3.
These are independent source copies; XCX does not read another game at build/runtime.
The XCX version, data hashes, runtime preparation and frontend are adapted locally.
No game data, keys, saves or translated shaders are included.

`extract_exefs.py` and `extract_romfs.py` are independent copies of the generic
readers in `games/acnh/tools` at upstream `d1966c52ad30c30e133196bc444d28466628f90a`; the latter produced the checked XCX base RomFS.

The prebuilt `totk_xlate` is byte-identical to BY3 at that upstream commit
(SHA-256 is pinned beside it). Its upstream source/build provenance remains in
`prebuilt/macos-arm64/README.md`; no shader translated from a game is shipped.

`extract_update_romfs.py` is an independent copy from `games/totk/scripts`
at upstream `9fe3fdc1d08ce5e1a64d120f257d79cc2dc7c0ca`, bound to the XCX title ID
and external-artifact path guard. It retains all six IVFC verification gates.

`build_hactool.py` independently copies the pinned build-only hooks from root
`build_app.py` at the same commit. It requires a dedicated external hactool
checkout at `1d64a83450e025622f3468c28fc4164dad2c5ef6` (ISC; bundled mbedTLS
Apache-2.0); retain that source and its license files. No runtime dependency.

The opt-in 2.0.0 timing experiment in `runtime/game_patches.c` independently
adapts `recomp/patches-upd.txt` (`xcx60` only) and the XCX section of
`runtime/frame_rate.c` from [jmars125/xcx-recomp](https://github.com/jmars125/xcx-recomp)
at `feb125c975924331da8ca5365f39d7a6548d6eed` (MIT, Copyright (c) 2026
Valiantsin Dzerakh; license retained in `XCX_RECOMP_LICENSE`). The reference
credits Fl4sh_ and StevensND's 1.0.1 60FPS mod for the original timing-site
research. Only the MIT reference's checked 2.0.0 adaptations are used here;
no mod archive or game code is distributed. Native branches, code-cave
planning, fail-before-write validation and camera execution tests are local.

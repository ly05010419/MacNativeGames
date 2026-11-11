# dav1d (AV1 decoder, arm64 subset)

dav1d 1.5.4 (upstream commit `54706fc`, 2026-07-14), unmodified. Upstream: <https://code.videolan.org/videolan/dav1d>.
License: BSD 2-Clause, see `COPYING`; the contributors are in `THANKS.md`.
The host decodes the games' AV1 movies with it where VideoToolbox has no AV1 decoder (Apple chips before M3 and A17 Pro), and on any chip with `DM_AV1_DAV1D=1` (`dm/host/hle_movie_av1.c`; Fire Emblem Fortune's Weave's movies are AV1). `dm/host/Makefile` builds it into `gen/dav1d/libdav1d.a`; the iOS build compiles the same files (`dm/ios/tools/gen_build.py` reads them from the Makefile).

Only what an arm64 build needs is kept: `include/` (the public headers and `common/`), the C sources and headers of `src/`, and `src/arm/` with the 64-bit NEON assembly; the x86, 32-bit ARM, PowerPC, RISC-V and LoongArch code, the tools, tests and meson files are left out. The files compiled are those `src/meson.build` lists for aarch64 with both bit depths and the assembly on:

- every `src/*.c` but the `*_tmpl.c` templates, and `src/arm/cpu.c`;
- each template once per bit depth, through the wrappers in `config/tmpl/` (`<name>_8bpc.c`, `<name>_16bpc.c`: `#define BITDEPTH` and an include), where meson passes `-DBITDEPTH`: one file per object, so the iOS project, which has one set of defines for all its files, builds them too;
- every `src/arm/64/*.S` but the files other assembly files include (`*_tmpl.S`, `util.S`).

`config/config.h` is the file meson 1.11 writes for dav1d 1.5.4 on an arm64 Apple target, verbatim (`meson setup -Dbitdepths=8,16 -Denable_tools=false -Denable_tests=false`, the default release build; macOS and an `arm64-apple-ios` cross build give the same file with Apple clang 21): both bit depths, the assembly with the C fallbacks it replaces trimmed, the Mach-O symbol prefix, logging, and the assembler features (`.arch armv8.6-a+crc`, `.arch_extension` dotprod, i8mm, sve, sve2). The assembly paths are picked at run time from the CPU's features (`src/arm/cpu.c`). `config/vcs_version.h` holds the version, as a build from the release archive writes it. To check the settings against a new release, run that `meson setup` on its sources and compare `config.h`, and read `src/meson.build` for the file lists.

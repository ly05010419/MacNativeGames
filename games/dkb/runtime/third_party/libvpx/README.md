# libvpx (VP8 / VP9 decoder subset)

The VP8 and VP9 decoders from libvpx v1.17.0 (upstream commit `6df3ec3`, 2026-08-20), unmodified. Upstream: <https://chromium.googlesource.com/webm/libvpx> (mirror <https://github.com/webmproject/libvpx>).
License: BSD 3-Clause, see `LICENSE`; the patent grant is in `PATENTS`, the authors in `AUTHORS`.
The host decodes the games' VP8 and VP9 movies with it (`dm/host/hle_movie.c`, Fire Emblem Engage's cutscenes and Pokemon Brilliant Diamond's opening movies). `dm/host/Makefile` builds every `.c` here into `gen/vpx/libvpx.a`; the iOS build compiles the same files (`dm/ios/tools/gen_build.py` reads them from the Makefile).

Only the files that build needs are kept: the 116 sources libvpx's own build compiles for this configuration, and the headers they include. `config/` holds what `configure` generated (`vpx_config.h`, `vpx_version.h`, the `*_rtcd.h` dispatch headers). One arm64 configuration serves Mac and iOS; NEON is used directly, with no run-time CPU detection. To regenerate, run in an empty directory:

```
<libvpx>/configure --target=arm64-darwin24-gcc --enable-vp8-decoder --disable-vp8-encoder --enable-vp9-decoder --disable-vp9-encoder \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-runtime-cpu-detect \
  --disable-neon-dotprod --disable-neon-i8mm --disable-sve --disable-sve2 --disable-webm-io --disable-libyuv \
  --disable-install-docs --disable-install-bins
make libvpx.a V=1
```

The `-c` lines of that build give the source list (without the generated `vpx_config.c`), and the `.d` files give the headers.

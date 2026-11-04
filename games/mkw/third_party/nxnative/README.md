# nxnative shader frontend (vendored)

The Ampere (SM86) shader frontend that `tools/ampere_service` runs: `dmsh_xlate/` (Switch shader machine code to SPIR-V) and
`ryujinx/` (nxnative's subset of the Ryujinx shader translator, with nxnative's local changes listed in `ryujinx/README.md`).

- Source: https://github.com/seedds/nxnative, PR #983, commit `b124b24a0af5ca9f96a15f92482357bb28d55573`
  (`dm/gfx/tools/dmsh_xlate` and `dm/third_party/ryujinx`, the files Git tracks there, unmodified).
- Only change here: `dmsh_xlate/dmsh_xlate.csproj` points `RyuSrc` at `../ryujinx`.
- License: MIT. nxnative's notice is `LICENSE`; the Ryujinx notice is `ryujinx/LICENSE.txt`.

Links in `ryujinx/README.md` to `docs/` refer to the nxnative repository. `make shader-tools` builds it into
`build/dmsh_xlate/`; to update, replace both folders from a newer nxnative commit and change the commit above.

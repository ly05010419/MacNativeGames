# Bundled shader translator

`totk_xlate` is this game's own unmodified copy of the NativeAOT arm64 macOS shader translator from
MacNativeGames `games/by3/prebuilt/macos-arm64` at commit `30a1b93`. It is a regular file, not a symlink.
The Makefile reads only this copy and checks `totk_xlate.sha256` before installing it in the work folder.

SHA-256: `6dd53d6486c96e3a95ae5fd49fdc35f75f6e603ff8ededa3d7250fd2e4fe1220`.

It translates Maxwell shaders to SPIR-V using the Ryujinx shader library, with the MacNativeGames patches and
service compiled in. The separately compiled, bundled SPIRV-Cross converts SPIR-V to Metal Shading Language.
Building and running this port require no .NET installation and never read another game's translator.

For provenance, the original translator source and publishing instructions are available in
[MacNativeGames at the copied revision](https://github.com/ly05010419/MacNativeGames/tree/30a1b93/games/totk/tools/totk_xlate).
This is a source reference, not a build dependency. Local license copies are listed in
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

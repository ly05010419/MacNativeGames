# Locked binary-only shader compiler tooling

`dmsh_xlate/` contains the 12 required files from nxnative's `dm/gfx/tools/dmsh_xlate_bin` at commit
`b1dee89c`: the apphost, runtime/dependency metadata and required DLLs. Retained files are byte-identical; `.pdb`
files and Humanizer satellite resource DLLs are removed. `spirv-cross/` is nxnative's `dm/gfx/tools/spirv_cross_bin`
at the same commit: the SPIRV-Cross build of tag `vulkan-sdk-1.4.363.0` (commit `f11ba9f0`) that its `build.sh`
makes, with its licenses. `SHA256SUMS` covers the exact retained file set and is verified before staging
(`scripts/build_ss.py`).

These are build tools, not translated game shaders, and are never bundled in the generated game app. Their sources
are not duplicated in this target: the translator's and its Ryujinx shader subset's are in the
[nxnative tree](https://github.com/seedds/nxnative/tree/b1dee89c/dm/gfx/tools/dmsh_xlate), SPIRV-Cross's upstream.

`runtime/gfx/tools/dmsh_chain.lock` pins the translator DLL fingerprints, its embedded source hash (`--source-sha`)
and the SPIRV-Cross fingerprint; the chain takes the translator's source hash from the lock. Updating either binary
requires new SHA-256 sums, its provenance, the lock, a regenerated chain header and a full shader rebuild.

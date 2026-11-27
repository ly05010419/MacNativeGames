# MCD2 shader-service sources

Game-local fork of `games/totk/tools/totk_xlate` (Ryujinx translator patches and
NativeAOT service). Other games and their prebuilts are not modified.

Compute requests additionally require `shared`: the aligned shared-memory byte
count from the guest NVN control section at +0x754 (zero is valid for kernels
without shared memory). The service forwards it unchanged and records
`deps.shared`. Runtime lookup rejects legacy compute variants without this
witness. Rebuild runtime and service together; the runtime refuses output from
an old service that silently drops this field. This candidate still needs GPU
sorting/visual validation and is not installed in the formal App.

Dynamic resource queries require explicit `array_lengths` answers:
`texture_pool` (1..2^20), `sampler_pool` (1..2^12), or `buffer_<cbSlot>` for
constant-buffer arrays. Missing answers fail instead of silently using 32.
Used answers are emitted in `.res.json` and reflection `deps.array_lengths`.
Only metadata-verified Direct texture-buffer/sampler pool arrays are reflected;
other array kinds remain unsupported. Rebuilding this service does **not** mean
pool-array replay is implemented or gameplay validated.

```
tools/totk_xlate/publish_native.sh /path/to/dotnet /path/to/MCD2_build
```

Output: `<work>/build/shader-service-candidate/totk_xlate` and SHA-256. Generated
sources/caches stay in `<work>/src`. This is a tool rebuild, not an alternative
App pipeline: the App still uses the unified `GameBuilder` build. No repository
prebuilt, active worktree service, or signed App is overwritten by this script.

The development runtime now tracks bound-pool domain copies, includes capacities
in answers and graphics/compute selection, checks used-length dependencies, and
filters legacy array cache rows. These do not resolve descriptors across pools:
registration still uses global ID tables. Before using the new service it must
validate the complete resource path. The development runtime now has 32-bit
argument IDs, a checked range planner, a dense-layout/raw-RID gate and request-sized
arenas, with CPU and GPU capability tests; production array replay is still disabled.
It must resolve real pool descriptors, synchronize buffer textures and retain resources
through GPU completion. Those paths remain fail-closed until validated.

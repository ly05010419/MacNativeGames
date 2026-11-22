// Separate experimental TOTK cache identity; D2R shader caches are rejected.
// aocx-native-v5: Age of Calamity translates with Tears of the Kingdom's translator (games/totk/prebuilt/macos-arm64), which
// gives a combined texture handle (c[2] + 0x20) its slot's bound type (a73d65e2): the first battle's compute shaders fetch
// their light lists through a buffer texture bound to such a handle, which the earlier translator (Odyssey's, the v4 chain
// of games/mk8dx and games/rdr) read as a 2D texture (undefined on Metal: the GPU never finished; the Mac panicked).
// aocx-native-v6-shared-memory: a compute kernel's shared memory size comes from its control section (+0x754, rounded up
// to 16 bytes, as nxnative's shader_split.py answers it) instead of the translator's 4 KiB default. The first battle's
// particle kernel dc81d690 keeps 68 bytes per thread from byte 304 for 64 threads (4656 bytes): beyond 4 KiB its threads
// wrote outside the threadgroup array, so the counts its loops run to were garbage and the GPU never finished.
#define DMSH_CHAIN "aocx-native-v6-shared-memory"

// Separate experimental TOTK cache identity; D2R shader caches are rejected.
// Super Smash Bros. Ultimate: its own translator build (games/ssbu/tools/totk_xlate: no gather offsets, no non-constant
// texture offsets on Metal) translates shaders with offsets differently, so its cache has its own name.
#define DMSH_CHAIN "totk-native-v7-ssbu-offsets"

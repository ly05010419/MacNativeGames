/* The translation chain each game's shader caches belong to (shader-cache/rt-<chain>): the shared graphics layer is built
   for several games with their own translators, so the chain is the game's. Build scripts read the line tagged with their
   game ("game: <id>"). TOTK's is a separate experimental cache identity; D2R shader caches are rejected. */
#if defined(GAME_OOT)
#define DMSH_CHAIN "oot-native-v28-mesh-primitive-blocks"   /* game: oot */   /* v28: mesh outputs read as every vertex block, then every primitive block (MeshStage.cs); v27: pixel shaders zero a non-finite colour under a zero blend weight (nvn_blend_zero.h) */
#else
#define DMSH_CHAIN "totk-native-v9-unverified"   /* game: totk (also BotW and the Switch 2 games built with games/totk) */
#endif

/* The zero-copy pool limit of nvn_mtl.m (pool_range_check): a header, so the shared file keeps its line numbers. */
#ifdef GAME_BOTW   /* both versions: the 1.6.0 graphics objects are built with GAME_BOTW alone */
/* Breath of the Wild 1.9.0 and 1.6.0 with TOTK_BOTW_HD_HEAPS=1 (game_patches_botw.c): the HD texture pack's 2032 MiB texture
   compaction heap is one pool, so the limit is 2 GiB there (the owner, 2026-10-24; Bayonetta 3's layer runs at 2 GiB too). */
int runtime_botw_hd_heaps(void);
#define POOL_BUFFER_MAX (runtime_botw_hd_heaps() ? 2ull << 30 : 1ull << 30)
#else
#define POOL_BUFFER_MAX (1ull << 30)
#endif

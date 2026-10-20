# The 4K texture pack on 1.4.3 (Mac)

The HDPacks.com 4K texture pack (Tears of the Kingdom) ships its textures and two System tables for 1.0.0 to 1.2.1, with a v1.0.1 patch for 1.4.2. The textures are the same bytes in 1.4.3; only the tables name the version. This page says how to make them for 1.4.3 and how the build uses them. The pack is not part of this repository: download it yourself.

## Make the mod folder

Extract the pack and its patch. Then, from `games/totk`:

```
.venv/bin/python scripts/make_texture_pack_tables.py <pack>/4kTextures/RomFS/System <patch>/4kTextures/RomFS/System \
    <pack>/4kTextures/RomFS/TexToGo --output <mod folder> --heapdef-overlay extracted/v143/romfs-patches-out/4k
.venv/bin/python scripts/make_mod_manifest.py <mod folder>
```

- `ResourceSizeTable` is the game's own table with the size of each pack texture replaced by the pack's size. Other entries stay as the game ships them.
- `HeapDef` is the game's own with the heaps the pack enlarges (TextureHeap, the graphics heap, the grass heap) set to the pack's sizes. Every other heap must be equal in the game's and the pack's file, or the script stops.
- With `--heapdef-overlay` the HeapDef goes to the build's overlay folder, not the mod folder. A mod and the build must not both replace the same file; the build already replaces HeapDef when the output is bigger than 1920x1080.

Copy the textures (`TexToGo`) into the mod folder and point `TOTK_ROMFS_MODS` at it.

## Run

`TOTK_4K=1` tells the runtime the pack is installed. It then
- adds the pack's heap growth (3.4 GiB) to the application pool (`runtime/texture_pack.h`, `runtime_output_res_extra_bytes`),
- loads the HeapDef overlay `romfs-patches-out/<canvas>-4k`, or `romfs-patches-out/4k` when the output is the game's own size,
- allows a 4 GiB zero-copy pool buffer for the 3.75 GiB texture heap (`TOTK_POOL_BUFFER_MAX_MB` sets another limit).

With a bigger output (`TOTK_OUTPUT_RES`, `TOTK_RENDER_RES`) build the overlay on top of the pack's HeapDef so the growth stays:

```
.venv/bin/python scripts/make_output_res_overlay.py 2560x1440 --version v143 \
    --base-heapdef extracted/v143/romfs-patches-out/4k/System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs --name 2560x1440-4k
```

## Memory

The 4K heaps make the application pool about 7.8 GiB. The guest address reservation is 8 GiB for the low region, the heap cap is 7 GiB and the smallest alias region is 4 GiB (the game maps about 3.3 GiB of it).

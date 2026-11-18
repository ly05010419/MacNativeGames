# dm/gfx/tex — texture format mapping, block-linear deswizzling, BC decompression

中文：[README.zh-CN.md](README.zh-CN.md)

The part of the graphics layer that deals with texture data, called by the replay layer `../nvn/nvn_mtl.m`. It does three things:

1. converts NVN texture format numbers to Metal pixel formats;
2. unpacks the Switch's "block-linear" layout into an ordinary row-by-row layout;
3. decompresses BC textures on GPUs that do not support BC compressed textures.

Block linear is how the Tegra X1 GPU stores textures: the data is not one row after another, but interleaved in small blocks of 64 bytes × 8 rows (GOBs), and several GOBs are then stacked into larger blocks according to a "block height" and a "block depth". The game's packaged texture files are in exactly this layout, and it must be undone before uploading to Metal.

## Files

| File | Contents |
|---|---|
| `nvn_fmt_mtl.h`, `nvn_fmt_mtl.c` | `nvn_fmt_to_mtl()`: NVN format (0x01–0x9C) → Metal pixel format + component remapping. Returns 1 for an exact match, 2 for a substitute (precision, bit order or interpretation differs and a conversion is needed), 0 when Metal has no corresponding format. How each format is handled is written in the comments inside the table. Pure C, no dependency on Metal headers |
| `nvn_deswizzle.h`, `nvn_deswizzle.c` | `nvn_deswizzle()`: deswizzles one level and one layer from block linear to linear; `nvn_swizzle()`: the reverse, used for round trips in unit tests; `nvn_bl_offset()`: the block-linear address of a single point. Pure C |
| `nvn_deswizzle_gpu.h`, `nvn_deswizzle_gpu.m` | A Metal compute shader version of the same deswizzling algorithm, used by the replay layer when uploading packaged textures; when initialization fails or an unsupported format is encountered, the replay layer falls back to the CPU version |
| `nvn_bcdec.h`, `nvn_bcdec.c` | `nvn_bc_unpacked()` / `nvn_bc_decode()`: decompress BC textures on the CPU into uncompressed formats; decoding uses `dm/third_party/bcdec` |
| `tests/` | Unit tests |

The offsets of each level and layer within the storage, and the block height and block depth actually used by each level, are all taken from `../nvn/nvn_tex.c` (`nvn_tex_layout`, `nvn_tex_level_offset`, `nvn_tex_level_tiles`); this directory only does addressing and moving within a level. The game's packaged textures carry their own layout parameters, whose byte 0 is the log2 of the level 0 block height; when it is present it takes precedence (the `pkg_block_h` field of `nvn_texdesc`).

## Devices without BC support

Some older Apple GPUs do not support BC compressed textures (determined by `supportsBCTextureCompression` of `MTLDevice`). On these devices:

- the game's textures can be converted to ASTC format offline in advance with an optional supplementary pack (`dm/tools/astc_pack.py`; for dependencies see the "Optional" section of `docs/DEPENDENCIES.md`);
- the remaining BC textures are created by the replay layer in their decompressed format and decompressed at upload; decompression on the CPU goes through `nvn_bcdec.c` in this directory.

The environment variable `DM_TEST_NO_BC=1` forces this path on any machine.

## Tests

```sh
make -C dm/gfx/tex/tests samples   # fetch real-data samples (see below)
make -C dm/gfx/tex/tests run       # run t_fmt_mtl and t_tex
```

- **`t_fmt_mtl`**: takes the mapping for every NVN format in 0x01–0x9C and, for every one whose mapping is not 0, actually creates a texture on the local Metal device; all of them must be created successfully. It also checks that every NVN format with sRGB in its name lands on a Metal sRGB format. No game files needed.
- **`t_tex`**: the deswizzling tests, in several groups:

  | Group | Criterion |
  |---|---|
  | A round trip | Writes random data into each level and layer and reads it back out; must be byte-for-byte equal. Covers 2D, 2D array and 3D, several bytes-per-block values and non-power-of-two sizes |
  | B address formula | The result of `nvn_bl_offset()` equals the address computed by hand from the public layout rules |
  | E hand-made 3D | Places markers at hand-computed addresses, checks where they land after deswizzling, and checks block height, block depth and storage size |
  | C real data | The Switch version (block linear) of a texture, after deswizzling, is compared with the PC version (linear) of the same texture: those at the same resolution must be byte-for-byte equal; those whose Switch version was reduced in resolution were recompressed, so the comparison is instead that the PSNR after decoding (peak signal-to-noise ratio, a measure of how alike two images are) is not below 22 dB. It also checks that the per-level offsets in the file agree with `nvn_tex_level_offset` |
  | D negative control | Deliberately deswizzles with the block height off by one step; the result must no longer be byte-for-byte equal and the PSNR must be below 22 dB, proving that the criterion of group C can tell a wrong layout apart |

  The A round trip can only prove "one-to-one and within bounds"; it cannot prove that the layout formula is right. What really backs it up is the hand-computed addresses of group B and the byte-for-byte equality of the same-resolution samples of group C.

**About the real-data samples**: groups C and D need one copy each of the Switch version and the PC version of the same texture. `tests/fetch_samples.py` takes samples from the game files in your workspace: the Switch version is read from `$DM_ROOT/data/switch/3.3.94012/Update/RomFS/data`, the PC version from `$DM_ROOT/data/assets-src/raw`, and the output goes to `$DM_ROOT/build/cache/dm/tex_samples`. When there are no samples, the real-data items of `t_tex` count as failed and are not skipped; groups A, B and E need no game files. The samples do not go into the repository.

## Layout-related fields in the texture file header

Facts used by `t_tex.c` when parsing the game's `.texture` files:

- The file begins with `<DE(`; the Switch version and the PC version use the same container.
- In files that went through the Switch conversion, the four bytes starting at offset 0x14 are `00 00 00 07` (0 in the PC version), which indicates that the pixel area is in block-linear layout; offset 0x13 is the log2 of the level 0 block height, with the same meaning as byte 0 of the layout parameters the game passes to `SetPackagedTextureLayout` at run time.
- Files without this marker are stored linearly and are byte-for-byte identical to the PC version.

## Limitations

- `nvn_deswizzle()` does not support multisampled textures, textures with the LINEAR flag (which are linear to begin with) or the BUFFER target; it returns -1 when it encounters them.
- Cubes and cube arrays are handled as layered textures and have not been separately checked with real data.
- 3D textures have only the round-trip and hand-computed address checks; there are no real-data samples.
- The component bit order of the 16-bit packed formats (565 / 5551 / 4444) is mapped according to the format names and has not been checked with real data; they are marked as "substitute" in the format table.
- The real-data sample set is small, covering the four formats RGBA8, BC1, BC3 and BC4.

# Copy provenance

The initial copy is `games/tomodachi` at MacNativeGames commit
`3edfb0b6`. That donor is the 2024 Nintendo-SDK / NVN-on-Metal 4 host with
libopus and libvpx. No donor folder is read by the Emio build or runtime.

`COPIES.json` is the donor snapshot at copy time (hashes of the Tomodachi
tree before Emio retargeting). Paths under other games in that file are
provenance only.

Emio adaptations after the copy:

- Identity retargeted to `0100A9501759E000` / 1.0.0 (`assets/expected-v1.0.0.json`).
- Empty `runtime/game_patches.c` until a first boot measures a needed site.
- Ticket titlekey handling accepts a plaintext common-ticket titlekey as well
  as a titlekek wrap.
- Builder, launcher, save macros, and log prefix retargeted off Tomodachi.
- Donor Mii/cloth/pool instruction patches and the Mii allocator crash probe
  were not kept.

See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for licenses.

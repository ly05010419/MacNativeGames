# Optional native player-cheat panel (macOS, TOTK 1.2.1)

The app menu's **金手指…** (Command-Option-C) opens a searchable native panel.
Use the mouse or controller Up/Down, A to toggle and B to close.
**全部开启** selects batch-eligible compatible entries, keeping existing variant
choices; **全部关闭** clears the selection. Packs explicitly opt entries into batch
activation with `bulk: true` and may provide `bulk_rank` to prefer a default variant.
Inventory add/remove and progress edits remain individually selectable. Number
prefixes in imported names are hidden without changing stable IDs or attribution. The panel captures
controller input while open. **暂停全部** retains the selection; preferences live
in `cheats.json` beside the profile directory. New entries start off.

The repository contains no cheat pack, game instructions or game addresses.
`runtime/cheat_vm.h` independently executes the documented Atmosphere **data
format**, not ARM instructions. It is not a CPU interpreter or emulator backend.
Its read/write callbacks operate only on this runtime's own guest mappings.
Supported records: 0–9, A and C0; unsupported records fail closed. Programs have
isolated scratch registers, validated control flow, nesting and execution limits.

A player may prepare a private catalog from their supplied TXT and their verified
retail NSO using `scripts/cheat_catalog.py --help`. Its output contains game
instructions and must stay outside the checkout. Install it in the app's Resources
as `PlayerCheats/v121/catalog.json`, or use `TOTK_CHEAT_CATALOG` for a development
run. The catalog's full build ID must match the app's extracted build-ID record.
The exporter checks direct writes against the runtime/rewriter's protected sites
and records original readonly words from the verified NSO. A package installer
must additionally check those guards against its pristine rewritten image before
bundling. Do not layer an older `TOTK_PCHTXT` pack over the same catalog.

Selections with incompatible direct writes or the same variant group exclude each
other. Entries without codes appear disabled and explain why. A successfully
parsed or applied entry is **not** a claim that its game effect has been tested.

The native adapter stages one entry's writes, checks pointers and guards, then
commits it while this runtime's retained guest threads are suspended. It makes
readonly pages temporarily RW, never RWX, invalidates their instruction caches,
restores protections, and resumes the threads on every recoverable path. The
suspended section performs no Objective-C calls or allocations. Thread creation is
blocked across snapshot and suspension so a newly created guest cannot escape it. A periodic tick
has a four-millisecond execution deadline, a bounded page cache/write set, and
round-robin progress. No selections means no memory reads or guest suspensions;
static patches without hotkeys do no ongoing work after applying. Catalog entries
may set `apply_once: true` for single-application data edits or `interval_ms`
(33–5000 ms) for recurring work. Failed applications retry after 500 ms. Status
refreshes update existing visible fields only when their text changes.

Unchecking restores guarded nonzero code/constant words after their last owner
leaves. Dormant originally-zero cave bodies stay until the next clean process
launch, because a stopped guest may already be executing inside one. Reusing such a cave for a
different payload is refused until a clean restart. Data edits
stop repeating; inventory, map, health and quest changes may persist in saves.
Turning a switch off does not restore an older save or reverse acquired items.
Keep a separate save backup before experimenting with data-changing entries.

Validation uses synthetic fixtures only: `tests/test_cheat_vm.py` checks bounded
control flow, pointers, guards, ownership, restoration and host-memory isolation;
`tests/test_cheat_panel.py` checks filtering, missing entries, exclusivity,
persistence and input capture without game data. Actual effects require in-game
validation, separately from compilation and these fixtures. The native panel is
macOS-only; the shared accessors also compile in the iOS runtime.

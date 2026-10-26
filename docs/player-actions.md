# Optional player actions: TOTK 1.0.0 and 1.4.3 on Mac

This change splits the independent gameplay layer out of #141, separate from the
physical binding editor and #42 display/pacing features. It includes both verified
1.0.0 and 1.4.3 instruction layouts. It does not replace `controller_map.h`,
`controls.txt`, main’s mouse mode, or any physical keyboard/controller bindings.

Choose **Player Actions…** in the Mac app menu. The 16 game-visible sources are
A/B/X/Y, L/R/ZL/ZR, four D-pad directions, stick clicks and Plus/Minus. Each selects
one of 17 targets, including Unassigned, Pause/Inventory and Map. For example A
can jump during gameplay while A continues to confirm in menus. Reset restores
the original action layout; Cancel discards the draft. The sheet neutralizes input
and releases the mouse. The game’s button prompt images remain unchanged.

The feature starts **off** unless a valid `player-actions.txt` exists next to the
launcher-selected `controls.txt`, or `TOTK_ACTION_LAYOUT=xbox`, `TOTK_ACTION_MAP`
or `TOTK_PLAYER_ACTIONS=1` explicitly enables it. First Apply saves the 16 action
codes and asks for a restart to install translation before RX sealing. Subsequent
Apply publishes a sequence-checked writable data table without rewriting live
executable memory. Failed instruction guards refuse Apply and persistence;
failed atomic saves restore the prior live layout. External file edits are
protected. `TOTK_PLAYER_ACTIONS=0` disables installation even with saved settings.
Physical binding configuration remains exclusively in `controls.txt`; the optional
gameplay settings contain only action codes, no duplicate physical layout.

The translators read separate pressed/held/released masks and leave the shared
raw input object untouched. Scoped gameplay screen requests and ability/shortcut
queries follow the action layout; ordinary menu, dialogue and map inputs retain
the original buttons. Plus/Minus can select gameplay actions without opening
their old screens, and another source can request Pause/Inventory or Map.

1.0.0 installs a translator into checked zero text padding, after existing
animation veneers. Original operation words, entry/return, gameplay router and
query publication are validated before any writes. 1.4.3 preserves the original
gating at `main+0xf23e28`, then translates the w21/w20/w22 operation masks at
`main+0xf23e44`; its gameplay edge/release load is fused and has a dedicated
trampoline. It reserves a second private 16 KiB page for immutable action code,
separate from the writable page holding main’s existing timing/output settings
and the action table at +64. The loader seals the code page RX and verifies its
permissions. Instruction, padding and branch-range checks fail without partial
installation. The patch changes no frame timing, resolution, MetalFX or shared
NVN behavior. The two existing size assertions accept the additional action page;
the timing/output algorithms and defaults remain main’s.

Unsupported versions (1.1.2, 1.2.1, BotW) and iOS/export retain their original action
path. Shared code does not import optional Mac frontend symbols; the AppKit editor
is included only in the Mac frontend. iOS allocates the existing private page only.

Core validation:

- `TOTK_RUN_APPKIT_PROBES=1 python3 -m unittest discover -s tests -p test_action_bindings.py`
- Native ARM64 execution for both versions: all 16 sources/17 targets,
  pressed/held/released, gating, gameplay router/query scopes, register/flag
  preservation, raw device immutability, guarded failure and live table updates.
- Concurrent publication and unsupported-version refusal; optional startup and
  saved-file loading/override/error cases. The real AppKit sheet covers 16 rows,
  live Apply, atomic persistence, Reset/Cancel, file conflict and failed guards.
- Modified C/AppKit sources compiled on Mac; shared input and both game patch
  sources compiled against the iPhoneOS SDK. Targeted existing input, menu,
  picture-mode and pchtxt probes check integration boundaries.

These are native execution/compiler/probe checks of this isolated branch, not a
new full retail App/IPA build or game-play pass. Existing local development had
1.0.0 gameplay evidence and a 1.4.3 retail guard-install record before this split;
those older results are not represented as gameplay validation of this new head.
No physical controller model certification or full-game completion is claimed.
All generated probes, screenshots and metadata stay outside the repository.

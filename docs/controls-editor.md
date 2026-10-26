# Input binding editor (Mac)

Open **Input Bindings…** from the app menu, or press **Command+,**.
Keyboard lists physical Mac key positions; select a Switch button, a movement/look
stick direction, or None. Multiple keys can drive the same target. Controller and
Mouse lists all 22 existing physical buttons, including touchpad and five mouse
buttons, with the existing hold/tap/toggle/aim modes. Aim’s last column chooses the
other physical button that fires. Mouse mode and controller type stay in main’s settings panel (**1**). Keys **1–9**
and Command are reserved for application shortcuts, including HDR (**8**, #582)
and the frame-rate cap (**9**, #640); the editor does not add or restore shortcuts.

Apply validates and atomically saves the launcher-selected `TOTK_CONTROLS` file
(normally the game data directory’s `controls.txt`), then updates live input. Cancel
discards the draft. Reset Defaults changes the draft until Apply. The editor retains
comments, rewrites binding lines, and refuses to overwrite a file changed externally
since opening. Invalid/unreadable files produce an error; they are not replaced by
defaults. While the sheet is open the pointer is released and guest input is neutral;
closing clears held input, including old tap/toggle state.

Existing controller files remain valid. Keyboard entries extend that same file:

```text
# Physical Mac key codes, independent of the active character layout
key_38 = none       # clear J’s default A binding
key_7 = a          # X now drives A
key_0 = look_left  # A drives the camera stick left
mouse_right = zr aim mouse_left
south = b
```

Unspecified entries keep main’s defaults. Keyboard targets are the 16 existing
Switch button names, `none`, `move_left/right/up/down`, or
`look_left/right/up/down`. Keyboard entries take no mode. The launcher, runtime
and editor use one parser. The original `controller_map.h` controller parser and
mode engine are reused.

This is physical binding editing: remapped Switch buttons affect gameplay and
menus together. Independent player actions belong to a separate change. This PR
contains no game patch, display/pacing change, version table change or second
preference store. It is independent of TOTK instruction addresses, including 1.0.0
and 1.4.3; the shared BotW runtime also uses it. The sheet is compiled only in the
Mac frontend. iOS keeps its existing touch/controller input.

Core validation: `TOTK_RUN_APPKIT_PROBES=1 python3 -m unittest discover -s tests -p test_controls_layout.py`, existing controller-map/mouse/controller-type probes,
and macOS/iOS compilation of the modified runtime. The AppKit probe uses synthetic
controls files and exercises save/reload, comments, conflict/error handling,
Reset/Cancel and the complete rows. Runtime checks cover remapped short taps,
alternate holds/releases, stick directions, sheet isolation and shortcut/default
regression. These checks do not certify physical controller models or retail
gameplay. Generated probes/screenshots stay outside the source tree.

Button-icon compatibility: `prompt_layout.py` and its C parity probe are based on
[PR #460](https://github.com/ly05010419/MacNativeGames/pull/460), head
`6be9aff510d67f1efca297f56600dd5123650a10`. The shared controller-prompt parser now
validates the editor’s `key_` rows and excludes them from controller prompt
selection. Its controller mapping, face modes and hold/tap/toggle/aim handling are
unchanged. Invalid keyboard rows still fail with their original line number.
This does not add remapped keyboard glyph rendering to #460’s fixed keyboard style.

Review validation on 2026-10-26 (main `10144b0`): the three shared-file/runtime/
AppKit probes and 13 controller-prompt tests passed, including all 128 keyboard
codes against the C parser. The controls input probe uses `--controls-only`: the
full, pre-existing motion fixture still expects 4 ms samples while main now emits
5 ms states. Rumble/motion validation is outside this change. The complete macOS
1.4.3 runtime also built and passed its loader probe.

The #460 icon tool generated all four TOTK font archives and the resource-size
table from the editor's canonical 150-line file (22 physical rows + 128 keyboard
rows); unmodified #460 rejects that same file at its first `key_` row. Evidence
logs and generated files are external to this checkout at
`/Volumes/mac-APFS/MacNativeGames-artifacts/2026-10-26-controls-review`.

The combined code was run in TOTK 1.4.3 on this Mac with a copied profile and
main's launcher GPU flags. The actual game sheet showed 1–9 and Command disabled,
Apply saved the canonical file, and X (`key_7 = a`) confirmed loading and retrying.
The Great Sky Island scene rendered with the generated Xbox font prompts (LB
visible); the fixed-image pickup popup retained its Switch A, as #460 documents.
This is one local game run, not physical-controller certification or a complete
#460 launcher/automatic-kit test. The test window was closed normally; the
installed apps, desktop entry and original profile were left in place.

Since #460 is still open, this PR carries only its shared `prompt_layout.py` and
C/Python parity probes with the keyboard-line compatibility change. Keep these
versions of those files when combining the PRs; the rest of #460 is not imported.

Before publishing, main advanced to `84eebcd` (#523 feedback settings, #477
DualSense triggers, #511 sampled copies). It was merged with all new settings
preserved. The three affected controls/AppKit probes and the full macOS 1.4.3
runtime build passed again; the earlier retail game run is on the `10144b0`
integration and is not presented as another game run after this final sync.

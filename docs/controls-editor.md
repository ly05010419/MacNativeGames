# Input binding editor (Mac)

Open **Input Bindings…** from the app menu, or press **Command+,**.
Keyboard lists physical Mac key positions; select a Switch button, a movement/look
stick direction, or None. Multiple keys can drive the same target. Controller and
Mouse lists all 22 existing physical buttons, including touchpad and five mouse
buttons, with the existing hold/tap/toggle/aim modes. Aim’s last column chooses the
other physical button that fires. Mouse movement and key **7** keep main’s mouse
mode; keys **1–7**, Command shortcuts, controller type and all defaults are unchanged.

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

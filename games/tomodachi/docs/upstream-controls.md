# Tomodachi: upstream settings and controller drawings

This port reuses the upstream control diagrams and mapping helpers rather than
redrawing a similar-looking controller. Source snapshot:
`ly05010419/MacNativeGames@933b6a0d1438de160208a0a13eb5e8d7a2cb8094`.
The code remains covered by this project's upstream MIT license.

## Reviewed changes

- [PR #1210](https://github.com/ly05010419/MacNativeGames/pull/1210),
  author commit `3eace3c640dee328d4b9484cbee22ad71295c2c7`, merged as
  `d3389b079ea7b0a80d4c0ddd79c969c5d2c13678`: keyboard/mouse and controller
  drawings, clickable assignments, controller-family labels, mapping helpers,
  and the tabbed settings interface.
- [PR #977](https://github.com/ly05010419/MacNativeGames/pull/977), merge
  `c4bb1fe`: controller-operated settings navigation. Its prerequisite
  [PR #976](https://github.com/ly05010419/MacNativeGames/pull/976), merge
  `6f9b179`, introduced the controller capture APIs for the native keyboard.
- Later PRs #1392 and #1397 were visible as open changes during review. They
  were not silently imported into the pinned port. In particular, the current
  adapter does not implement the newer button-icon mod feature.

## Reused source and title adapter

`runtime/bindings_diagram.h`, `keyboard_map.h`, `controller_map.h`,
and `gamepad_layout.h` come from the pinned upstream runtime. The diagram has
one conditional title-label hook: Tomodachi uses its actual button names,
movement, and zoom labels instead of Zelda combat actions. Its geometry,
callouts, keyboard drawing, colours, hit testing, and missing-binding display
remain upstream's implementation.

`runtime/settings_controls.h` connects these helpers to the
existing title input hooks and preferences. `settings_controls_ui.h` adapts
upstream's drawing menus and navigation patterns to this title's settings.
The settings surface uses an NSView inside the game window, as upstream does;
the earlier separate NSPanel/material-window approximation is removed.

Keyboard assignments use upstream `keymap_assign`; controller assignments use
`controller_map_assign`. The title stores them in its profile's settings.json
as `keyboardMap` and `controllerMap`; existing ten-key settings and face-button
swap preferences are migrated when these arrays are absent. Guest key/button
translation reads atomic snapshots so settings changes take effect immediately.
The two optional hooks in shared gamepad.m deliver physical button normalization
and native controller events only when a title supplies them. Other titles keep
their existing path.

The Input page picks Auto, Xbox, PlayStation, or Switch. The controller drawing
follows that family. L3+R3 opens/closes settings, L/R selects a page, B closes,
and d-pad/stick navigation selects settings or controller callouts. On the
controller drawing left/right changes the highlighted physical button's action;
A switches the keyboard/controller drawing. Click a drawing's button for its
action menu. Keyboard and controller defaults can be restored independently.

Title-specific differences are intentional: mouse buttons retain Tomodachi's
touch interactions, so their diagram entries explain that they are unavailable
for remapping; the touchpad is unavailable until this runtime reports it. The
port does not expose Zelda-only bow/ability/graphics patches as working options.

## Verification and external evidence

- Preference tests cover legacy settings, invalid bindings/language, and
  controller bit preservation.
- The native UI smoke test checks tabs, overlay persistence, and closing and
  reopening the in-game panel. It also assigns through the diagram adapter and
  checks the resulting guest keyboard/controller bits. Simulated native pad
  events verify settings opening, shoulder-tab navigation, and closing.
- The Metal 4 desktop app builds, packages, and verifies its code signature.
  The actual controller drawing and in-game settings were visually inspected
  in the running desktop app. A physical-controller compatibility sweep across
  Xbox/PlayStation/Switch devices has not been performed.
- Fresh contribution-build validation and known limitations are recorded in the title README. Tests generate their own temporary files outside source.

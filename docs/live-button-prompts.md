# Live button prompts (Tears of the Kingdom 1.4.3, macOS)

Settings (key **1**) → **Input** → **Button prompts** chooses Auto,
Keyboard / mouse, Xbox, PlayStation, Switch, or Off (the game's own icons, no redrawing). With a controller type picked
on key **6**, Auto labels that controller by the type picked. Auto follows fresh keyboard,
mouse, or controller input. A held controller button and small stick drift do
not continually switch it back. The choice is saved in `prompt-icons.txt`
beside `controls.txt`.

Key bindings changed in the settings panel update the visible prompts during
the same session. The primary usable binding is shown; with mouse mode off,
a keyboard alternative is used. An action with no usable binding shows a dash.
Controller icons name the physical button assigned to the action, including
remaps. On a default Xbox layout, the Switch A action is the physical B button;
on a DualSense it is Circle. Changing controller icon style does not remap input.

This covers the icon-font prompts in menus and the eight shared HUD button
textures, including normal gameplay interactions such as Examine. Baked drawings
(tutorial animations and the faint controller diagram behind interaction prompts)
are outside this feature. UI mods that replace the original
HUD masks may not match the live replacements and need their own integration.
Other game versions and iOS keep their existing prompt rendering.

## Implementation

- `runtime/prompt_bindings.h` resolves the effective bindings without platform
  APIs. `runtime/input_prompts.m` owns synchronized snapshots, device/style
  selection, and top-first grayscale key/button drawing through CoreText.
- `runtime/prompt_cache_v143.h` guards two calls to the game's font-cache flush.
  After the original flush, it publishes the current prompt-glyph rectangles.
  Its version-specific offsets are documented there. The adapter is disabled
  during iOS exports; `TOTK_PROMPT_ICONS=0` disables the feature for diagnosis.
- `totk_prompt_icons.m` makes renderer-owned R8 texture copies. Font scanlines
  are bottom-up; HUD masks are top-first. Neither game memory nor text metrics
  are modified. Replacement textures use the renderer's residency and deferred
  release paths. Prompt revisions invalidate descriptor views and argument
  buffers so already visible prompts refresh.
- `scripts/prepare_prompt_hud.py` fingerprints the eight HUD masks from the
  player's verified RomFS. `build/prompt-hud.tsv` contains dimensions and hashes,
  not images. Build and runtime-update packaging generate this file. An older
  app without it can still use live icon-font prompts.

There is no subprocess, file regeneration, or font-cache reset when a binding
changes. The renderer consumes a new binding snapshot on its next update.

## Static icon mods

The existing generator also accepts a saved keyboard layout:

```sh
python3.14 games/totk/scripts/make_button_icons.py <mod-folder> \
  --app <game.app> --version v143 --style keyboard \
  --bindings <keyboard.txt>
```

The sibling `mouse.txt` determines whether mouse bindings are usable; `--mouse`
or `--no-mouse` overrides it. An invalid binding file stops generation before
assets are written. Static mods still need regeneration and a restart; the live
renderer does not.

## Validation

Focused tests (macOS, Python 3.14; static generator tests also use fontTools):

```sh
python3.14 -m unittest discover -s tests -p test_input_prompts.py -v
python3.14 -m unittest discover -s tests -p test_prompt_assets.py -v
python3.14 -m unittest discover -s tests -p test_button_icons_hud.py -v
python3.14 -m unittest discover -s tests -p test_builder_keyboard_layout.py -v
python3.14 -m unittest discover -s tests -p test_keyboard_map.py -v
```

The Metal tests use synthetic textures without submitting GPU work: they check
replacement on a revision, reuse on an unchanged revision, HUD fingerprint
matching, unchanged game memory and non-prompt pixels, and pool removal.
The input tests exercise keyboard/mouse fallback, controller remaps, unbound
actions, directional groups, device selection, fixed styles, and drift filtering.
Physical Xbox/DualSense testing requires those controllers and is separate from
these software checks.

Validated on macOS with Tears of the Kingdom 1.4.3 in the Room of Awakening:
the Examine popup changed from E to F after remapping Interact in Settings,
then to Xbox B and PlayStation Circle, all in the same running session. The
inventory's footer and shoulder-button prompts also updated. The full Python
suite passed (1,545 tests, two skipped), along with the native runtime build,
loader probe, app seal and self-check. A custom static mod was generated for
all four font archives and the HUD masks. No physical controllers were available;
the iOS SDK was unavailable, so an iOS build was not verified.

# Keyboard mapping (iPhone / iPad)

A Magic Keyboard, Smart Keyboard or Bluetooth keyboard drives the same virtual controller as a game controller. The
keys are physical US-QWERTY positions (a Chinese QWERTY keyboard matches the labels).

## Sticks

| Input | Key |
|---|---|
| Left stick | `W` `A` `S` `D` |
| Right stick (camera) | arrow keys |
| L3 (press the left stick) | `Z` |
| R3 (press the right stick) | `C` |

## Buttons (Nintendo layout)

| Switch button | Key |
|---|---|
| A (confirm) | `J` |
| B (cancel) | `K` or `Space` |
| X | `I` |
| Y | `U` |
| L | `Q` |
| R | `E` |
| ZL | `Option` (Alt) |
| ZR | `Shift` |
| + (Plus) | `Return` |
| − (Minus) | `Tab` or `Esc` |
| D-pad left | `F` |
| D-pad up | `T` |
| D-pad right | `H` |
| D-pad down | `G` |

The two clusters, by position:

```text
        T (up)                 U = Y    I = X
  F (left)   H (right)         J = A    K = B
        G (down)
```

## Notes

- The keyboard is detected automatically. Settings › Apps › the game's name › **Keyboard** switches it off
  entirely (touch and controllers keep working).
- While a keyboard is connected the touch layer hides. **Keep touch with a keyboard** in the same pane keeps both.
- When the game asks for text (naming a horse), the keyboard types into the system alert and is paused from game
  input until the alert closes.
- A disconnect or sleep releases every key, so a held `W` or `Space` cannot keep the game walking.
- The mapping is fixed and positional. The in-game controller layout (`controls.txt`, `TOTK_CONTROLS`) applies to
  physical controllers, not to these keys.
- The Mac build's `1` / `2` / `3` / `4` / `5` shortcuts are not mapped on iPhone or iPad. Their functions live in the
  Settings pane instead: on-screen status, frame-rate cap, picture mode, aspect ratio and edge smoothing.

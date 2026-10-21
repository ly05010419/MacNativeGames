# Game input bindings (macOS)

Open **Game Controls…** with F1 or Command+, and select **Keyboard & Mouse** or
**Controller**. This extends the control menu introduced in PR #42. The editor
configures the host controller presented to Tears of the Kingdom; it does not
modify game code, saves or the game's own options.

## Keyboard and mouse

Each of the 16 Switch buttons, four movement directions and four look directions
has two optional bindings. Click a binding, then press a key or mouse button.
Escape cancels recording; × clears a binding. Left/right Shift, Option, Control
and Command are separate keys. Physical Mac key positions are used, as before.
Regular game bindings use one key; application shortcut chords remain on the
Shortcuts page. Escape remains available as the default alternate Minus binding,
but is reserved for cancelling the editor while recording.

A physical key or mouse button can belong to only one action. Clear its old
binding before assigning it elsewhere. Plain application shortcuts and key 5
(the existing temporal-AA toggle) cannot also be game bindings. Command+Q and
Command+, keep their application functions. Use the Shortcuts tab if a plain
shortcut occupies a key you want for the game.

Enable **Hold right mouse button and drag to look** for optional mouse look.
Sensitivity ranges from 10% to 500%; Y can be inverted. Movement is converted to
the look stick and returns to neutral when dragging stops. The right button may
also be bound to a game button (for example ZL). Mouse look does not lock or hide
the cursor. It is off by default; the existing keyboard defaults and aliases
(Space for B, Escape for Minus) are retained.

## Controller

Rows identify physical positions and Switch / Xbox / PlayStation labels in that
order. Choose a Switch button or Unassigned for each physical button. Multiple
physical buttons may share one game action. Existing Nintendo face-button
normalization and the `TOTK_FACE_BUTTONS` override are retained; use the default
normalization for the position labels shown in this editor.

The existing `controls.txt` modes are editable here:

| Mode | Behavior |
| --- | --- |
| hold | Active while the physical button is held. |
| tap | One 400 ms pulse on press. |
| toggle | Press to toggle; movement returning to center also ends it. |
| aim | Hold the mapped action; pressing the selected physical fire button releases it and suppresses that fire button's ordinary mapping. |

An aim binding needs a different physical fire button. The fire selector is
only enabled for aim. You can also swap the left/right sticks and invert the
resulting look stick's Y axis. Configuration works before a controller is
connected and applies to connected and subsequently connected controllers.

## Apply, Cancel and defaults

Changes remain a draft until **Apply**. **Cancel**, closing the sheet, and
Escape outside recording discard the draft. Each input page has a reset button;
resetting also remains a draft until Apply. Switching tabs stops key recording.
While the sheet is open, game input is neutral. Applying or leaving the sheet
clears keyboard/mouse presses, controller pulses and toggles; held controller
buttons must be released and sticks centered before they resume.

The versioned input preferences live in the existing
`org.macnativegames.totk.display` preferences suite. A saved controller layout
overrides the initial `controls.txt` layout on subsequent launches; the file is
never rewritten. Without saved input preferences, the existing file/defaults
continue to apply. Reset Controller selects the built-in default layout.
Malformed stored input arrays are ignored. Test sessions use the existing
separate display test suite. This editor is macOS-only; iOS retains its existing
input configuration and touch controls.

## Core verification

- `python3 -B -m unittest discover -s tests -p 'test_control*.py'`: production
  keyboard/mouse dispatch, guest state sampling, alternate bindings, mouse axes,
  shortcut handling, suspension and validation, plus render/pacing policy.
- `python3 -B -m unittest discover -s tests -p 'test_input_settings.py'`: compile
  the actual AppKit editor and run the production controller state transitions
  with synthetic physical states (hold/tap/toggle/aim, swap/invert, neutral gates).
- On a logged-in Mac, set `TOTK_RUN_APPKIT_PROBES=1` for the same command to run
  actual controls, recording scope, Apply/Cancel/reset and preference reload.
  The probe uses only a test preferences suite and no guest, saves or GPU work.

These probes do not constitute physical-controller or in-game play testing.

## 中文使用说明

按 F1 或 Command+, 打开控制菜单，进入“键盘与鼠标”或“手柄”。键鼠每个动作可设
两个绑定，点击后按键或鼠标按钮；Esc 取消录入，× 清除。重复绑定、快捷键冲突会在
应用前提示。可开启按住鼠标右键拖动视角，并调整灵敏度、反转 Y。

手柄可按实体按钮位置调整游戏动作，支持原有 hold / tap / toggle / aim 模式，以及
左右摇杆交换、视角 Y 反转。aim 需指定另一个实体按钮开火。

“应用”后生效并保存；“取消”不改当前设置；恢复默认也需应用。打开菜单时游戏输入
保持中立，关闭后需松开手柄按钮、将摇杆回中再继续。菜单保存的手柄配置会覆盖启动时
读取的 controls.txt，文件本身不变。

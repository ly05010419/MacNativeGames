# x86recomp in windows_games/ra2

This is [x86recomp](https://github.com/JTM-rootstorm/x86recomp) (MIT, `LICENSE`) at upstream commit `1c90fe3`
("README: the project site's new address"), with WinNativeGames' changes applied, as the games are built with it.
Upstream's own `CLAUDE.md` (its author's working notes) is left out; nothing else is.

Built at configure time from the pinned sources it fetches itself: Berkeley SoftFloat 3 (`5c06db3`, BSD-3-Clause) and
CORE-MATH (`74fcd10`, MIT); see `cmake/`.

What our changes add: MMX instructions (`src/runtime/mmx.h`); DirectDraw v1/v2 interfaces and 16-bit 2D blits, colour
keys and GetDC; DirectSound v1; OLE structured storage written from the [MS-CFB] specification (`src/runtime/cfb.*`,
`src/runtime/hle/ole32_storage.cpp`); IMM; ANSI code pages through the system's iconv (`src/runtime/hle/codepage.*`);
user32 dialogs and controls; the present path, screen controls and touch gestures for iOS; resizable windows; install files a launcher puts elsewhere
(`GameConfig::install_files`); compact
movies (Bink frames re-encoded as VP9 / Opus behind the Bink header, `src/runtime/hle/bink_format.h`,
`tools/bink_decode`); lifter fixes. Wine, Bochs and
GPL code were read for behaviour only, never copied.

To see them against upstream:

```sh
git clone https://github.com/JTM-rootstorm/x86recomp /tmp/x86recomp && git -C /tmp/x86recomp checkout 1c90fe3
diff -ru -x .git -x CLAUDE.md /tmp/x86recomp windows_games/ra2/third_party/x86recomp
```

Files added (28):
- `src/runtime/cfb.cpp`
- `src/runtime/cfb.h`
- `src/runtime/gfx/overlay.cpp`
- `src/runtime/gfx/overlay.h`
- `src/runtime/gfx/shaders/overlay.frag`
- `src/runtime/gfx/shaders/overlay.vert`
- `src/runtime/hle/bink_format.h`
- `src/runtime/hle/codepage.cpp`
- `src/runtime/hle/codepage.h`
- `src/runtime/hle/comctl32.cpp`
- `src/runtime/hle/imm32.cpp`
- `src/runtime/hle/ole32.cpp`
- `src/runtime/hle/ole32_storage.cpp`
- `src/runtime/hle/screen_controls.cpp`
- `src/runtime/hle/screen_controls.h`
- `src/runtime/hle/touch_gestures.cpp`
- `src/runtime/hle/touch_gestures.h`
- `src/runtime/hle/user32_controls.cpp`
- `src/runtime/hle/user32_dialog.cpp`
- `src/runtime/hle/user32_internal.h`
- `src/runtime/mmx.h`
- `tests/bink_format_test.cpp`
- `tests/cfb_test.cpp`
- `tests/codepage_test.cpp`
- `tests/frame_layout_test.cpp`
- `tests/touch_gestures_test.cpp`
- `tools/bink_decode/CMakeLists.txt`
- `tools/bink_decode/bink_decode.cpp`

Files changed (34):
- `CMakeLists.txt`
- `src/runtime/dispatch.cpp`
- `src/runtime/gfx/device.cpp`
- `src/runtime/gfx/device.h`
- `src/runtime/gfx/gpu_device.cpp`
- `src/runtime/hle/advapi32.cpp`
- `src/runtime/hle/binkw32.cpp`
- `src/runtime/hle/ddraw.cpp`
- `src/runtime/hle/ddraw_impl.h`
- `src/runtime/hle/ddraw_types.h`
- `src/runtime/hle/dsound.cpp`
- `src/runtime/hle/hle.cpp`
- `src/runtime/hle/kernel32.cpp`
- `src/runtime/hle/kernel32_file.cpp`
- `src/runtime/hle/kernel32_misc.cpp`
- `src/runtime/hle/kernel32_state.h`
- `src/runtime/hle/kernel32_sync.cpp`
- `src/runtime/hle/present.cpp`
- `src/runtime/hle/present.h`
- `src/runtime/hle/user32_window.cpp`
- `src/runtime/hle/user32_window.h`
- `src/runtime/hle/win32.h`
- `src/runtime/hle/ws2_32.cpp`
- `src/runtime/lifted.h`
- `src/runtime/platform.cpp`
- `src/runtime/platform.h`
- `src/runtime/process.cpp`
- `src/runtime/runtime.h`
- `tests/CMakeLists.txt`
- `tools/lifter/emit.cpp`
- `tools/lifter/function.cpp`
- `tools/lifter/main.cpp`
- `tools/lifter/program.cpp`
- `tools/lifter/program.h`

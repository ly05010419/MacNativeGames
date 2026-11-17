"""Echoes of Wisdom's settings panel (key 1): the shared panel (shared/runtime/settings_panel.h) with this game's rows and pages
(runtime/settings_game.h), exercised in AppKit by tests/settings_panel/probe.m without game data, a guest or the GPU, as
games/acnh/tests/test_acnh_settings_panel.py does for Animal Crossing. The board, screenshot, nickname and homepage
addresses point at a closed local port (127.0.0.1:9), so nothing reaches the network."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
ROOT = GAME.parents[1]
GFX = GAME / "third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"), "requires macOS and Xcode")
class EchoesSettingsPanel(unittest.TestCase):
    def test_panel_pages_rows_and_restore(self):
        with tempfile.TemporaryDirectory(prefix="eow-settings-") as directory:
            root = Path(directory)
            settings, build = root / "settings", root / "build"
            settings.mkdir()
            build.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith(("TOTK_", "EOW_", "BY3_", "DM_"))}
            env.update(SETTINGS_TEST_DIR=str(settings), TOTK_UI_LANG="en")
            exe = build / "panel"
            command = ["xcrun", "clang", "-std=c11", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                       "-Wno-unused-function", "-I" + str(GFX / "include"), "-Wl,-undefined,dynamic_lookup", "-Wl,-dead_strip",
                       str(GAME / "tests/settings_panel/probe.m"), str(GAME / "runtime/hid_feedback.c"),
                       str(GFX / "fps_overlay.c"), str(GFX / "totk_mfx_up.m"),
                       "-framework", "Cocoa", "-framework", "Metal", "-framework", "MetalFX", "-framework", "QuartzCore",
                       "-framework", "CoreVideo", "-framework", "IOKit", "-framework", "GameController", "-o", str(exe)]
            made = subprocess.run(command, capture_output=True, text=True, timeout=180)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            for layout in ("compact", "corners"):
                with self.subTest(layout=layout):
                    for name in ("graphics-overlay.txt", "graphics-output.txt", "keyboard.txt", "rumble.txt"):
                        (settings / name).unlink(missing_ok=True)
                    run = subprocess.run([str(exe)], env=dict(env, TOTK_HUD=layout), capture_output=True, text=True, timeout=60)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn("pass: settings panel", run.stdout)
            self.assertEqual((settings / "graphics-overlay.txt").read_text(), "3\n")   # FPS full information
            self.assertEqual((settings / "graphics-output.txt").read_text(), "1\n")    # MetalFX upscaling
            self.assertEqual((settings / "rumble.txt").read_text(), "on\n")
            self.assertIn("\na = m\n", (settings / "keyboard.txt").read_text())
            self.assertTrue((settings / "panel.png").stat().st_size > 0)
            run = subprocess.run([str(exe), "restore"], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("pass: settings restore", run.stdout)


class EchoesPanelSources(unittest.TestCase):
    """The shared files are links, the game's own part is its settings_game.h (shared/README.md)."""

    def test_links_to_the_shared_files(self):
        runtime = ("settings_panel.h", "hud_box.h", "screenshot.h", "board_upload.h", "board_tab.h", "nick_row.h",
                   "shot_upload.h", "shot_tab.h", "dsu_client.h", "dsu_protocol.h", "dsu_settings_rows.h")
        gfx = ("fps_overlay.c", "fps_overlay.h", "fps_board.c", "fps_board.h", "nvn_capture_region.h", "include/totk_ui_lang.h")
        for path, shared in [(f"runtime/{n}", f"shared/runtime/{n}") for n in runtime] + \
                            [(f"third_party/d2r_gfx/gfx/nvn/{n}", f"shared/third_party/d2r_gfx/gfx/nvn/{n}") for n in gfx]:
            with self.subTest(path=path):
                link = GAME / path
                self.assertTrue(link.is_symlink())
                self.assertFalse(os.path.isabs(os.readlink(link)))   # relative: any checkout
                self.assertEqual(link.resolve(), (ROOT / shared).resolve())

    def test_pages_and_saves(self):
        game = (GAME / "runtime/settings_game.h").read_text()
        self.assertEqual(sorted(re.findall(r"#define (SETTINGS_HAS_\w+) 1", game)),
                         ["SETTINGS_HAS_BOARD", "SETTINGS_HAS_EFFECTS", "SETTINGS_HAS_SHOTS"])
        self.assertIn("#define SETTINGS_HAS_CONTRIB 0", game)
        self.assertNotIn("SETTINGS_PANEL_ALPHA", game)   # the Zelda games' opacity
        self.assertNotIn("SETTINGS_HAS_USERS", game.split("*/", 1)[1])   # no User page: the saves keep their one account
        self.assertFalse((GAME / "runtime/account.c").is_symlink())    # the game's own single-user account.c
        self.assertFalse((GAME / "runtime/users.h").exists())
        self.assertFalse((GAME / "runtime/contrib_tab.h").exists())
        self.assertIn('{KEYMAP_SCREENSHOT,"截图","Screenshot",NULL}', game)
        self.assertIn("v<=HUD_MODE_FULL)fps_overlay_set_hud_mode(v)", game)
        self.assertIn('#define GAME_INBOX_ID "eow"', (GFX / "totk_contrib_upload.h").read_text())

    def test_launcher_names_the_board_folder(self):
        launcher = (GAME / "app/launcher.c").read_text()
        self.assertIn('setenv("TOTK_BOARD_DIR", data, 0);', launcher)
        self.assertNotIn("TOTK_SHADER_CONTRIB", launcher)   # no contribution file: no 着色器贡献 page

    def test_graphics_layer_feeds_the_box_board_and_screenshot(self):
        mtl, hle = (GFX / "nvn_mtl.m").read_text(), (GFX / "hle_nvn.c").read_text()
        self.assertIn("hitch_overlay_note(&h, clock_gettime_nsec_np(CLOCK_UPTIME_RAW))", hle)   # the long-frame ring's hitches
        self.assertIn("void nvn_board_pacing(int *lock, int *interp)", hle)
        for needle in ("hitch_overlay_publish(now);", "fps_board_start();", "void mtl_grab_next_game_frame(",
                       "void mtl_board_view(", "fps_board_note(ticket, now);", "int flip = origin_ll();"):
            self.assertIn(needle, mtl)
        self.assertNotIn("runtime_shortcut_help_toggle", (GAME / "runtime/frontend.m").read_text())   # the old reminder box


if __name__ == "__main__":
    unittest.main()

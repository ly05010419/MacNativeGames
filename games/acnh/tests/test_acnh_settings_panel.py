"""Animal Crossing's settings panel (key 1): the shared panel (shared/runtime/settings_panel.h) with this game's rows and pages
(runtime/settings_game.h), exercised in AppKit by tests/settings_panel/probe.m without game data, a guest or the GPU, as
games/odyssey/tests/test_settings_panel.py does for Odyssey. The board, screenshot, nickname and homepage addresses point
at a closed local port, so nothing reaches the network."""
import hashlib
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
VERSION_303 = 2228224


def frame_rate_assets(root):
    """Prepared 60 and 120 FPS data as build_host.py pins it (synthetic: the panel never installs it)."""
    header = "#define ACNH_FPS_ENABLED 1\n#define ACNH_FPS_PATCH_COUNT 1\n#define ACNH_FPS120_ENABLED 1\n#define ACNH_FPS120_PATCH_COUNT 1\n"
    env = {}
    for rate, prefix, variable in ((60, "ACNH_FPS", "ACNH_FPS_ASSETS"), (120, "ACNH_FPS120", "ACNH_FPS120_ASSETS")):
        folder = root / f"fps{rate}"
        (folder / "romfs").mkdir(parents=True)
        (folder / "manifest.json").write_bytes(f"synthetic {rate} identity\n".encode())
        (folder / "patches.tsv").write_text("MNG_ACNH_FPS1\n0 00000001 0000000a\n")
        (folder / "romfs/manifest.tsv").write_bytes(f"synthetic {rate} overlay\n".encode())
        for key, name in (("MANIFEST", "manifest.json"), ("PATCH", "patches.tsv"), ("ROMFS", "romfs/manifest.tsv")):
            header += f'#define {prefix}_{key}_SHA "{hashlib.sha256((folder / name).read_bytes()).hexdigest()}"\n'
        env[variable] = str(folder)
    (root / "fps_config.h").write_text(header)
    return env


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"), "requires macOS and Xcode")
class AnimalCrossingSettingsPanel(unittest.TestCase):
    def test_panel_pages_rows_and_restore(self):
        with tempfile.TemporaryDirectory(prefix="acnh-settings-") as directory:
            root = Path(directory)
            settings, build = root / "settings", root / "build"
            settings.mkdir()
            build.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith(("TOTK_", "ACNH_", "BY3_", "DM_"))}
            env.update(frame_rate_assets(build), SETTINGS_TEST_DIR=str(settings), TOTK_UI_LANG="en")
            exe = build / "panel"
            command = ["xcrun", "clang", "-std=c11", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                       "-Wno-unused-function", f"-DACNH_VERSION_RAW={VERSION_303}", "-I" + str(GFX / "include"), "-I" + str(build),
                       "-Wl,-undefined,dynamic_lookup", "-Wl,-dead_strip",
                       str(GAME / "tests/settings_panel/probe.m"), str(GAME / "runtime/frame_rate.c"),
                       str(GAME / "runtime/hid_feedback.c"), str(GFX / "fps_overlay.c"), str(GFX / "totk_mfx_up.m"),
                       "-framework", "Cocoa", "-framework", "Metal", "-framework", "MetalFX", "-framework", "QuartzCore",
                       "-framework", "CoreVideo", "-framework", "IOKit", "-framework", "GameController", "-o", str(exe)]
            made = subprocess.run(command, capture_output=True, text=True, timeout=180)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            (settings / "frame-rate.txt").write_text("120\n")   # chosen on a 120 Hz screen; this one shows 60
            for layout in ("compact", "corners"):
                with self.subTest(layout=layout):
                    for name in ("graphics-overlay.txt", "graphics-output.txt"):
                        (settings / name).unlink(missing_ok=True)
                    (settings / "frame-rate.txt").write_text("120\n")
                    run = subprocess.run([str(exe)], env=dict(env, TOTK_HUD=layout), capture_output=True, text=True, timeout=60)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn("pass: settings panel", run.stdout)
            self.assertEqual((settings / "graphics-overlay.txt").read_text(), "3\n")   # FPS full information
            self.assertEqual((settings / "graphics-output.txt").read_text(), "1\n")    # MetalFX upscaling
            self.assertEqual((settings / "frame-rate.txt").read_text(), "60\n")
            self.assertTrue((settings / "panel.png").stat().st_size > 0)
            self.assertTrue((settings / "user-page.png").stat().st_size > 0)   # the User page as drawn
            run = subprocess.run([str(exe), "restore"], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("pass: settings restore", run.stdout)


class AnimalCrossingPanelSources(unittest.TestCase):
    """The shared files are links, the game's own part is its settings_game.h (shared/README.md)."""

    def test_links_to_the_shared_files(self):
        for path, shared in (("runtime/settings_panel.h", "shared/runtime/settings_panel.h"), ("runtime/hud_box.h", "shared/runtime/hud_box.h"),
                             ("runtime/screenshot.h", "shared/runtime/screenshot.h"), ("runtime/dsu_client.h", "shared/runtime/dsu_client.h"),
                             ("third_party/d2r_gfx/gfx/nvn/fps_overlay.c", "shared/third_party/d2r_gfx/gfx/nvn/fps_overlay.c"),
                             ("third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h", "shared/third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h")):
            with self.subTest(path=path):
                link = GAME / path
                self.assertTrue(link.is_symlink())
                self.assertFalse(os.path.isabs(os.readlink(link)))   # relative: any checkout
                self.assertEqual(link.resolve(), (ROOT / shared).resolve())

    def test_pages_and_saves(self):
        game = (GAME / "runtime/settings_game.h").read_text()
        self.assertEqual(sorted(re.findall(r"#define (SETTINGS_HAS_\w+) 1", game)),
                         ["SETTINGS_HAS_BOARD", "SETTINGS_HAS_EFFECTS", "SETTINGS_HAS_SHOTS", "SETTINGS_HAS_USERS"])
        self.assertIn("#define SETTINGS_HAS_CONTRIB 0", game)
        self.assertTrue((GAME / "runtime/users.h").is_symlink())   # the user store's header, shared/runtime/users.h
        self.assertEqual((GAME / "runtime/users.h").resolve(), (ROOT / "shared/runtime/users.h").resolve())
        self.assertTrue((GAME / "runtime/account.c").is_symlink())   # the shared store, with its GAME_ACNH entries
        self.assertEqual((GAME / "runtime/account.c").resolve(), (ROOT / "shared/runtime/account.c").resolve())
        self.assertIn('{KEYMAP_SCREENSHOT,"截图","Screenshot",NULL}', game)
        self.assertIn("v<=HUD_MODE_BARE)fps_overlay_set_hud_mode(v)", game)   # every overlay choice, Bare FPS too (2026-11-18)
        self.assertIn('#define GAME_INBOX_ID "acnh"', (GFX / "totk_contrib_upload.h").read_text())


if __name__ == "__main__":
    unittest.main()

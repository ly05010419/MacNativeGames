#!/usr/bin/env python3
"""Bounded, isolated local test launcher for the Odyssey native host.

Plays from the app's own test profile (Odyssey_build/manual-test/profile). On first use, when that profile holds no
save, it is seeded from the Cap Kingdom baseline (Odyssey_build/profiles/cap-kingdom-start), so Resume continues in
Cap Kingdom; an existing save is never replaced. Recorded shaders and pipelines are warmed before play.
The session closes itself after ODYSSEY_TEST_MINUTES (default 20).
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    game = Path(__file__).resolve().parents[1]
    build = game / "Odyssey_build"
    executable = build / "build/odyssey-native"
    rewritten = build / "rewritten/local"
    if not executable.is_file() or not rewritten.exists():
        print("The Odyssey test build is missing. Build the Odyssey Native scheme in Xcode first.")
        return 1

    test = build / "manual-test"
    test.mkdir(exist_ok=True)
    seed_save(test / "profile", build / "profiles/cap-kingdom-start")
    minutes = float(os.environ.get("ODYSSEY_TEST_MINUTES", "20"))
    commands = test / "commands.txt"
    commands.write_text("")
    log_path = test / "last-run.log"
    env = os.environ.copy()
    for diagnostic in ("TOTK_ASSUME_FOCUS", "TOTK_VERIFY_STARTUP", "TOTK_HEADLESS"):
        env.pop(diagnostic, None)
    env.update(
        TOTK_GPU_ALLOW_SUBMIT="1",
        TOTK_GPU_WIDEN="1",
        TOTK_GPU_BLOCKRT="1",
        TOTK_INPUT_FILE=str(commands),
        TOTK_PROFILE_DIR=str(test / "profile"),
        TOTK_SLOT_OWNER="user",   # the user's game goes ahead of agent test runs (runtime/game_slot.c)
        TOTK_SLOT_PURPOSE="personal bounded test",
    )
    print("Opening Odyssey test window. Use a controller or the Mac keyboard.")
    print("Keyboard: W/A/S/D move, arrow keys turn the camera, J or K/Space jump (A / B), U or I throw Cappy (Y / X),")
    print("Option/Shift crouch (ZL / ZR), Q/E are L/R, Return is Plus (menu), Tab is Minus (map).")
    print(f"Choose Resume to continue in Cap Kingdom. The session closes automatically after {minutes:g} minutes.")
    print("You can close the game window earlier. This port is still under development.")
    print(f"Test log: {log_path}")
    with log_path.open("w") as log:
        process = subprocess.Popen(
            [str(executable), "rewritten/local", "boot"], cwd=build, env=env,
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            process.wait(timeout=minutes * 60)
        except subprocess.TimeoutExpired:
            print(f"{minutes:g}-minute test limit reached; asking the game window to close.")
            with commands.open("a") as output:
                output.write("close\n")
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                print("The game did not close in time. Ending this test process.")
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        except KeyboardInterrupt:
            with commands.open("a") as output:
                output.write("close\n")
            process.wait(timeout=15)
    print(f"Test ended (exit {process.returncode}).")
    return 0 if process.returncode == 0 else 1


def seed_save(profile: Path, baseline: Path) -> None:
    """Give an empty test profile the Cap Kingdom save (account id and save folder) once; never replace a save."""
    saves = profile / "save"
    if saves.is_dir() and any(f.is_file() for f in saves.rglob("*")):
        return
    if not (baseline / "save").is_dir():
        print("No Cap Kingdom save to start from; the game will offer New Game.")
        return
    profile.mkdir(parents=True, exist_ok=True)
    if (profile / "account.uid").is_file():
        shutil.copy2(profile / "account.uid", profile / "account.uid.before-seed")
    shutil.copy2(baseline / "account.uid", profile / "account.uid")
    shutil.copytree(baseline / "save", saves, dirs_exist_ok=True)
    print("Test profile seeded with the Cap Kingdom save.")


if __name__ == "__main__":
    sys.exit(main())

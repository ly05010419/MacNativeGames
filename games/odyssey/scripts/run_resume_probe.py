#!/usr/bin/env python3
"""Bounded local Odyssey probe that resumes the saved Cap Kingdom game instead of replaying the intro.

Each run copies the ignored baseline profile Odyssey_build/profiles/cap-kingdom-start (the save the game wrote in Cap
Kingdom on 22 October) into a fresh test profile, so every run starts from the same save. Startup time varies with the
shader cache, so A is tapped every 4 s until the game is in: on the title menu it confirms the default Resume entry,
in play it only jumps. Then the arrows answer the camera tutorial, and W / A / J check that Mario walks and jumps.
Inputs are timed from the first presented frame (ODYSSEY_PROBE_ANCHOR, default 14: schedule seconds minus 14 after it).
Usage: run_resume_probe.py [log name] [seconds] [explore]
"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import time

GAME = Path(__file__).resolve().parents[1]
BUILD = GAME / "Odyssey_build"
BASELINE = BUILD / "profiles" / os.environ.get("ODYSSEY_BASELINE", "cap-kingdom-start")   # or bonneton-cappy (autosave after getting Cappy)
TEST = BUILD / "capture" / time.strftime("resume-%Y%m%d-%H%M%S")   # one folder per run: captures are evidence


def main():
    if not BASELINE.is_dir():
        raise SystemExit(f"missing baseline profile {BASELINE}")
    TEST.mkdir(parents=True)
    shutil.copytree(BASELINE, TEST / "profile")
    commands = TEST / "commands.txt"
    commands.write_text("")
    env = os.environ.copy()
    env.update(TOTK_GPU_ALLOW_SUBMIT="1", TOTK_GPU_WIDEN="1", TOTK_GPU_BLOCKRT="1",
               TOTK_INPUT_FILE=str(commands), TOTK_PROFILE_DIR=str(TEST / "profile"),
               TOTK_SLOT_OWNER="probe", TOTK_SLOT_PURPOSE="bounded resume into Cap Kingdom")
    log = BUILD / (sys.argv[1] if len(sys.argv) > 1 else "boot-resume.log")
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    shot = lambda name: "shot " + str(TEST / name)
    schedule = [(t, "key 38 100") for t in range(20, 61, 4)]
    # Cap Kingdom resumes at Cappy's camera tutorial ("R Look Around", PlayGuide H.264 clip). Right-stick sweeps (arrows
    # 123-126) finish it once the stick-direction bits are reported (runtime/input.c); the "B Jump" step and the wake-up
    # scene follow, and the gameplay HUD was up by ~114 s on 22 October. Then walk (W 13, A 0, D 2), run, jump (J 38 = A).
    schedule += [(30, shot("menu-30.png")), (45, shot("load-45.png")), (62, shot("ingame-62.png")),
                 (64, "key 124 5000"), (66, shot("look-right.png")), (70, "key 126 2000"), (73, "key 125 2000"),
                 (76, "key 123 5000"), (82, shot("after-look.png")), (84, "key 13 2000"), (86, shot("jump-prompt.png")),
                 (88, "key 40 100"), (90, "key 38 100"), (92, "key 2 1500"), (100, shot("wake-100.png")),
                 (110, "key 38 100"), (114, "key 38 100"), (118, shot("play-before.png")), (120, "key 13 3000"),
                 (121.5, shot("walk-forward.png")), (123.5, shot("walk-after.png")), (125, "key 0 2000"),
                 (126.5, shot("walk-left.png")), (128, "key 38 100"), (128.3, shot("jump-air.png")),
                 (131, "key 1 2000"), (132.5, shot("walk-back.png")), (135, "key 2 2000"), (136.5, shot("walk-right.png")),
                 (total - 6, shot("end.png"))]
    if len(sys.argv) > 3 and sys.argv[3] == "explore":
        # Longer play after the HUD (~115 s): walk forward toward Bonneton with jumps and short camera turns, A at
        # intervals to advance any dialogue, and a shot every 15 s; the save files' times show any autosave.
        schedule = [item for item in schedule if item[0] < 118 or item[1].startswith("shot " + str(TEST / "end"))]
        t = 120
        while t < total - 15:
            schedule += [(t, "key 13 4000"), (t + 2, "key 40 100"), (t + 4.5, "key 38 100"), (t + 6, "key 124 400"),
                         (t + 7, "key 13 3000"), (t + 10.5, "key 38 100"), (t + 12, shot(f"explore-{t}.png"))]
            t += 15
    if os.environ.get("ODYSSEY_PROBE_VR"):
        # VR Start (audit A3: nn::pl::BeginVrMode / nn::oe::BeginBlockingHomeButton) from the title menu, where
        # "Playing in VR" is the fourth entry (capture menu-30.png, 22 October: Resume, Resume 2P, New Game, Playing in
        # VR). The A taps before 29 s only pass the splash (the menu is still on Resume at 30 s); then D-pad down (G)
        # three times, A, and A on the VR screen's Start. A shot after every press shows the path; the log must say
        # "VR mode begins" and frames must keep coming.
        schedule = [item for item in schedule if item[0] < 29 or item[1].startswith("shot " + str(TEST / "end"))]
        schedule += [(30, shot("vr-0-title.png")), (31, "key 5 150"), (32, "key 5 150"), (33, "key 5 150"),
                     (34.5, shot("vr-1-select.png")), (35, "key 38 150"), (39, shot("vr-2-screen.png")),
                     (40, "key 38 150"), (45, shot("vr-3-start.png")),
                     # Start asks whether to restrict VR mode for children six and under (22 October): Do Not
                     # Restrict is one down; this only changes the run's copied test profile.
                     (46, "key 5 150"), (47.5, shot("vr-4-restrict.png")), (48, "key 38 150"),
                     (55, shot("vr-5-vr.png")), (75, shot("vr-6-later.png")), (110, shot("vr-7-later.png"))]
    # Runtime flags before play, e.g. ODYSSEY_PROBE_FLAGS="usnap_index_check 1,pool_query_check 1" (nvn_mtl.m dm_gfx_set_flag)
    for i, flag in enumerate(f for f in os.environ.get("ODYSSEY_PROBE_FLAGS", "").split(",") if f.strip()):
        schedule.append((1 + i * 0.1, "flag " + flag.strip()))
    if os.environ.get("ODYSSEY_PROBE_CAP"):   # cap throw (U = Y, I = X) once in play, captured mid-throw
        schedule += [(63, shot("cap-before.png")), (63.5, "key 32 150"), (63.9, shot("cap-throw-y.png")),
                     (68, "key 34 150"), (68.4, shot("cap-throw-x.png"))]
    schedule.sort(key=lambda item: item[0])
    # Event-driven timing (task 0g, from Applier's run_mfx_ab.py): the schedule's seconds were tuned for a game whose first
    # frame came ~14 s after launch, so each entry fires at first presented frame + (seconds - 14). A late start (cold
    # Metal cache after a rebuild, or a wait for the game slot) can no longer push presses into the title menu. The
    # binary's SHA-256 is recorded at start and end, so a rebuild during the run is visible.
    anchor = float(os.environ.get("ODYSSEY_PROBE_ANCHOR", "14"))
    binary = BUILD / "build/odyssey-native"
    digest_at_start = hashlib.sha256(binary.read_bytes()).hexdigest()
    (TEST / "binary.sha256").write_text(digest_at_start + "\n")
    with log.open("w") as output:
        process = subprocess.Popen([str(binary), "rewritten/local", "boot"],
                                   cwd=BUILD, env=env, stdout=output, stderr=subprocess.STDOUT)
        start = time.monotonic(); first = None; queue = []
        try:
            while process.poll() is None and time.monotonic() - start < total + 400:
                if first is None and b"first presented frame" in log.read_bytes():
                    first = time.monotonic(); queue = list(schedule)
                if first is not None:
                    elapsed = time.monotonic() - first + anchor
                    while queue and elapsed >= queue[0][0]:
                        with commands.open("a") as stream:
                            stream.write(queue.pop(0)[1] + "\n")
                    if elapsed >= total:
                        break
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                with commands.open("a") as stream:
                    stream.write("close\n")
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
    changed = hashlib.sha256(binary.read_bytes()).hexdigest() != digest_at_start
    print(f"first presented frame after {None if first is None else round(first - start, 1)} s; "
          f"{'BINARY CHANGED DURING RUN' if changed else 'binary unchanged'}")
    print(f"Probe exit {process.returncode}; local log: {log}; captures: {TEST}")
    return process.returncode


if __name__ == "__main__":
    raise SystemExit(main())

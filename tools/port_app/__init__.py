"""What the ports MNG Launcher builds through their own games/<game>/build.py share: Super Mario Odyssey, Animal Crossing
and Echoes of Wisdom (2026-11-18; until then each carried a copy).

  adapter.py  the launcher's side of build.py: arguments, the work folder, its venv and the run inside it, stage lines,
              the express update's entry (--update-app, update_runtime.py)
  bundle.py   the packager's plumbing: APFS clones, the launcher compiled, the icon, Info.plist and its update keys, the
              ad hoc signature with the bundle ID as designated requirement, staging beside the app, Launch Services

Each game's own steps, files, checks and tuning stay in its folder. The C side of the app launcher is
shared/app/app_launcher.h (shared/ holds files games link; this folder holds Python the games import, as build_app.py
at the repository root is)."""

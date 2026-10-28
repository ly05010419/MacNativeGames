#!/usr/bin/env python3
"""Build the Mario Kart 8 Deluxe native runtime from your own dump.

    .venv/bin/python scripts/build_mk8d.py --game BASE.nsp --update UPDATE.nsp --keys prod.keys \\
        --firmware FIRMWARE_NCA_DIR --work WORK

The base game and update 4.0.1 may each be an NSP or an NSZ. Steps (each is skipped when its verified output already exists,
so a failed run can simply be started again):
 0. NSZ inputs are expanded to NSP (nsz -D, which checks every NCA's hash); the base and the update are joined into one NSP
 1. check the input: title, and the update's Control data names 4.0.1
 2. extract the program modules (main, sdk, subsdk0) and merge the base + update RomFS (all six IVFC levels verified)
 3. extract the system fonts from your firmware's NCA files
 4. rewrite the ARM64 modules ahead of time (third_party/d2r/patch_nso.py, native ruleset)
 5. loader tables: verifies ExeFS, rewrite and RomFS against assets/expected-4.0.1.json
 6. shader tools (pinned SPIRV-Cross) and the runtime
 7. the app icon, read from the update's Control data (for `make app`, scripts/package_mk8d_app.py)
 8. the whole-game shader seed (scripts/prepare_mk8d_shader_seed.py) and its Metal libraries
    (scripts/precompile_shader_libs.py): every shader translated and compiled now instead of the first time a scene
    shows it in play. The longest step on the first build, reused after; --skip-seed leaves it out.

Everything written stays in the work folder, outside the checkout (or an ignored *_build folder of games/mk8d): it holds
game data, so never share or commit it. `make app WORK=…` packages the app.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from mk8d import GAME as GAME_DIR, VERSION

SPIRV_CROSS = ("https://github.com/KhronosGroup/SPIRV-Cross.git", "aa217aeb6c9f0ace7a0ab233b28807edf45eb165")
MODULES = ("main", "sdk", "subsdk0")


def step(n, title):
    print(f"\n== {n}. {title}", flush=True)
    return time.monotonic()


def done(t0, note=""):
    print(f"   done in {time.monotonic() - t0:.0f} s{(' — ' + note) if note else ''}", flush=True)


def run(cmd, env=None, cwd=GAME_DIR):
    print("   $ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=cwd, env={**os.environ, **(env or {})})


def link(path: Path, target: str):
    if path.is_symlink() or path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(target, path)


def expand_nsz(game: Path, keys: Path, work: Path, py: str) -> Path:
    """An NSZ becomes <work>/input/containers/<name>.nsp, reused while the source keeps its path, size and time."""
    if game.suffix.lower() != ".nsz":
        return game
    folder = work / "input/containers"
    output, manifest = folder / f"{game.stem}.nsp", folder / f"{game.stem}.json"
    stat = game.stat()
    source = dict(source=str(game), source_size=stat.st_size, source_mtime_ns=stat.st_mtime_ns)
    if output.is_file() and manifest.is_file() and json.loads(manifest.read_text()) == source:
        print(f"   {game.name}: already expanded")
        return output
    folder.mkdir(parents=True, exist_ok=True)
    for stale in (output, manifest):
        stale.unlink(missing_ok=True)
    run([Path(py).parent / "nsz", "-D", "--keys", keys, "-o", folder, game])   # the nsz console script of this Python
    if not output.is_file():
        raise SystemExit(f"cannot build:\n - nsz did not produce {output}")
    manifest.write_text(json.dumps(source, indent=2) + "\n")
    return output


def icloud_synced(path: Path) -> bool:
    """iCloud Drive itself, or Desktop / Documents while macOS syncs them to it."""
    home = Path.home()
    if home / "Library/Mobile Documents" in path.parents:
        return True
    if not any(folder in path.parents or folder == path for folder in (home / "Desktop", home / "Documents")):
        return False
    synced = subprocess.run(["defaults", "read", "com.apple.finder", "FXICloudDriveDocuments"], capture_output=True, text=True)
    return synced.stdout.strip() == "1"


def preflight(args, py):
    problems = []
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        problems.append("an Apple silicon Mac is required")
    if not (Path(py).parent / "nsz").is_file():
        problems.append(f"the nsz command is missing beside {py} ({py} -m pip install -r requirements.txt)")
    for name in ("capstone", "cryptography", "lz4"):
        if subprocess.run([py, "-c", f"import {name}"], capture_output=True).returncode:
            problems.append(f"Python module {name} is missing ({py} -m pip install -r requirements.txt)")
    for lib in ("/opt/homebrew/opt/opus/lib/libopus.a", "/opt/homebrew/opt/libvpx/lib/libvpx.a"):
        if not Path(lib).exists():
            problems.append(f"{lib} is missing (brew install opus libvpx)")
    if not Path(args.developer_dir, "usr/bin/xcodebuild").exists():
        problems.append(f"Xcode not found at {args.developer_dir} (--developer-dir)")
    for path in (args.game, args.update, args.keys):
        if not path.is_file():
            problems.append(f"{path} not found")
    if not args.firmware.is_dir():
        problems.append(f"{args.firmware} is not a folder of firmware NCA files (the system fonts come from it)")
    checkout = GAME_DIR.parents[1]
    if checkout in args.work.parents and not any(p.parent == GAME_DIR and p.name.endswith("_build") for p in {*args.work.parents, args.work}):
        problems.append(f"--work must be outside the checkout, or an ignored *_build folder of games/mk8d: {args.work}")
    if icloud_synced(args.work):
        problems.append(f"--work is inside iCloud Drive, which would upload the game data (tens of GB) and keep staged copies in "
                        f"~/Library/Application Support/CloudDocs: pick a folder outside it, e.g. ~/mk8d-work ({args.work})")
    if " " in os.path.relpath(args.work, GAME_DIR):
        problems.append(f"make cannot build into a folder whose path has a space: {args.work}")
    # About 25 GB from NSPs (the joined NSP and the merged RomFS, 12 GB each), 12 GB more for expanded NSZs.
    need = 5 if (args.work / "extracted/local/romfs/index.json").is_file() else 37 if args.game.suffix.lower() == ".nsz" else 25
    free = shutil.disk_usage(args.work if args.work.exists() else args.work.parent).free / 2**30
    if free < need:
        problems.append(f"only {free:.0f} GB free; about {need} GB more are needed")
    if problems:
        raise SystemExit("cannot build:\n - " + "\n - ".join(problems))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", type=Path, required=True, help="your Mario Kart 8 Deluxe base game, NSP or NSZ")
    p.add_argument("--update", type=Path, required=True, help=f"update {VERSION} (v1507328), NSP or NSZ")
    p.add_argument("--keys", type=Path, required=True, help="prod.keys dumped from your own Switch (never copied)")
    p.add_argument("--firmware", type=Path, required=True, help="your Switch firmware as a folder of NCA files (system fonts)")
    p.add_argument("--work", type=Path, required=True, help="build folder outside the checkout")
    p.add_argument("--developer-dir", default="/Applications/Xcode.app/Contents/Developer")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    p.add_argument("--skip-seed", action="store_true", help="leave out the whole-game shader seed (more stutter on first visits)")
    args = p.parse_args()
    for name in ("game", "update", "keys", "firmware", "work"):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    py = sys.executable
    preflight(args, py)
    work, ext = args.work, args.work / "extracted"
    work.mkdir(parents=True, exist_ok=True)
    keyargs = ["--keys", args.keys]
    started = time.monotonic()

    merged = work / "input/mk8d.nsp"
    # The containers are needed only to extract: once the modules and the merged RomFS are there (step 5 checks them
    # against the pins every build), steps 0 and 1 are skipped and the expanded / joined NSPs (24 GB) may be deleted.
    extracted = (ext / "extraction-manifest.json").is_file() and (ext / "local/romfs/index.json").is_file()
    t = step(0, "Expand NSZ inputs and join the base and update NSPs")
    if extracted:
        print("   skipped: the game files are already extracted (step 5 checks them against the pins)")
    else:
        base, update = expand_nsz(args.game, args.keys, work, py), expand_nsz(args.update, args.keys, work, py)
        if not merged.is_file() or merged.stat().st_mtime < max(base.stat().st_mtime, update.stat().st_mtime):
            merged.unlink(missing_ok=True)
            run([py, "scripts/merge_nsp.py", base, update, "--output", merged])
        else:
            print("   already joined")
        done(t)

        t = step(1, "Check the input")
        run([py, "scripts/inspect_input.py", merged, *keyargs])
        run([py, "scripts/inspect_version.py", merged, *keyargs, "--output", work / "analysis/versions.json"])
        done(t)

    t = step(2, "Program modules and the merged RomFS")
    if (ext / "extraction-manifest.json").is_file():
        print("   modules already extracted")
    else:
        run([py, "scripts/extract_code.py", merged, *keyargs, "--output", ext])
    link(ext / "local/exefs", "..")
    if (ext / "local/romfs/index.json").is_file():
        print("   RomFS already merged")
    else:
        run([py, "scripts/merge_romfs.py", merged, *keyargs, "--output", ext / "local/romfs"])
    done(t)

    t = step(3, "System fonts")
    fonts = ext / "firmware/shared-fonts"
    if len(list(fonts.glob("*.bfttf"))) >= 6:
        print("   fonts already extracted")
    else:
        run([py, GAME_DIR / "third_party/totk-scripts/extract_shared_fonts.py", *keyargs, "--firmware", args.firmware,
             "--output", fonts, "--inventory", work / "analysis/firmware-inventory.json"])
    done(t)

    t = step(4, "Rewrite the ARM64 modules")
    rewritten = work / "rewritten"
    rewritten.mkdir(exist_ok=True)
    link(rewritten / "local", "../rewritten")
    for name in MODULES:
        if (rewritten / f"{name}.patched.bin").is_file():
            print(f"   {name} already rewritten")
            continue
        run([py, GAME_DIR / "third_party/d2r/patch_nso.py", ext / name, rewritten, name],
            env={"DM_RULESET": "native", "DM_PRIVATE_GATES": "0"})
    done(t)

    t = step(5, f"Loader tables (verifies every input against the pinned {VERSION} hashes)")
    run([py, "scripts/prepare_runtime.py", "--work", work])
    done(t)

    t = step(6, "Shader tools and the runtime")
    src = work / "src/SPIRV-Cross"
    if not (src / ".git").is_dir():
        run(["git", "clone", "--quiet", SPIRV_CROSS[0], src])
    run(["git", "-C", src, "checkout", "--quiet", SPIRV_CROSS[1]])
    run(["make", f"-j{args.jobs}", "runtime", f"WORK={os.path.relpath(work, GAME_DIR)}", f"PY={py}"],
        env={"DEVELOPER_DIR": args.developer_dir})
    done(t)

    t = step(7, "App icon")
    icon = work / "app-assets/icon.jpg"
    if icon.is_file():
        print("   icon already extracted")
    else:   # an NSZ keeps its small Control NCA as is, so the update is read where it is
        run([py, "scripts/extract_app_icon.py", args.update, *keyargs, "--output", icon])
    done(t)

    t = step(8, "Whole-game shader seed and its Metal libraries")
    if args.skip_seed:
        print("   skipped (--skip-seed): each shader is translated the first time a scene shows it")
    else:   # each is reused when its inputs are unchanged
        run([py, "scripts/prepare_mk8d_shader_seed.py", "--work", work])
        run([py, "scripts/precompile_shader_libs.py", "--work", work, "--jobs", min(args.jobs, 8),
             "--developer-dir", args.developer_dir])
    done(t)
    print(f"\nBuilt {work / 'build/mk8d-native'} in {(time.monotonic() - started) / 60:.0f} min. The work folder holds game data "
          f"from your own dump: do not share it.\nPlay: make run WORK={work}, or package the app: make app WORK={work}")


if __name__ == "__main__":
    main()

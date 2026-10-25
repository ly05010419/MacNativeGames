#!/usr/bin/env python3
"""Build Odyssey.app from your own Super Mario Odyssey 1.3.0 dump in one command.

    .venv/bin/python scripts/build_odyssey.py --game GAME.xci --keys prod.keys [--work Odyssey_build]

Steps (each is skipped when its verified output already exists, so a failed run can simply be started again):
 1. check the input (title, version, integrity; prints no keys or game data)
 2. extract the program modules (main, sdk, subsdk0) and merge the base + update RomFS
 3. extract the system fonts from the game card's system-update partition (NSP: pass --firmware)
 4. rewrite the ARM64 modules ahead of time (third_party/d2r/patch_nso.py, native ruleset)
 5. loader tables: verifies ExeFS, rewrite and RomFS against assets/expected-local-update.json
 6. shader tools (pinned SPIRV-Cross) and the runtime
 7. the whole-game shader seed (the longest step; --skip-seed leaves it out), then its Metal libraries, compiled ahead
    of time so the game does not compile them on the spot when a scene first shows them (needs Xcode's Metal Toolchain)
 8. extract the game's icon and package <work>/Odyssey.app, with saves, shader cache and logs in "<work>/Odyssey Data"

Everything written stays in the work folder, which Git ignores; it holds game data, so never share or commit it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[1]
GAMES = GAME_DIR.parent
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


def preflight(args, py):
    problems = []
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        problems.append("an Apple silicon Mac is required")
    for name in ("capstone", "cryptography", "lz4"):
        if subprocess.run([py, "-c", f"import {name}"], capture_output=True).returncode:
            problems.append(f"Python module {name} is missing ({py} -m pip install capstone cryptography lz4)")
    for lib in ("/opt/homebrew/opt/opus/lib/libopus.a", "/opt/homebrew/opt/libvpx/lib/libvpx.a"):
        if not Path(lib).exists():
            problems.append(f"{lib} is missing (brew install opus libvpx)")
    if not Path(args.developer_dir, "usr/bin/xcodebuild").exists():
        problems.append(f"Xcode not found at {args.developer_dir} (--developer-dir)")
    for path in (args.game, args.keys):
        if not path.is_file():
            problems.append(f"{path} not found")
    if args.game.suffix.lower() == ".nsp" and not args.firmware:
        problems.append("an NSP carries no system fonts: pass --firmware <unpacked firmware NCA directory>")
    # About 12 GB in all; the merged RomFS (5.6 GB) is most of it, so a resumed build needs less.
    need = 5 if (args.work / "extracted/local/romfs/index.json").is_file() else 12
    free = shutil.disk_usage(args.work.parent if not args.work.exists() else args.work).free / 2**30
    if free < need:
        problems.append(f"only {free:.0f} GB free; about {need} GB more are needed")
    if problems:
        raise SystemExit("cannot build:\n - " + "\n - ".join(problems))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", type=Path, required=True, help="your Odyssey XCI or NSP holding the base game and update 1.3.0")
    p.add_argument("--keys", type=Path, required=True, help="prod.keys dumped from your own Switch")
    p.add_argument("--work", type=Path, default=GAME_DIR / "Odyssey_build", help="build folder (default Odyssey_build)")
    p.add_argument("--firmware", type=Path, help="unpacked firmware NCA directory, for the system fonts of an NSP build")
    p.add_argument("--developer-dir", default="/Applications/Xcode.app/Contents/Developer")
    p.add_argument("--skip-seed", action="store_true", help="leave out the whole-game shader seed (more first-play stutter)")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = p.parse_args()
    args.game, args.keys, args.work = args.game.resolve(), args.keys.resolve(), args.work.resolve()
    py = sys.executable
    preflight(args, py)
    work, ext = args.work, args.work / "extracted"
    work.mkdir(parents=True, exist_ok=True)
    keyargs = ["--keys", args.keys]
    started = time.monotonic()

    t = step(1, "Check the input")
    run([py, "scripts/inspect_input.py", args.game, *keyargs])
    done(t)

    t = step(2, "Program modules and the merged RomFS")
    if (ext / "extraction-manifest.json").is_file():
        print("   modules already extracted")
    else:
        run([py, "scripts/extract_code.py", args.game, *keyargs, "--output", ext])
    link(ext / "local/exefs", "..")
    if (ext / "local/romfs/index.json").is_file():
        print("   RomFS already merged")
    else:
        run([py, "scripts/merge_romfs.py", args.game, *keyargs, "--output", ext / "local/romfs"])
    done(t)

    t = step(3, "System fonts")
    fonts = ext / "firmware/shared-fonts"
    if len(list(fonts.glob("*.bfttf"))) >= 6:
        print("   fonts already extracted")
    else:
        source = ["--firmware", args.firmware] if args.firmware else ["--xci", args.game]
        try:
            run([py, GAME_DIR / "third_party/totk-scripts/extract_shared_fonts.py", *keyargs, *source, "--output", fonts,
                 "--inventory", work / "analysis/firmware-inventory.json"])
        except subprocess.CalledProcessError:
            if args.firmware:
                raise
            # Not every card's system-update partition carries the font data (seen 23 October on a 1.3.0 card).
            raise SystemExit("this game card's system-update partition has no shared-font data: dump your Switch's "
                             "firmware (its NCA files) and pass the folder with --firmware, then run this again")
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

    t = step(5, "Loader tables (verifies every input against the pinned 1.3.0 hashes)")
    run([py, "scripts/prepare_runtime.py", "--work", work])
    done(t)

    t = step(6, "Shader tools and the runtime")
    src = work / "src/SPIRV-Cross"
    if not (src / ".git").is_dir():
        run(["git", "clone", "--quiet", SPIRV_CROSS[0], src])
    run(["git", "-C", src, "checkout", "--quiet", SPIRV_CROSS[1]])
    # make cannot handle spaces in target paths, so the work folder goes in relative to games/odyssey (the checkout
    # itself may sit in a folder with a space, as long as the work folder's own path below it has none).
    make_work = os.path.relpath(work, GAME_DIR)
    if " " in make_work:
        raise SystemExit(f"make cannot build into a folder whose path has a space: {make_work}; pick another --work")
    run(["make", f"-j{args.jobs}", "runtime", f"WORK={make_work}", f"PY={py}"], env={"DEVELOPER_DIR": args.developer_dir})
    done(t)

    seed = work / "build/shader-seed"
    t = step(7, "Whole-game shader seed")
    if args.skip_seed:
        print("   skipped (--skip-seed)")
    elif (seed / "seed-manifest.json").is_file():
        print("   seed already prepared")
    else:
        env = {"ODYSSEY_WORK": str(work)}
        try:
            run([py, "scripts/prepare_odyssey_shader_seed.py", "--output", seed], env=env)
        except subprocess.CalledProcessError:
            # The shared tool can fail in its last step after translating everything (seen 22 October); publish the
            # translated partial seed and finish it, as prepare_odyssey_shader_seed.py --salvage does.
            print("   the seed's last step failed; salvaging the translated shaders")
            run([py, "scripts/prepare_odyssey_shader_seed.py", "--output", seed, "--salvage"], env=env)
        run([py, "scripts/prepare_odyssey_shader_seed.py", "--output", seed, "--rules"], env=env)
    if not args.skip_seed and (seed / "seed-manifest.json").is_file():
        # Without these the runtime compiles every Metal library from source the first time a scene shows it: about 11 s
        # for the ~70 new shaders at the end of the Sand Kingdom arrival cutscene (24 October, an app built by this
        # script before this step existed). package_native_app.py puts them in the app. Already compiled ones are kept.
        try:
            run([py, "scripts/precompile_cached_shaders.py", "--cache", seed, "--jobs", min(max(args.jobs, 1), 8)],
                env={"ODYSSEY_WORK": str(work), "DEVELOPER_DIR": args.developer_dir})
        except subprocess.CalledProcessError:
            print("   some Metal libraries were not precompiled (see build/shader-libs/compile-failures.log, or install "
                  "Xcode's Metal Toolchain); the game compiles those on first use")
    done(t)

    t = step(8, "Package the app")
    icon = work / "app-assets/OdysseyIcon.jpg"
    if not icon.is_file():   # the game's own icon, from its Control data
        run([py, "scripts/extract_app_icon.py", args.game, *keyargs, "--output", icon])
    run([py, "scripts/package_native_app.py"], env={"ODYSSEY_WORK": str(work)})
    done(t)
    print(f"\nBuilt {work / 'Odyssey.app'} in {(time.monotonic() - started) / 60:.0f} min. It contains game data from your "
          "own dump: do not share it.")


if __name__ == "__main__":
    main()

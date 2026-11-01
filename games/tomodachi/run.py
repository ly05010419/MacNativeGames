#!/usr/bin/env python3
"""Run a bounded Tomodachi native diagnostic with an isolated, disposable profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from games.tomodachi.tools.common import outside_repository


def run(work, mode, seconds, app_pool_mib=None, backend="metal3"):
    if backend not in ("metal3", "metal4"): raise ValueError("Unknown renderer backend")
    work = outside_repository(work, ROOT)
    if seconds <= 0 or seconds > 600:
        raise ValueError("Diagnostic duration must be between 1 and 600 seconds")
    if app_pool_mib is not None and not 1024<=app_pool_mib<=4096:
        raise ValueError("Application pool must be 1024..4096 MiB")
    info = json.loads((work / "host-info.json").read_text())
    if info["title_id"] != "010051F0207B2000":
        raise ValueError("Wrong host title")
    prepared = Path(info["prepared"])
    tests = work / "tests"
    tests.mkdir(exist_ok=True)
    session = Path(tempfile.mkdtemp(prefix=f"{mode}-", dir=tests))
    # Never inherit another game's save path, input script, patches or instrumentation.
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("TOTK_", "DM_"))}
    environment.update(TOTK_PROFILE_DIR=str(session / "profile"),
                       TOTK_ROMFS=str(prepared / "romfs"), TOTK_ROMFS_PATCHES="0",
                       TOTK_GAME_SLOT_DIR=str(session / "slot"), TOTK_SLOT_OWNER="codex",
                       TOTK_ASSUME_FOCUS="1",
                       TOTK_SHADER_CACHE=str(work / "build/shader-cache"))
    if mode == "startup":
        environment.update(TOTK_HEADLESS="1", TOTK_VERIFY_STARTUP="1")
    if mode == "boot":
        environment["TOTK_ATT_SNAPSHOT"]="1"
        environment["TOTK_GPU_BLOCKRT"]="1"
        environment["TOTK_GPU_WIDEN"]="1"
        environment["TOTK_GPU_ALLOW_SUBMIT"]="1"
        environment["TOTK_ASYNC_REPLAY"]="0"
        environment["TOTK_GPU_KEEPALIVE"]="0"
        environment["TOTK_GPU_MIDCLEAR"]="1"
        environment["DM_NVN_SHOT"]=f"1,10,60,180:{session}/frame-%d.png"
        environment["DM_NVN_SHOT_FRAME"]="1"
        environment["TOTK_MEM_LOG"]="1"
        if app_pool_mib is not None:environment["TOTK_APP_POOL"]=str(app_pool_mib<<20)
    command = [str(work / "build" / ("tomodachi-native-metal4" if backend == "metal4" else "tomodachi-native")), str(work / "rewritten"),
               "boot" if mode in ("boot", "startup") else mode]
    timed_out = False
    with (session / "runtime.log").open("w") as log:
        process = subprocess.Popen(command, cwd=work, env=environment, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            try:
                code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
    result = {"mode": mode, "exit_code": code, "timed_out": timed_out,
              "log": str(session / "runtime.log"), "profile": str(session / "profile"),
              "scope": "Diagnostic only; exit status does not establish playable rendering or save compatibility"}
    (session / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 2 if timed_out else (0 if code == 0 else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("metal3", "metal4"), default="metal3")
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--mode", choices=("probe", "probe-gpu-midclear", "probe-gpu-blockrt", "probe-gpu-widen", "probe-gpu-storage-view", "startup", "boot"), default="probe")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--app-pool-mib", type=int, help="diagnostic application-memory budget")
    args = parser.parse_args()
    try:
        raise SystemExit(run(args.work, args.mode, args.seconds,args.app_pool_mib,args.backend))
    except (ValueError, OSError, KeyError) as error:
        parser.exit(2, f"Tomodachi diagnostic failed: {error}\n")

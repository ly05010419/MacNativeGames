#!/usr/bin/env python3
"""Run a bounded AoC diagnostic with a new disposable profile and exact records."""
import argparse
import fcntl
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import time

from build_aoc import GAME, TITLE


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--session',required=True,help='new session name under WORK/validation')
    parser.add_argument('--seconds',type=float,default=60)
    parser.add_argument('--shader-cache',type=Path,help='explicit diagnostic cache; default full offline build')
    parser.add_argument('--host-from',type=Path,help='explicit prior host snapshot for a recorded regression run')
    parser.add_argument('--fps',type=int,choices=(0,30,60),help='AoC simulation/presentation target; 0 keeps original timing; 120 is disabled pending gameplay timing fixes')
    parser.add_argument('--fixed-step',action='store_true',help='diagnostic: use 1/target instead of measured dynamic step')
    parser.add_argument('--window',action='store_true')
    parser.add_argument('--audio',action='store_true')
    parser.add_argument('--save-from',type=Path,help='copy an existing validation save into this disposable session')
    parser.add_argument('--keys',help='scripted controller actions (DM_KEYS), not a game keys file')
    args=parser.parse_args()
    w=args.work_dir.expanduser().resolve();b=w/'build'
    if w==GAME.parents[1] or GAME.parents[1] in w.parents:
        parser.error('work must be outside the checkout')
    if Path(args.session).name != args.session or args.session in ('.','..') or args.seconds<=0:
        parser.error('use a simple new session name and a positive duration')
    cache=(args.shader_cache or b/'shader/cache').expanduser().resolve()
    host=(args.host_from or b/'tools/dmhost').expanduser().resolve()
    r=w/'validation'/args.session;r.mkdir(parents=True,exist_ok=False)
    for d in ('save','temp'):(r/d).mkdir()
    if args.save_from:
        shutil.copytree(args.save_from.expanduser().resolve(),r/'save',dirs_exist_ok=True)
    subprocess.run(['cp','-cR',str(w/f'source/dm/games/{TITLE}'),str(r/'profile')],check=True)
    ctl=r/'control.txt';ctl.touch()
    e=dict(os.environ,DM_ROOT=str(b),DM_ROMFS=str(b/'extract/v327680/RomFS'),
           DM_SAVE=str(r/'save'),DM_TEMP=str(r/'temp'),DM_MSL_DIR=str(cache),
           DM_FONT_DIR=str(b/'font'),DM_NVN_SHMAP=str(r/'profile/shader_map.tsv'),
           DM_LANG='en-US',DM_METAL_RUNTIME='0',DM_BOOTPROF='1',DM_CTL=str(ctl),
           DM_PSO_RECORD=str(r/'pso-record.tsv'),DM_NVN_TSV=str(r/'frames.tsv'),DM_NVN_TSV_CALLS='0',
           DM_WINDOW=str(int(args.window)),DM_AUDIO=str(int(args.audio)))
    if not args.window:e.pop('DM_WINDOW',None)
    if args.keys:e['DM_KEYS']=args.keys
    if args.fps is not None:e['DM_FPS_TARGET']=str(args.fps)
    if args.fixed_step:e['DM_DYNAMIC_FPS']='0'
    # One diagnostic at a time across disposable profiles. Existing user games
    # are detected, never killed to make room for a test.
    with (w/'diagnostic.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        processes=subprocess.check_output(['pgrep','-x','dmhost'],text=True) if subprocess.run(['pgrep','-x','dmhost'],capture_output=True).returncode==0 else ''
        if processes.strip():raise ValueError('a dmhost is already running; finish it before this diagnostic')
        # Freeze the executable and rewrite outputs for this run. A later build
        # cannot replace the executable currently mapped by the diagnostic.
        shutil.copy2(host,r/'dmhost')
        subprocess.run(['cp','-cR',str(b/'patched_v327680'),str(r/'patched')],check=True)
        command=[str(r/'dmhost'),str(r/'patched')]
        record=dict(command=command,environment={k:v for k,v in e.items() if k.startswith('DM_')},
                    host_sha256=hashlib.sha256((r/'dmhost').read_bytes()).hexdigest(),
                    source_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=GAME,text=True).strip(),
                    duration_limit=args.seconds)
        from inspect_inputs import sha256
        record['rewritten_outputs']={p.name:sha256(p) for p in sorted((r/'patched').iterdir()) if p.is_file()}
        record['profile_files']={p.relative_to(r/'profile').as_posix():sha256(p) for p in sorted((r/'profile').rglob('*')) if p.is_file()}
        record['initial_save']={p.relative_to(r/'save').as_posix():sha256(p) for p in sorted((r/'save').rglob('*')) if p.is_file()}
        (r/'run.json').write_text(json.dumps(record,indent=2)+'\n')
        start=time.monotonic()
        with (r/'host.log').open('w') as log:
            p=subprocess.Popen(command,cwd=r,env=e,stdout=log,stderr=subprocess.STDOUT)
            (r/'pid').write_text(str(p.pid))
            try:p.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        record.update(exit=p.returncode,elapsed=time.monotonic()-start)
        (r/'run.json').write_text(json.dumps(record,indent=2)+'\n')
        print(f'Exit {p.returncode}; diagnostic records: {r}')
        return 0 if p.returncode==0 else 1


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,subprocess.CalledProcessError) as error:sys.exit(f'error: {error}')

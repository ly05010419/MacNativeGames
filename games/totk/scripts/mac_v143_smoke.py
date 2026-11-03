#!/usr/bin/env python3
"""Load candidate Mac scenes from private save copies, one game at a time.

Captures require human review: a candidate coordinate can be inside geometry,
airborne or affected by quest state. A completed process is not a full-game
acceptance result. Real user input stops scripted control and leaves it open.
"""
import argparse
import csv
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/dev'))
from game_slot import GameSlot


class TakenOver(Exception):
    pass


class PreemptedTrial(Exception):
    pass


def last_frame(path):
    if not path.exists():
        return 0
    with path.open('rb') as stream:
        stream.seek(max(0, path.stat().st_size - 100000))
        lines = stream.read().splitlines()
    return max((int(line.split(b'\t', 1)[0]) for line in lines if line.split(b'\t', 1)[0].isdigit()), default=0)


def summarize(path, first, last):
    frames = {}
    with path.open(errors='replace') as stream:
        for line in stream:
            parts = line.rstrip().split('\t')
            if len(parts) != 3 or not parts[0].isdigit() or not first <= int(parts[0]) <= last:
                continue
            try:
                value = float(parts[2])
            except ValueError:
                continue
            if math.isfinite(value):
                frames.setdefault(int(parts[0]), {})[parts[1]] = value
    def values(name):
        return [row[name] for row in frames.values() if row.get(name, -1) >= 0]
    def percentile(data, q):
        return sorted(data)[min(len(data)-1, int(q*(len(data)-1)))] if data else None
    elapsed = [x for x in values('@frame_ms') if x > 0]
    return dict(first_frame=first, last_frame=last, measured_frames=len(elapsed),
                fps_mean=1000/statistics.mean(elapsed) if elapsed else None,
                frame_ms_p50=percentile(elapsed, .5), frame_ms_p95=percentile(elapsed, .95),
                frame_ms_p99=percentile(elapsed, .99), gpu_ms_p50=percentile(values('@gpu_ms'), .5),
                scope='default mode, fixed camera after warm-up; scene-specific, not a version comparison')


def run_point(args, point):
    out = args.output / point['id']
    out.mkdir(parents=True)
    subprocess.run([sys.executable, str(ROOT/'scripts/stage_test_profile.py'), str(args.profile),
                    str(out/'profile'), '--xyz', point['x'], point['y'], point['z']], check=True)
    commands, log_path, tsv = out/'input.txt', out/'game.log', out/'frames.tsv'
    commands.write_text('')
    env = os.environ | {
        'TOTK_PROFILE_DIR': str(out/'profile'), 'TOTK_INPUT_FILE': str(commands),
        'TOTK_SHADER_CACHE': str(ROOT/'build/shader-cache/rt-totk-native-v8-unverified'),
        'TOTK_ROMFS_PROOF_DIR': str(args.output/'romfs-proof'),
        'TOTK_ASSUME_FOCUS': '1', 'TOTK_AUTOLOAD': '1', 'TOTK_FPS60': '1', 'TOTK_REALTIME': '1',
        'TOTK_MFX': '0', 'TOTK_MFX_UPSCALE': '0', 'TOTK_MFX_OUTPUT': '0', 'TOTK_OUTPUT_RES': 'auto',
        'TOTK_WINDOW_SIZE': '1280x720', 'TOTK_DRAWABLE_SCALE': '1',
        'TOTK_GPU_ALLOW_SUBMIT': '1', 'TOTK_GPU_WIDEN': '1', 'TOTK_GPU_BLOCKRT': '1',
        'TOTK_GPU_MIDCLEAR': '1',
        'DM_NVN_TSV': str(tsv), 'DM_NVN_TSV_CALLS': '0',
    }
    env.pop('TOTK_INPUT_SCRIPT', None)
    env.pop('TOTK_SURVEY', None)
    result = dict(id=point['id'], category=point['category'], xyz=[float(point[k]) for k in ('x','y','z')],
                  status='running', started=time.time(), screenshots=[],
                  runtime_sha256=hashlib.sha256((args.runtime/'build/totk-native').read_bytes()).hexdigest())
    report_path = out/'report.json'
    def save():
        report_path.write_text(json.dumps(result, indent=2)+'\n')
    save()
    taken = False
    with GameSlot(owner='codex', purpose='v143 scene '+point['id'], expect_seconds=int(args.warmup+args.observe+180)) as slot:
        with log_path.open('wb') as log:
            process = slot.popen([str(args.runtime/'build/totk-native'), 'rewritten/v143', 'boot'],
                                 cwd=args.runtime, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        (out/'pid').write_text(str(process.pid))
        result['pid'] = process.pid
        reader = log_path.open('rb')
        tail = b''
        loaded_at = None
        def check():
            nonlocal tail, loaded_at
            recent = reader.read()
            scan = tail+recent
            tail = scan[-128:]
            if b'[input] user input' in scan:
                raise TakenOver('real user input; scripted control stopped, game left open')
            if b'[autoload] load started' in scan and loaded_at is None:
                loaded_at = time.monotonic()
            if slot.preempted:
                raise PreemptedTrial('the user launched their own game')
            if process.poll() is not None:
                raise RuntimeError('game exited with '+str(process.returncode))
        def wait(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                check()
                time.sleep(min(.2, max(0, end-time.monotonic())))
        def send(command):
            check()
            with commands.open('a') as stream:
                stream.write(command+'\n')
        def capture(name):
            path = out/(name+'.png')
            send('wshot '+str(path))
            end = time.monotonic()+15
            while not path.exists() and time.monotonic()<end:
                wait(.1)
            if not path.exists():
                raise RuntimeError('screenshot missing: '+name)
            wait(.3)
            result['screenshots'].append(path.name)
            save()
        try:
            deadline=time.monotonic()+180
            while loaded_at is None or time.monotonic()-loaded_at<args.warmup:
                if time.monotonic()>deadline:
                    raise RuntimeError('scene load did not settle')
                wait(.2)
            # Fold a load-time paraglider if present; at a valid ground candidate
            # this is a tiny landing. Large falls/invalid coordinates need review.
            send('key 40 150')
            wait(3)
            capture('default-start')
            observed_from=time.monotonic()
            first=last_frame(tsv)+1
            print(json.dumps(dict(id=point['id'], phase='observing', pid=process.pid)), flush=True)
            wait(args.observe)
            last=last_frame(tsv)
            result['baseline_frames']=[first,last]
            result['baseline_wall_seconds']=time.monotonic()-observed_from
            result['baseline_game_frames_per_second']=(last-first+1)/result['baseline_wall_seconds']
            capture('default-end')
            for index in range(3):
                send('key 124 750')
                wait(2)
                capture('camera-'+str(index+1))
            if not args.no_picture_modes:
                for name in ('metalfx','native-2k','off-restored'):
                    send('key 20 150')
                    wait(3)
                    capture(name)
            result['status']='captured-needs-visual-review'
        except KeyboardInterrupt:
            result.update(status='interrupted', error='driver interrupted')
        except TakenOver as error:
            taken=True
            result.update(status='user-takeover', error=str(error))
        except PreemptedTrial as error:
            result.update(status='preempted', error=str(error))
        except Exception as error:
            result.update(status='failed', error=str(error))
        finally:
            if process.poll() is None and not taken and not slot.preempted:
                try:
                    check()
                    with commands.open('a') as stream:
                        stream.write('close\n')
                    process.wait(timeout=30)
                except TakenOver:
                    taken=True
                    result['status']='user-takeover'
                except PreemptedTrial:
                    result['status']='preempted'
                except (subprocess.TimeoutExpired, RuntimeError):
                    if process.poll() is None and not taken:
                        # Only this owned child can be stopped here. A real
                        # input seen just before termination still takes over.
                        try:
                            check()
                        except TakenOver:
                            taken=True
                            result['status']='user-takeover'
                        except PreemptedTrial:
                            result['status']='preempted'
                        except RuntimeError:
                            pass
                        if not taken and not slot.preempted and process.poll() is None:
                            process.terminate()
                            process.wait(timeout=15)
                            result['forced_stop']=True
            reader.close()
            result['exit_code']=process.poll()
            if slot.preempted:
                result['status']='preempted'
            elif result['exit_code'] not in (None,0) and not result.get('forced_stop'):
                result.update(status='failed',error='unexpected process exit '+str(result['exit_code']))
            result['finished']=time.time()
            save()
    if not taken and process.poll() is not None and tsv.exists():
        if 'baseline_frames' in result:
            result['baseline']=summarize(tsv,*result['baseline_frames'])
        compressed=tsv.with_suffix('.tsv.gz')
        with tsv.open('rb') as raw,gzip.open(compressed,'wb') as packed:
            shutil.copyfileobj(raw,packed)
        tsv.unlink()
        result['frame_data']=compressed.name
        save()
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--slot',type=Path,required=True)
    parser.add_argument('--points',type=Path,default=ROOT/'assets/mac-v143-smoke-points.tsv')
    parser.add_argument('--runtime',type=Path,default=ROOT.parents[1]/'out/Tears of the Kingdom.app/Contents/Resources/totk')
    parser.add_argument('--warmup',type=float,default=25)
    parser.add_argument('--observe',type=float,default=30)
    parser.add_argument('--only',nargs='*')
    parser.add_argument('--no-picture-modes',action='store_true')
    args=parser.parse_args()
    args.profile=args.profile.resolve();args.output=args.output.resolve();args.runtime=args.runtime.resolve()
    if args.warmup<10 or args.observe<10 or not (args.runtime/'build/totk-native').is_file():
        parser.error('requires a built runtime, warmup >=10 and observe >=10')
    os.environ['TOTK_GAME_SLOT_DIR']=str(args.slot.resolve())
    with args.points.open() as stream:
        points=list(csv.DictReader((line for line in stream if line.strip() and not line.startswith('#')),delimiter='\t'))
    for point in points:
        if not re.fullmatch('[A-Za-z0-9_-]+',point['id']):
            parser.error('invalid scene id')
    if args.only:
        unknown=set(args.only)-{p['id'] for p in points}
        if unknown:
            parser.error('unknown scene ids: '+', '.join(sorted(unknown)))
        points=[p for p in points if p['id'] in args.only]
    args.output.mkdir(parents=True,exist_ok=True)
    for point in points:
        existing=args.output/point['id']/'report.json'
        if existing.exists():
            print(json.dumps(dict(id=point['id'],phase='kept-existing-result')),flush=True)
            continue
        result=run_point(args,point)
        print(json.dumps(result),flush=True)
        if result['status'] in ('user-takeover','preempted','failed','interrupted'):
            return 1
    return 0


if __name__=='__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Test the packaged app with empty translation caches, then a repeated launch.

Uses private empty profiles and the standard shared game slot. Stops all control
on real input/preemption. Screenshots still need human review. The system Metal
driver cache is not cleared: this checks discovery/warm-up, not cold GPU timing.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent/'dev'))
from game_slot import GameSlot

DATA = Path.home()/'Library/Application Support/MacNativeGames/TOTK'
LOG = Path.home()/'Library/Logs/MacNativeGames/totk.log'


def startup_facts(text, mode, expected):
    discovery = re.findall(r'\[shader-seed\].*?着色器 (\d+) 个，导入 (\d+) 个文件', text)
    priority = re.findall(r'启动关键管线已就绪：等待 (\d+) ms，可用着色器 (\d+) / 渲染管线 (\d+) / 计算管线 (\d+)', text)
    first, ready, user = (text.find(s) for s in ('first presented frame', '启动关键管线已就绪：', '[input] user input'))
    return dict(discovery=[list(map(int, x)) for x in discovery],
        priority=[list(map(int, x)) for x in priority],
        seed_ready_before_first_present=0 <= ready < first,
        first_present_before_user_input=first >= 0 and (user < 0 or first < user),
        first_launch_contract_passed=bool(mode != 'off' and discovery and priority and
            int(discovery[0][0]) == expected and int(priority[0][1]) >= expected and
            0 <= ready < first and (user < 0 or first < user)))


def trial(app, output, mode, cache, profile, expected, timeout=1800):
    output.mkdir()
    profile.mkdir(exist_ok=True)
    commands = output/'input.txt'
    commands.write_text('')
    before = LOG.stat().st_ino if LOG.exists() else None
    report = dict(status='running', seed_mode=mode, started=time.time(), cache=str(cache),
                  cache_files_before=len(list(cache.iterdir())) if cache.exists() else 0,
                  profile=str(profile), screenshot='title.png')
    path = output/'report.json'
    def save():
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    save()
    env = os.environ | dict(TOTK_DEBUG_LOG='1', TOTK_PROFILE_DIR=str(profile), TOTK_INPUT_FILE=str(commands),
        TOTK_SHADER_CACHE=str(cache), TOTK_SHADER_SEED=mode, TOTK_ASSUME_FOCUS='1',
        TOTK_WINDOW_SIZE='1280x720', TOTK_MFX_INTERP='0', TOTK_MFX='0',
        TOTK_MFX_UPSCALE='0', TOTK_MFX_OUTPUT='0')
    for name in ('DM_PSO_MANIFEST', 'TOTK_AUTOLOAD', 'DM_NVN_TSV', 'TOTK_INPUT_SCRIPT', 'TOTK_SURVEY'):
        env.pop(name, None)
    taken, text = False, ''
    with GameSlot(owner='codex', purpose='first-launch shader '+output.name, expect_seconds=int(timeout+60)) as slot:
        with (output/'launcher.log').open('wb') as stream:
            game = slot.popen([str(app/'Contents/MacOS/TOTK')], env=env, stdout=stream, stderr=subprocess.STDOUT)
        report['pid'] = game.pid
        report['token'] = slot.token
        save()
        def check():
            nonlocal text, taken
            if slot.preempted:
                taken = True
                raise RuntimeError('user launch preempted this test; control stopped')
            if LOG.exists() and LOG.stat().st_ino != before:
                text = LOG.read_text(errors='replace')
                if '[input] user input' in text:
                    taken = True
                    raise RuntimeError('real user input; control stopped and game left open')
            if game.poll() is not None:
                raise RuntimeError('game exited before test completed: '+str(game.returncode))
        try:
            deadline, ready = time.monotonic()+timeout, None
            while time.monotonic() < deadline:
                check()
                if 'first presented frame' in text and '管线预热完成：' in text:
                    ready = ready or time.monotonic()
                    if time.monotonic()-ready >= 15:
                        break
                time.sleep(.25)
            else:
                raise RuntimeError('timed out waiting for warm-up and first frame')
            check()
            with commands.open('a') as f:
                f.write('wshot '+str(output/'title.png')+'\n')
            deadline = time.monotonic()+15
            while not (output/'title.png').exists() and time.monotonic() < deadline:
                check(); time.sleep(.25)
            check()
            if not (output/'title.png').exists():
                raise RuntimeError('title screenshot did not complete')
            discovered = re.findall(r'\[shader-seed\].*?着色器 (\d+) 个，导入 (\d+) 个文件', text)
            warm = re.findall(r'管线预热完成：着色器 (\d+) 个 / 渲染管线 (\d+) 个.*?/ 计算管线 (\d+) 个', text)
            priority = re.findall(r'启动关键管线已就绪：等待 (\d+) ms，可用着色器 (\d+) / 渲染管线 (\d+) / 计算管线 (\d+)', text)
            report.update(discovery=[list(map(int, x)) for x in discovered],
                          warm=[list(map(int, x)) for x in warm], priority=[list(map(int, x)) for x in priority],
                          warm_before_first_present=text.index('管线预热完成：') < text.index('first presented frame'),
                          cache_files_after=len(list(cache.iterdir())))
            if mode == 'off':
                assert not discovered, 'negative control imported the seed'
            else:
                assert discovered and int(discovered[0][0]) == expected, 'seed discovery missing'
                assert warm and int(warm[-1][0]) >= expected, 'prepared shaders were not warmed'
                assert priority and int(priority[0][1]) >= expected, 'startup released before seed readiness'
                assert startup_facts(text, mode, expected)['seed_ready_before_first_present']
            report['status'] = 'pass-awaiting-visual-review'
        except Exception as error:
            report.update(status='interrupted' if taken else 'failed', error=str(error))
        finally:
            if not taken and not slot.preempted and game.poll() is None:
                try:
                    check()
                    with commands.open('a') as f:
                        f.write('close\n')
                    game.wait(timeout=20)
                except Exception as error:
                    report.update(status='interrupted' if taken else 'failed', close_error=str(error))
            report.update(finished=time.time(), exit=game.poll(), game_left_open=game.poll() is None)
            # Preserve completed startup facts even if the user takes over the
            # later screenshot/idle stage. This never resumes scripted control.
            report['startup_phase'] = startup_facts(text, mode, expected)
            (output/'game.log').write_text(text)
            save()
    if report['game_left_open'] or report['status'] != 'pass-awaiting-visual-review':
        raise RuntimeError(str(path)+': '+report.get('error', report['status']))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=1800, help='per-launch shader preparation timeout in seconds')
    a = p.parse_args()
    app, output = a.app.resolve(), a.output.resolve()
    if output.exists():
        raise ValueError('use a new output directory; existing evidence is preserved')
    output.mkdir(parents=True)
    seed = json.loads((app/'Contents/Resources/totk/build/shader-seed/seed-manifest.json').read_text())
    os.environ['TOTK_GAME_SLOT_DIR'] = str(DATA/'game-slot')
    reports = []
    for name, mode, cache, profile in [
            ('first-empty', 'on', output/'first-cache', output/'first-profile'),
            ('second-reuse', 'on', output/'first-cache', output/'first-profile'),
            ('negative-empty', 'off', output/'negative-cache', output/'negative-profile')]:
        print(name, flush=True)
        r = trial(app, output/name, mode, cache, profile, seed['shaders'], a.timeout)
        reports.append(r)
        print(json.dumps({k:r[k] for k in ('status', 'discovery', 'warm', 'priority')}), flush=True)
    assert reports[0]['cache_files_before'] == reports[2]['cache_files_before'] == 0
    assert reports[0]['discovery'][0][1] > 0 and reports[1]['discovery'][0][1] == 0
    binary = app/'Contents/Resources/totk/build/totk-native'
    (output/'summary.json').write_text(json.dumps(dict(status='pass-awaiting-visual-review',
        runtime_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), prepared_shaders=seed['shaders'],
        reports=reports, scope=__doc__), ensure_ascii=False, indent=2)+'\n')


if __name__ == '__main__':
    main()

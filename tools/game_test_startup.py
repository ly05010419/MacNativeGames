#!/usr/bin/env python3
"""Fail closed before a quick/test game launch; no guest code runs in preflight.

Use under this machine's storage_guard. The source tree's official quick build
prepares the seed and libraries; this entry validates those exact files before
starting an already-built native runtime through the shared game slot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def read_record(path):
    value = json.loads(Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError(f'not a manifest: {path}')
    return value


def checked_files(folder, files):
    if not isinstance(files, dict) or not files:
        raise ValueError(f'empty file manifest: {folder}')
    for name, want in files.items():
        if Path(name).name != name or not re.fullmatch(r'[0-9a-f]{64}', str(want)):
            raise ValueError(f'invalid manifest file: {name}')
        path = folder / name
        if not path.is_file() or digest(path) != want:
            raise ValueError(f'missing or damaged prepared file: {path}')


def preflight(binary, native, seed, libraries, expected_chain):
    binary, native, seed, libraries = map(Path, (binary, native, seed, libraries))
    chains = set(x.decode('ascii') for x in re.findall(rb'MNG_SHADER_CHAIN=([A-Za-z0-9_.-]+)\x00', binary.read_bytes()))
    if chains != {expected_chain}:
        raise ValueError(f'compiled runtime chain {sorted(chains)} does not match {expected_chain}; rebuild the test runtime')
    metadata = read_record(seed / 'seed-manifest.json')
    if metadata.get('chain') != expected_chain:
        raise ValueError(f'seed chain {metadata.get("chain")} does not match runtime {expected_chain}; prepare the quick seed')
    if (not metadata.get('all_default') or not isinstance(metadata.get('shaders'), int) or
            metadata['shaders'] <= 0 or metadata.get('translated_default') != metadata.get('corpus_shaders')):
        raise ValueError('startup seed is incomplete')
    translator = native / 'shader-service/totk_xlate'
    if digest(translator) != metadata.get('translator_sha256'):
        raise ValueError('runtime translator differs from the startup seed')
    checked_files(seed, metadata.get('files'))
    libs = read_record(libraries / 'libraries-manifest.json')
    if (native / 'shader-libs').resolve() != libraries.resolve():
        raise ValueError('validated libraries are not the libraries the runtime will open')
    if not libs.get('complete'):
        raise ValueError('startup Metal libraries are incomplete')
    checked_files(libraries, libs.get('files'))
    for name, want in metadata['files'].items():
        if name.endswith('_b.metal') and (libs.get('sources', {}).get(name) != want or
                                         want + '.metallib' not in libs['files']):
            raise ValueError(f'prepared library does not cover startup shader: {name}')
    title = native / 'pso_title.tsv'
    if not title.is_file() or not any(line and not line.startswith('#') for line in title.read_text().splitlines()):
        raise ValueError('quick startup has no resolved title pipeline list')
    return dict(chain=expected_chain, runtime_sha256=digest(binary), prepared_shaders=metadata['shaders'],
                translator_sha256=digest(translator), seed_manifest_sha256=digest(seed / 'seed-manifest.json'),
                libraries_manifest_sha256=digest(libraries / 'libraries-manifest.json'))


def launch(binary, native, seed, libraries, expected_chain, command, *, mode='quick', env=None, popen=None):
    """A game test starts as a quick start by default (user 2026-11-15: "我们测试的使用 默认用 快速启动"). Shaders it was
    not given are made as the game's own start makes them (on the submit thread): background translation was tried the
    same day and its translator service crashed (user: "不用 后台编译了")."""
    if mode not in ('quick', 'test'):
        raise ValueError('game tests require quick or test startup')
    values = dict(os.environ if env is None else env)
    if values.get('TOTK_SHADER_SEED') == 'off':
        raise ValueError('test startup cannot disable its prepared seed')
    if values.get('TOTK_SHADER_XLATE') or values.get('TOTK_SHADER_SERVICE', 'native') != 'native':
        raise ValueError('test startup cannot override its validated translator')
    if values.get('TOTK_GPU_ALLOW_SUBMIT') != '1' or values.get('TOTK_GPU_NOCOMMIT') == '1':
        raise ValueError('real-game test requires explicit GPU submission; encoding-only runs cannot validate a picture')
    facts = preflight(binary, native, seed, libraries, expected_chain)
    values.update(TOTK_NATIVE_DIR=str(Path(native).resolve()), TOTK_SHADER_SEED_DIR=str(Path(seed).resolve()),
                  TOTK_SHADER_SEED='on', TOTK_WARM_BEFORE_TITLE='1', MNG_TEST_STARTUP=mode)
    # The caller supplies GameSlot.popen; tests supply a spy proving rejection
    # happens before any process starts. No shell or implicit fallback launch.
    if popen is None:
        raise ValueError('a shared GameSlot launcher is required')
    return popen([str(binary), *map(str, command)], env=values), facts


def startup_state(text, expected_shaders):
    if 'GPU submission disabled' in text:
        raise ValueError('runtime disabled GPU submission; no game-picture validation is possible')
    if 'Startup shader seed identity is invalid' in text or '改为边玩边翻译' in text:
        raise ValueError('runtime rejected its prepared startup seed; stop and diagnose')
    if '【红】管线存档（' in text:   # nvn_mtl.m par_miss_summary: the App's own listed pipelines missed its archive
        raise ValueError('the App\'s pipeline archive missed its own listed pipelines (they are compiled again); stop and diagnose')
    ready = re.findall(r'启动关键管线已就绪：等待 \d+ ms，可用着色器 (\d+) / 渲染管线 (\d+) / 计算管线 (\d+)', text)
    if ready:
        shaders, render, compute = map(int, ready[-1])
        if shaders < expected_shaders or render + compute == 0:
            raise ValueError('startup warmed no usable prepared pipelines; stop and diagnose')
    return 'ready' if ready and 'first presented frame' in text else 'preparing'


class StartupWatch:
    """Audio/rumble/CPU-load chatter cannot turn a stalled startup into progress."""
    def __init__(self, now, quiet_seconds=30):
        self.last_progress = now
        self.quiet_seconds = quiet_seconds
        self.signature = ()

    def observe(self, text, now, expected_shaders):
        state = startup_state(text, expected_shaders)
        events = tuple(line for line in text.splitlines() if any(word in line for word in
                       ('[totk-boot]', '[shader-seed]', '[test-startup]', '预热', 'compiled task', '着色器库：')))
        if events != self.signature:
            self.signature = events
            self.last_progress = now
        if state != 'ready' and now - self.last_progress >= self.quiet_seconds:
            return 'diagnose'
        return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for option in ('binary', 'native', 'seed', 'libraries'):
        p.add_argument('--' + option, type=Path, required=True)
    p.add_argument('--chain', required=True)
    a = p.parse_args()
    # Read-only command useful before acquiring the game slot or starting any
    # controller. A failed check never falls through to runtime translation.
    print(json.dumps(preflight(a.binary, a.native, a.seed, a.libraries, a.chain), indent=2))


if __name__ == '__main__':
    main()

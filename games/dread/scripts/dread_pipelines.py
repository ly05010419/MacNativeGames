"""Build the App's exact warm-up list outside the App; retain usable archive entries."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def run_outside_bundle(argv, **kwargs):
    if any(part.endswith(".app") for part in Path(argv[0]).parts):
        raise ValueError("pipeline builder must run outside an App")
    # The same shared slot used by other games' packaging and GPU probes.
    scripts = Path(__file__).resolve().parents[2]/'totk/scripts'
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    from dev.game_slot import GameSlot
    timeout = kwargs.pop('timeout')
    with GameSlot(owner='build', purpose='Dread offline pipelines', expect_seconds=timeout) as slot:
        process = slot.popen(argv, **kwargs)
        try:
            code = process.wait(timeout=timeout)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
        return subprocess.CompletedProcess(argv, code)


def list_digest(profile):
    digest = hashlib.sha256()
    for name in ('pso_title.tsv', 'pso_manifest.tsv'):
        path = profile/name
        digest.update(name.encode()+b'\0'+(path.read_bytes() if path.is_file() else b'absent')+b'\0')
    return digest.hexdigest()


def check_archive_lists(archive, profile):
    report = json.loads((archive/'manifest.json').read_text())
    if report.get('list_sha256') != list_digest(profile):
        raise ValueError('pipeline archive was built from another warm-up list')


def mac_pipelines(host, profile, libraries, build, env):
    # This cache is deliberately independent of the runtime's identity. The host asks
    # each existing archive for the actual descriptor and compiles only a miss.
    archive = build/'pipeline-archive'
    archive.mkdir(parents=True, exist_ok=True)
    log = build/'logs/pipeline-archive.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    wanted = list_digest(profile)
    rows = lambda p: {line for line in p.read_text().splitlines() if line and not line.startswith('#')}
    if (profile/'pso_title.tsv').is_file() and not rows(profile/'pso_title.tsv') <= rows(profile/'pso_manifest.tsv'):
        raise ValueError('title warm-up rows must be included in the archive manifest')
    settings = dict(env, DM_PSO_OFFLINE='1', DM_PSO_ARCHIVE_BUILD='1', DM_PSO_PREPARE='1',
                    DM_PSO_PREPARE_THREADS=str(min(8, __import__('os').cpu_count() or 2)),
                    DM_PSO_MANIFEST=str(profile/'pso_manifest.tsv'), DM_NVN_SHMAP=str(profile/'shader_map.tsv'),
                    DM_PSO_ARCHIVE=str(archive), DM_PSO_PREBUILD_DEPTH='zero',
                    DM_PSO_USER='off', DM_WINDOW='0', DM_AUDIO='0', DM_METAL_RUNTIME='0')
    print('pipelines: checking the current descriptors against the retained archive', flush=True)
    # host is build/tools/dmhost, never an executable inside the assembling App.
    with log.open('w') as stream:
        done = run_outside_bundle([str(host)], env=settings, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    text = log.read_text(errors='replace')
    result = re.search(r'\[MNG\] manifest archive: (\d+) requested, (\d+) built, (\d+) shaders, success', text)
    if done.returncode or not result or result[1] != result[2] or (int(result[2]) and not list(archive.glob('*.mngarc'))):
        raise ValueError(f'pipeline archive did not complete; see {log}')
    # A fresh process proves the serialized files contain the requested bases; the
    # compiler process's in-memory cache cannot stand in for a successfully saved file.
    verify_log = log.with_name('pipeline-archive-verify.log')
    with verify_log.open('w') as stream:
        checked = run_outside_bundle([str(host)], env=dict(settings, DM_PSO_REQUIRE_ARCHIVE='1'),
                           stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    if checked.returncode or result.group(0) not in verify_log.read_text(errors='replace'):
        raise ValueError(f'pipeline archive failed fresh-process verification; see {verify_log}')
    if list_digest(profile) != wanted:
        raise ValueError('warm-up list changed while the archive was being built')
    (archive/'manifest.json').write_text(json.dumps({'list_sha256': wanted, 'pipelines': int(result[2]), 'shaders': int(result[3])}, sort_keys=True)+'\n')
    check_archive_lists(archive, profile)
    return archive

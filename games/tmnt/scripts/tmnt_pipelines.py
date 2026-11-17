"""Build and check Metal archives from the exact list shipped beside the shader map."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from rewrite_modules import GAME
from build_app import run_outside_bundle


def list_digest(build):
    return hashlib.sha256((build / 'pso_manifest.tsv').read_bytes()).hexdigest()


def check_archive_lists(archive, build):
    report = json.loads((archive / 'lists.json').read_text())
    if report['pso_manifest.tsv'] != list_digest(build):
        raise ValueError('pipeline archive was built from a different warm-up list')
    if report['requested'] != report['built'] or not report['built']:
        raise ValueError('pipeline archive incomplete')
    expected = set(report['files'])
    actual = {p.name for p in archive.iterdir() if p.suffix in ('.metalarchive', '.json') and p.name != 'lists.json'}
    if actual != expected:
        raise ValueError('pipeline archive contains missing or stale files')
    for name, digest in report['files'].items():
        if Path(name).name != name or hashlib.sha256((archive/name).read_bytes()).hexdigest() != digest:
            raise ValueError('pipeline archive changed')
    return report


def mac_pipelines(native, rewritten, build):
    # build/pso_manifest.tsv is the sole pipeline-list source for both packaging and startup.
    # Resolve collected bind states to variants this seed actually serves.
    subprocess.run([sys.executable, str(GAME.parents[1] / 'games/totk/scripts/shader_list.py'), 'resolve',
                    str(build/'shader-seed'), str(GAME/'assets/shader_list.tsv'),
                    str(build/'pso_manifest.tsv'), str(build/'pso_title.tsv')], check=True)
    with (build/'pso_manifest.tsv').open('a') as target:
        target.write((build/'pso_title.tsv').read_text())
    (build / 'shader-map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    rows = sorted({line for line in (build/'pso_manifest.tsv').read_text().splitlines() if line.startswith(('R\t','C\t','B\t'))})
    if not rows: raise ValueError('TMNT has no warm-up pipeline list')
    want = list_digest(build)
    root = build / 'mac-pipelines'; root.mkdir(exist_ok=True)
    def run_partition(index):
        folder = root / str(index); folder.mkdir(exist_ok=True)
        selected = rows[index::min(4,len(rows))]
        manifest = folder / 'pso_manifest.tsv'; manifest.write_text('\n'.join(selected)+'\n')
        cache = folder / 'cache'
        if (cache / 'pipelines').exists():
            shutil.rmtree(cache / 'pipelines')
        env = dict(os.environ, TOTK_AUDIO_OFF='1', TOTK_CLOUD_SAVE_DIR='0', TOTK_PROFILE_DIR=str(folder/'profile'),
                   TOTK_SHADER_CACHE=str(cache), TOTK_SHADER_SEED='on', TOTK_SHADER_SEED_DIR=str(build/'shader-seed'),
                   DM_NVN_SHMAP=str(build/'shader-map.tsv'), DM_PSO_MANIFEST=str(manifest), TOTK_WARM_BEFORE_TITLE='1',
                   TOTK_PIPELINE_ARCHIVE='1', TOTK_PIPELINE_ARCHIVE_FILL='0')
        # All executions happen in the work folder, never inside the new .app.
        observed = 0
        for check in range(2):
            with run_outside_bundle(native, folder / 'archive-tool', folder / 'outside') as program:
                result = subprocess.run([str(program),str(rewritten),'pipeline-archive'], env=env, cwd=build,
                                        capture_output=True,text=True,timeout=180)
            (folder/f'run-{check}.log').write_text(result.stdout+result.stderr)
            result.check_returncode()
            reports=[json.loads(l) for l in result.stdout.splitlines() if l.startswith('{"render":')]
            if not reports or reports[-1]['render']+reports[-1]['compute'] != len(selected) or reports[-1]['write_failed']:
                raise ValueError(f'pipeline partition incomplete; see {folder}')
            observed = reports[-1]['render'] + reports[-1]['compute']
        return cache/'pipelines', observed
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        partitions=list(pool.map(run_partition,range(min(4,len(rows)))))
    archive = build / 'pipeline-archive'
    if archive.exists():
        shutil.rmtree(archive)
    archive.mkdir()
    files={}
    built = 0
    for part, observed in partitions:
        built += observed
        for source in part.glob('*'):
            if source.suffix not in ('.metalarchive','.json'):continue
            sha=hashlib.sha256(source.read_bytes()).hexdigest()
            if source.name in files and files[source.name]!=sha:raise ValueError('archive partitions have different identities')
            shutil.copy2(source,archive/source.name);files[source.name]=sha
    if not any(n.endswith('.metalarchive') for n in files):raise ValueError('no pipeline archives written')
    if list_digest(build)!=want:raise ValueError('pipeline list changed during build')
    (archive/'lists.json').write_text(json.dumps({'pso_manifest.tsv':want,'requested':len(rows),'built':built,'files':files},sort_keys=True)+'\n')
    check_archive_lists(archive,build)
    return archive

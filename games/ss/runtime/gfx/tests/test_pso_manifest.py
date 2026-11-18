"""Real Metal archive round trips in fresh processes, using synthetic shaders."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check(binary):
    with tempfile.TemporaryDirectory(prefix='ss-manifest-') as tmp:
        root = Path(tmp)
        env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
        def run(*args, ok=True, **settings):
            result = subprocess.run([str(binary), *args], env=dict(env, **settings), capture_output=True, text=True, timeout=90)
            if (result.returncode == 0) != ok:
                raise AssertionError(result.stdout+result.stderr)
            return result.stdout+result.stderr
        run('--fixture', str(root))
        archive = root/'archive'; archive.mkdir()
        settings = dict(DM_MSL_DIR=str(root/'cache'), DM_NVN_SHMAP=str(root/'shader_map.tsv'),
                        DM_PSO_MANIFEST=str(root/'pso_manifest.tsv'), DM_PSO_ARCHIVE=str(archive),
                        DM_PSO_PREBUILD='manifest', DM_PSO_PREBUILD_DEPTH='zero', DM_PSO_PREPARE_THREADS='2')
        first = run(**settings)
        assert '1 requested, 1 built, success' in first and '1 compiled, 0 archived' in first, first
        files = lambda: {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in archive.glob('*.mtl4a')}
        before = files(); assert before
        again = run(**dict(settings, DM_TEST_SEED='1'))
        assert '0 compiled, 1 archived' in again, again
        assert files() == before, 'unchanged descriptors rewrote the archive'
        writable = root/'user-archive'; writable.mkdir()
        seeded = run(**dict(settings, DM_TEST_SEED='1', DM_PSO_SEED=str(archive), DM_PSO_ARCHIVE=str(writable)))
        assert '0 compiled, 1 archived' in seeded, seeded
        assert files() == before and not list(writable.glob('*.mtl4a')), 'read-only seed was recaptured'
        with (root/'pso_manifest.tsv').open('a') as f:
            f.write('R\tinvalid\n')
        bad = run(ok=False, **settings)
        assert 'packaging manifest incomplete' in bad, bad
        print('SS manifest archive: 4 fresh-process checks passed (build, reuse, read-only seed, bad row)')


if __name__ == '__main__':
    check(Path(sys.argv[1]).resolve())

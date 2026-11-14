"""Full native host's offline entry, using original synthetic Metal (no game data)."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME/'scripts'))
from worldend_pipelines import mac_pipelines, check_archive_lists


@unittest.skipUnless(os.environ.get('WORLDEND_TEST_HOST'), 'set WORLDEND_TEST_HOST to an externally built host')
class OfflineHost(unittest.TestCase):
    def test_compile_reuse_and_manifest_change(self):
        host = Path(os.environ['WORLDEND_TEST_HOST']).resolve()
        chain = re.search(r'#define DMSH_CHAIN "([^"]+)"', (GAME/'runtime/gfx/nvn/dmsh_chain.h').read_text())[1]
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); profile = work/'profile'; profile.mkdir()
            library = work/'cache'/f'b-{chain}'; library.mkdir(parents=True)
            rows = []
            mapping = ['package\tindex\tstage\tdm_ctl_sha16']
            for i in range(3):
                key = f'{i+1:016x}'
                source = work/f'{key}.metal'
                source.write_text('#include <metal_stdlib>\nusing namespace metal;\nkernel void main0() {}\n')
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal4.0', '-o', str(library/f'{key}_b.metallib'), str(source)], check=True)
                (library/f'{key}_b.json').write_text(json.dumps(dict(chain=chain, route='B', entry='main0', bindings=[], threadgroup_size=[1,1,1])))
                mapping.append(f'fixture\t{i}\tcompute\t{key}')
                rows.extend([f'S\t{key}_b\t5', f'C\t{key}_b'])
            (library/'inputs.tsv').write_text('game\t010048900CF64000\ninputs\tfixture\n')
            (profile/'shader_map.tsv').write_text('\n'.join(mapping)+'\n')
            (profile/'pso_manifest.tsv').write_text('\n'.join(rows[:4])+'\n')
            env = dict(os.environ, DM_ROOT=str(work))
            archive = mac_pipelines(host, profile, library, work, env)
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in archive.glob('*.mngarc')}
            self.assertTrue(before)
            mac_pipelines(host, profile, library, work, env)
            self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in archive.glob('*.mngarc')})
            (profile/'pso_manifest.tsv').write_text('\n'.join(rows)+'\n')
            with self.assertRaisesRegex(ValueError, 'another warm-up list'): check_archive_lists(archive, profile)
            mac_pipelines(host, profile, library, work, env)
            self.assertEqual(json.loads((archive/'manifest.json').read_text())['pipelines'], 3)
            self.assertEqual(before, {name: hashlib.sha256((archive/name).read_bytes()).hexdigest() for name in before})


if __name__ == '__main__': unittest.main()

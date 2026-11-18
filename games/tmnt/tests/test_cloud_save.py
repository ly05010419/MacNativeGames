"""Run the repository's save conflict/backup suite against the TMNT runtime copy."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('shared_cloud_tests', ROOT / 'tests/test_cloud_save.py')
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)

class TMNTCloudSave(shared.CloudSave):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temp.name) / 'probe'
        probe = Path(cls.temp.name) / 'probe.m'
        source = (ROOT / 'tests/cloud_save_probe.m').read_text().replace(
            '../games/totk/runtime/cloud_save.m', str(ROOT / 'games/tmnt/runtime/cloud_save.m'))
        probe.write_text(source)
        subprocess.run(['clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', str(probe), '-framework', 'Foundation',
                        '-o', str(cls.exe)], check=True, timeout=90)

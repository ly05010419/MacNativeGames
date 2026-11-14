"""The same per-work-folder Python environment used by MNG's other game builds."""
from pathlib import Path
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]


def build_python(work):
    if sys.version_info < (3, 14):
        raise ValueError('Worldend Syndrome requires Python 3.14 or later (MNG Launcher prerequisites)')
    folder = Path(work)/'venv'
    python = folder/'bin/python'
    if not python.is_file():
        subprocess.run([sys.executable, '-m', 'venv', str(folder)], check=True)
    expected = (GAME/'requirements.txt').read_bytes()
    marker = folder/'mng-worldend-requirements.txt'
    imports = subprocess.run([str(python), '-c', 'import lz4.block, capstone, fontTools'], capture_output=True)
    if imports.returncode or not marker.is_file() or marker.read_bytes() != expected:
        subprocess.run([str(python), '-m', 'pip', 'install', '-r', str(GAME/'requirements.txt')], check=True)
        marker.write_bytes(expected)
    return python

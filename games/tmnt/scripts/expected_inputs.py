"""Fail closed on unsupported original TMNT ExeFS input versions."""
import json
from pathlib import Path

EXPECTED = Path(__file__).resolve().parents[1] / 'assets/expected-base-1.0.0.json'


def check_supported_files(title_id, files):
    expected = json.loads(EXPECTED.read_text())
    if title_id != expected['title_id']:
        raise ValueError('unsupported TMNT title ID')
    actual = {entry['name']: entry['sha256'] for entry in files}
    if len(actual) != len(files) or actual != expected['files']:
        raise ValueError('unsupported TMNT ExeFS version or input hash mismatch')
    return expected['version']


def check_supported_romfs(sha256):
    expected = json.loads(EXPECTED.read_text())
    if sha256 != expected['romfs_sha256']:
        raise ValueError('unsupported TMNT RomFS version or input hash mismatch')
    return expected['version']

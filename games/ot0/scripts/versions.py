import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
FILES = {'v103': 'expected-v1.0.3.json', 'v104': 'expected-v1.0.4.json'}
VERSIONS = {version: json.loads((HERE / 'assets' / name).read_text()) for version, name in FILES.items()}


def expected(version):
    if version not in VERSIONS:
        raise ValueError(f'unsupported version {version}; supported: {", ".join(VERSIONS)}')
    return VERSIONS[version]


def supported():
    return ', '.join(pinned['display_version'] for pinned in VERSIONS.values())


def identify_exefs(exefs):
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in Path(exefs).iterdir() if path.is_file()}
    for version, pinned in VERSIONS.items():
        if hashes == pinned['exefs_sha256']:
            return version
    raise ValueError(f'unsupported update: its code matches none of {supported()} (the file name is not used to choose a version)')


def identify_update(program):
    for version, pinned in VERSIONS.items():
        nca = pinned['update_program_nca']
        if (program['name'], program['size']) == (nca['name'], nca['size']):
            return version
    raise ValueError(f'unsupported update: its Program NCA {program["name"]} belongs to none of {supported()}')

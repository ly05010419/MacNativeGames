import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
FILES = {'v100': 'expected-v1.0.0.json', 'v221': 'expected-v2.2.1.json'}
# A version is supported once its manifest (assets/expected-<version>.json) is written; until then none is.
VERSIONS = {version: json.loads((HERE / 'assets' / name).read_text()) for version, name in FILES.items() if (HERE / 'assets' / name).exists()}


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
    raise ValueError(f'unsupported game: its code matches none of {supported()} (the file name is not used to choose a version)')


def identify_base(program):
    """A base-only version (no update) by its base Program NCA."""
    for version, pinned in VERSIONS.items():
        nca = pinned['base_program_nca']
        if 'update_program_nca' not in pinned and (program['name'], program['size']) == (nca['name'], nca['size']):
            return version
    raise ValueError(f'unsupported base game: its Program NCA {program["name"]} belongs to none of {supported()}')


def identify_update(program):
    for version, pinned in VERSIONS.items():
        if 'update_program_nca' not in pinned:
            continue
        nca = pinned['update_program_nca']
        if (program['name'], program['size']) == (nca['name'], nca['size']):
            return version
    raise ValueError(f'unsupported update: its Program NCA {program["name"]} belongs to none of {supported()}')

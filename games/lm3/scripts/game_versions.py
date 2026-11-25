"""Known original inputs. New versions never replace an existing version's pins."""
import hashlib
import json

VERSIONS = {
    'v120': {
        'display': '1.2.0',
        'modules': {'main': '9fbb9dfcac34faee3f058ae5d1abdc433198bbac3b838a1810c032470ae8f2eb'},
    },
    # Update CNMT 327680, verified PFS0 ExeFS and NSO segment hashes.
    'v140': {
        'display': '1.4.0', 'cnmt_version': 327680,
        'romfs_sha256': 'c137d740159a4450c4da94800f3bec29e054d8c19892784516b08732858d69f1',
        'modules': {
            'main': '0689854bc8af51316daaa1978a8aceecd45bb2ba19a4c39496739201946c6283',
            'main.npdm': '605570894f4d4d9c0491262d14cf91776cb4c0b1ce01b457aa1131f7b04cf1b9',
            'rtld': '4e77d3fb96ed4808d4ede7a8680cdfdbdaf3a85b99f5971ad43cd1bbf5179986',
            'sdk': '754cf4b581d749fbe3d457cf66497b141ef994cf592d247919d64a37dcc30d27',
            'subsdk0': 'b39d72bc505d540515028f7ea293bd6ab9daca76f41aa19a2767fa705b527108',
            'subsdk1': '1778b9934561f24cbdae72314add6236645b906a411787f58f7cc328c7648c55',
        },
    },
}


def verify_exefs(folder, version):
    spec = VERSIONS[version]
    for name, expected in spec['modules'].items():
        with (folder / name).open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                raise ValueError(f'Unsupported LM3 {name}: expected {spec["display"]} original SHA-256')
    manifest = folder.parent / 'extraction-manifest.json'
    if manifest.is_file():
        metadata = json.loads(manifest.read_text()).get('content_meta')
        if metadata and 'cnmt_version' in spec and metadata['version'] != spec['cnmt_version']:
            raise ValueError('CNMT version differs from selected game version')
    return spec


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exefs', type=Path)
    parser.add_argument('--version', choices=VERSIONS, default='v120')
    args = parser.parse_args()
    print('Verified LM3 ' + verify_exefs(args.exefs, args.version)['display'] + ' original modules')

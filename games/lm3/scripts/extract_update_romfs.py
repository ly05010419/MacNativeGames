#!/usr/bin/env python3
"""Merge a base XCI/NSP and an update NSP with the pinned hactool BKTR reader.

Temporary common-ticket keys are private files, never arguments or log output.
The caller additionally verifies the resulting image against its known version hash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from extract_exefs import (load_keys, container_entries, common_tickets, nca_inventory,
                           program_nca, record_offset, require)
from extract_romfs import index_romfs

# Lockpick_RCM writes these as 17 bytes: the 16-byte source plus a trailing NUL.
# hactool exits unless the value is exactly 16 bytes, before any BKTR check.
_LOCKPICK_KEK_SOURCE = re.compile(r'(?:mariko_)?master_kek_source_[0-9a-f]{2}\Z')


def copy_range(source, offset, size, target):
    with source.open('rb') as src, target.open('wb') as dst:
        src.seek(offset)
        while size:
            data = src.read(min(size, 8 << 20))
            require(bool(data), 'Truncated NCA')
            dst.write(data)
            size -= len(data)


def hactool_prod_keys(source, target):
    """Copy prod.keys for hactool. The caller's file is left unchanged.

    hactool knows no comments: it reads the rest of a line as the value, so a trailing `# note` makes it exit. The copy
    keeps only the `name = value` lines, read the way load_keys reads them (comments dropped, spaces trimmed).
    A Lockpick trailing NUL on master_kek_source / mariko_master_kek_source is dropped. Any other length is rejected
    without recording the key bytes.
    """
    lines = []
    for number, line in enumerate(Path(source).read_text().splitlines(), 1):
        body = line.split('#', 1)[0]
        if '=' not in body:
            continue
        name, value = (part.strip() for part in body.split('=', 1))
        if _LOCKPICK_KEK_SOURCE.fullmatch(name.lower()) is not None:
            require(re.fullmatch(r'[0-9a-fA-F]+', value) is not None, f'Invalid key encoding at line {number}')
            if len(value) == 34 and value[-2:].lower() == '00':
                value = value[:32]
            require(len(value) == 32, f'{name.lower()} must be 16 bytes')
        lines.append(f'{name} = {value}')
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write('\n'.join(lines) + ('\n' if lines else ''))


def merge(base, update, keys_path, hactool, output, *, base_romfs=None):
    require(not output.exists(), 'Output exists')
    keys = load_keys(keys_path)
    inputs = []
    tickets = {}
    sources = ((update, True),) if base_romfs else ((base, False), (update, True))
    for source, is_update in sources:
        with source.open('rb') as f:
            kind, entries = container_entries(f, source.stat().st_size)
            require(not is_update or kind == 'nsp', 'Update must be an NSP')
            found = common_tickets(f, entries)
            for rights, key in found.items():
                require(rights not in tickets or tickets[rights] == key, 'Conflicting common tickets')
                tickets[rights] = key
            record, header = program_nca(nca_inventory(f, entries, keys), update=is_update)
            rights = header[0x230:0x240]
            require(rights == bytes(16) or rights in tickets, 'Missing common ticket')
            inputs.append((source, record, header))
    sections = [i for i in range(4) if inputs[-1][2][0x400+i*512+4] == 4]
    require(len(sections) == 1, 'Expected one BKTR section')
    output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(output.parent).free > inputs[-1][1]['size'] + 18*(1 << 30),
            'Need about 19 GiB free for the temporary update NCA and merged image')
    if base_romfs:
        require(Path(base_romfs).is_file(), 'Previous-version RomFS is missing')
    with tempfile.TemporaryDirectory(prefix='update-merge-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        keyfile = tmp/'title.keys'
        fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            for rights, key in tickets.items():
                f.write(f'{rights.hex()} = {key.hex()}\n')
        source, record, _ = inputs[-1]
        copy_range(source, record_offset(record), record['size'], tmp/'update.nca')
        result = tmp/'result'
        result.mkdir()
        base_arg = '--baseromfs='+str(Path(base_romfs).resolve()) if base_romfs else '--basenca='+str(base.resolve())
        environment = dict(os.environ, MNG_TITLE_KEYS=str(keyfile))
        environment.pop('MNG_BASE_RANGE_OFFSET', None); environment.pop('MNG_BASE_RANGE_SIZE', None)
        if not base_romfs:
            environment.update(MNG_BASE_RANGE_OFFSET=str(record_offset(inputs[0][1])),
                               MNG_BASE_RANGE_SIZE=str(inputs[0][1]['size']))
        hactool_keys = tmp/'hactool.keys'
        hactool_prod_keys(keys_path, hactool_keys)
        command = [str(hactool.resolve()), '-x', '-y', '--disablekeywarns', '--suppresskeys',
                   '-k', str(hactool_keys.resolve()), base_arg,
                   f'--section{sections[0]}='+str(result/'romfs.bin'), str(tmp/'update.nca')]
        with (result/'merge.log').open('w') as log:
            code = subprocess.run(command, env=environment,
                                  stdout=log, stderr=subprocess.STDOUT).returncode
        log = (result/'merge.log').read_text()
        # hactool also verifies the NCA's fixed-key and NPDM signatures, which need Nintendo's public keys:
        # without them it always prints those two lines as (FAIL). They are not data failures; the six IVFC
        # levels and the image SHA-256 recorded below decide whether the merge worked.
        unsigned = re.compile(r'\s*(?:Fixed-Key Signature|NPDM Signature) \(FAIL\)')
        checked = '\n'.join(line for line in log.splitlines() if not unsigned.match(line))
        if code or re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', checked):
            # hactool repeats a key it cannot parse ("Key (<value>) must be 32 hex digits!"): keep no long hex in the log
            (output.parent/(output.name+'-failure.log')).write_text(re.sub(r'[0-9A-Fa-f]{32,}', '<hex removed>', log))
        require(code == 0 and not re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', checked), 'BKTR verification failed')
        require(all(re.search(rf'Level {i} \(GOOD\)', log) for i in range(6)), 'Missing verified IVFC levels')
        image = result/'romfs.bin'
        with image.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        files = index_romfs(image)
        report = dict(status='pass', romfs_size=image.stat().st_size, romfs_sha256=digest,
                      files=files, integrity='BKTR merge; all six IVFC levels verified')
        (result/'index.json').write_text(json.dumps(report, indent=2)+'\n')
        result.rename(output)
    print(json.dumps(dict(status='pass', files=len(files), sha256=digest)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('update', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--hactool', required=True, type=Path)
    parser.add_argument('--base-romfs', type=Path, help='full decrypted base IVFC section, including hash levels (not a raw RomFS image)')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    merge(args.base, args.update, args.keys, args.hactool, args.output, base_romfs=args.base_romfs)

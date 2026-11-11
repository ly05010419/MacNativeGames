#!/usr/bin/env python3
"""The integrity checks of the extraction, each made to fail on purpose on a clone of the player's containers (never on the originals, whose SHA-256 is checked
before and after). Run by hand with the real files; writes <evidence>/result.json. The unmutated control is extract_ss.py's own run on the same files.

  integrity_mutations.py --base X.xci --update U.nsp --keys prod.keys --work-dir W --evidence NEW_DIR

Cases: data (one byte of the base game that the merged RomFS is shown to read, then extraction: exit 3 naming only that file), signature (update program header: 5),
hash (update program body: 6), and the base-meta exception (another container: 5; another meta: 5; an update meta: 5, never tolerated)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME/'scripts'))

EXCEPTION = json.loads((GAME/'assets/integrity-exceptions.json').read_text())['base_meta']
TARGET = '/region_common/map/Field.bntx'


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def table(f, base, magic):
    f.seek(base); h = f.read(16)
    if h[:4] != magic: raise ValueError(f'no {magic.decode()} at {base:#x}')
    n, strsz = struct.unpack_from('<II', h, 4); es = 0x40 if magic == b'HFS0' else 0x18
    raw = f.read(n*es+strsz); names = raw[n*es:]; data = base+16+n*es+strsz
    return {names[no:names.index(b'\0', no)].decode(): data+o for o, s, no in (struct.unpack_from('<QQI', raw, i*es) for i in range(n))}


def offsets(path):
    """{file name: offset in the container} of an NSP or of an XCI's secure partition."""
    with path.open('rb') as f:
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            f.seek(0x130); root = struct.unpack('<Q', f.read(8))[0]
            return table(f, table(f, root, b'HFS0')['secure'], b'HFS0')
        return table(f, 0, b'PFS0')


def clone(src, dst, at):
    """An APFS clone of src with one bit flipped at byte at (the original's blocks are shared until written; it is not touched)."""
    subprocess.run(['cp', '-c', str(src), str(dst)], check=True)
    with dst.open('r+b') as f:
        f.seek(at); b = f.read(1); f.seek(at); f.write(bytes([b[0] ^ 1]))
    return dst


def extract(tools, keys, base, update, out, log, *extra):
    with log.open('w') as stream:
        code = subprocess.run([str(tools/'nx_extract/nx_extract'), str(keys), str(base), str(update), str(out), *extra], stdout=stream, stderr=subprocess.STDOUT).returncode
    shutil.rmtree(out, ignore_errors=True)
    return code, log.read_text(errors='replace')


def locate(tools, keys, base, update, *args):
    r = subprocess.run([str(tools/'nx_locate/nx_locate'), *args[:1], str(keys), str(base), str(update), EXCEPTION['meta_sha256'], *map(str, args[1:])],
                       capture_output=True, text=True, check=True)
    return [l for l in r.stdout.splitlines() if not l.startswith('[')]


def case_data(tools, keys, x, u, ev, extracted):
    """The byte is shown to come from the base game before the outcome counts: the merged section reads it (window before and after), then the hash tree must catch it."""
    j = json.loads(locate(tools, keys, x, u, 'locate', TARGET)[0])
    out = dict(location=j)
    if not j['from_base'] or j['base_encryption'] != 'AesCtr':
        return dict(out, status='invalid', why='the window is not read from the base game, or not plain AES-CTR')
    on_disk = (extracted/'RomFS'/TARGET.lstrip('/')).open('rb')
    on_disk.seek(j['file_size']//2); disk = on_disk.read(64).hex(); on_disk.close()
    merged, base = (l.split()[1] for l in locate(tools, keys, x, u, 'window', j['section_offset'], j['base_section_offset']))
    out['control'] = dict(merged_equals_base=merged == base, merged_equals_file=merged == disk == j['window'])
    if not (merged == base == disk == j['window']):
        return dict(out, status='invalid', why='the unmutated windows differ')
    mutated = clone(x, ev/'data.xci', offsets(x)['27bc536b91a7089b9f2dac80720c08ba.nca']+j['nca_offset'])
    m_merged, m_base = (l.split()[1] for l in locate(tools, keys, mutated, u, 'window', j['section_offset'], j['base_section_offset']))
    diff = [i for i in range(64) if bytes.fromhex(m_merged)[i] != bytes.fromhex(merged)[i]]
    shown = diff == [0] and bytes.fromhex(m_merged)[0] ^ bytes.fromhex(merged)[0] == 1 and m_base == m_merged
    out['read_through'] = dict(changed_bytes=diff, merged_reads_the_changed_base_byte=shown)
    if not shown:
        mutated.unlink(); return dict(out, status='invalid', why='the merged RomFS does not read the changed byte')
    code, log = extract(tools, keys, mutated, u, ev/'data-out', ev/'data.log', '--skip-content-hash', '--tolerated-base-meta', EXCEPTION['meta_sha256'])
    mutated.unlink()
    damaged = re.findall(r'^\[RED\] hash check failed: (.+)$', log, re.M)
    out.update(exit=code, damaged=damaged)
    return dict(out, status='pass' if code == 3 and damaged == [TARGET] else 'fail')


def case_exit(tools, keys, x, u, ev, name, which, at, want_code, want_line, tolerate=True):
    """One byte changed in a copy, extraction expected to stop with want_code and the line that says why, before writing anything."""
    src = x if which == 'base' else u
    mutated = clone(src, ev/f'{name}{src.suffix}', at)
    args = ['--tolerated-base-meta', EXCEPTION['meta_sha256']] if tolerate else []
    code, log = extract(tools, keys, mutated if which == 'base' else x, mutated if which == 'update' else u, ev/f'{name}-out', ev/f'{name}.log', *args)
    mutated.unlink()
    ok = code == want_code and want_line in log and '[select]' in log and 'files' not in log.split(want_line)[-1]
    return dict(status='pass' if ok else 'fail', exit=code, expected_exit=want_code, expected_line=want_line, byte=at)


def case_other_container(args, ev):
    """The exception names the container by its SHA-256: a clone with a changed byte outside every NCA gets no tolerance from extract_ss.py (exit 5)."""
    mutated = clone(args.base, ev/'other-container.xci', 0x10)
    r = subprocess.run([sys.executable, str(GAME/'scripts/extract_ss.py'), '--base', str(mutated), '--update', str(args.update), '--keys', str(args.keys),
                        '--libhac', str(args.libhac), '--work-dir', str(args.work_dir), '--output', str(ev/'other-container-out'),
                        '--evidence', str(ev/'other-container')], capture_output=True, text=True)
    mutated.unlink(); shutil.rmtree(ev/'other-container-out', ignore_errors=True)
    result = json.loads((ev/'other-container/result.json').read_text())
    ok = r.returncode == 1 and result['exit'] == 5 and not result['tolerance_offered']
    return dict(status='pass' if ok else 'fail', exit=result['exit'], tolerance_offered=result['tolerance_offered'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base', 'update', 'keys', 'libhac', 'work_dir', 'evidence', 'extracted'):
        parser.add_argument('--'+name.replace('_', '-'), required=True, type=Path)
    args = parser.parse_args()
    for k in ('base', 'update', 'keys', 'libhac', 'work_dir', 'evidence', 'extracted'):
        setattr(args, k, getattr(args, k).expanduser().resolve())
    ev, tools = args.evidence, args.work_dir/'tools'
    ev.mkdir(parents=True)
    before = {p.name: sha256(p) for p in (args.base, args.update)}
    if before[args.base.name] != EXCEPTION['container_sha256']:
        raise ValueError('not the container the exception names')
    xo, uo = offsets(args.base), offsets(args.update)
    k, x, u = args.keys, args.base, args.update
    results = {}
    try:
        results['data'] = case_data(tools, k, x, u, ev, args.extracted)
        results['signature'] = case_exit(tools, k, x, u, ev, 'signature', 'update', uo['88d7f50b596192d406c20e6c395dfb80.nca']+0x10, 5,
                                         "[RED] update: the program NCA's header signature is Invalid")
        results['hash'] = case_exit(tools, k, x, u, ev, 'hash', 'update', uo['88d7f50b596192d406c20e6c395dfb80.nca']+0x2000000, 6, '[RED] update: 88d7f50b')
        # in the unused second section header (0x600): outside the card partition's hashed region (0x200), the signed part (0x200-0x3ff) and the meta's one section,
        # so the meta still opens and only its SHA-256 differs from the one the exception names
        results['other_meta'] = case_exit(tools, k, x, u, ev, 'other-meta', 'base', xo[EXCEPTION['meta_nca']]+0x610, 5,
                                          "[RED] base game: the meta NCA's header signature is Invalid")
        results['update_meta'] = case_exit(tools, k, x, u, ev, 'update-meta', 'update', uo['1a304c2fc433e643284c482bbb9de52c.cnmt.nca']+0x10, 5,
                                           "[RED] update: the meta NCA's header signature is Invalid")
        results['other_container'] = case_other_container(args, ev)
    finally:
        after = {p.name: sha256(p) for p in (args.base, args.update)}
        results['originals_unchanged'] = before == after
        status = 'pass' if after == before and all(isinstance(v, dict) and v.get('status') == 'pass' for v in results.values() if v is not True) else 'fail'
        (ev/'result.json').write_text(json.dumps(dict(status=status, cases=results), indent=2)+'\n')
    print(json.dumps({key: (v if v is True else v.get('status')) for key, v in results.items()}|{'status': status}))
    return 0 if status == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())

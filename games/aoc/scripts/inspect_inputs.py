#!/usr/bin/env python3
"""Validate an extracted AoC image without keys or container tooling.

This verifies NSO segment hashes and records extracted-file identities. It cannot
authenticate NCA signatures, BKTR composition, or container integrity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

GAME = Path(__file__).resolve().parents[1]
TITLE = '01002B00111A2000'
BUILDS = dict(main='c3cf52bf2b05d7310884971b50bcccb8',
              subsdk0='3ab92c3444d4ece2f52417b5d8ee86eb',
              sdk='95832210b36121e88b4f9b7b66239dad')


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def nso(path):
    import lz4.block
    b = path.read_bytes()
    if len(b) < 256 or b[:4] != b'NSO0':
        raise ValueError(f'not an original NSO: {path.name}')
    flags = struct.unpack_from('<I', b, 12)[0]
    segments = []
    for i, name in enumerate(('text', 'rodata', 'data')):
        offset, address, size = struct.unpack_from('<III', b, 0x10 + 16*i)
        stored = struct.unpack_from('<I', b, 0x60 + 4*i)[0]
        if offset < 256 or offset + stored > len(b):
            raise ValueError(f'truncated NSO segment: {path.name}/{name}')
        raw = b[offset:offset+stored]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if len(raw) != size:
            raise ValueError(f'NSO segment size mismatch: {path.name}/{name}')
        digest = hashlib.sha256(raw).hexdigest()
        verified = bool(flags & (1 << (i+3)))
        if verified and digest != b[0xa0+32*i:0xc0+32*i].hex():
            raise ValueError(f'NSO segment hash mismatch: {path.name}/{name}')
        segments.append(dict(name=name, address=address, size=size,
                             sha256=digest, header_hash_verified=verified))
    return dict(size=len(b), sha256=hashlib.sha256(b).hexdigest(),
                build_id=b[0x40:0x60].hex(), segments=segments,
                mapped_size=segments[2]['address']+segments[2]['size']+
                struct.unpack_from('<I', b, 0x3c)[0])


def npdm(path):
    b = path.read_bytes()
    if len(b) < 128 or b[:4] != b'META':
        raise ValueError('invalid main.npdm META header')
    offset, size = struct.unpack_from('<II', b, 0x70)
    if size < 0x40 or offset + size > len(b) or b[offset:offset+4] != b'ACI0':
        raise ValueError('invalid main.npdm ACI0 range')
    program = struct.unpack_from('<Q', b, offset+0x10)[0]
    if f'{program:016X}' != TITLE:
        raise ValueError(f'unsupported NPDM program ID: {program:016X}')
    sys_resource = struct.unpack_from('<I', b, 0x14)[0]
    if sys_resource != 0:
        raise ValueError(f'unsupported NPDM system resource: {sys_resource:#x}')
    return dict(size=len(b), sha256=hashlib.sha256(b).hexdigest(),
                program_id=f'{program:016X}', sys_resource=sys_resource,
                main_thread_priority=b[0xe], main_thread_core=b[0xf],
                main_thread_stack=struct.unpack_from('<I', b, 0x1c)[0],
                signature_verification='not performed')


def tree_manifest(root):
    if not root.is_dir():
        raise ValueError('RomFS must be the supplied extracted directory tree; images are not accepted by this input mode')
    rows = []
    for p in sorted(root.rglob('*')):
        if p.is_symlink():
            raise ValueError(f'symlink in original tree: {p}')
        if p.is_file() and p.name != '.DS_Store':
            rows.append(dict(path=p.relative_to(root).as_posix(), size=p.stat().st_size, sha256=sha256(p)))
    if not rows:
        raise ValueError('empty RomFS directory')
    identity = ''.join(f'{r["sha256"]}\t{r["size"]}\t{r["path"]}\n' for r in rows)
    return dict(form='directory', files=len(rows), bytes=sum(r['size'] for r in rows),
                sha256=hashlib.sha256(identity.encode()).hexdigest(), entries=rows)


def inspect(root, pins_path=None):
    root = root.expanduser().resolve()
    exefs, romfs = root/'ExeFS', root/'RomFS'
    if not exefs.is_dir():
        raise ValueError('expected extracted root containing ExeFS/ and RomFS/')
    expected = set(BUILDS) | {'rtld', 'main.npdm'}
    actual = {p.name for p in exefs.iterdir() if p.name != '.DS_Store'}
    if actual != expected or any(p.is_symlink() for p in exefs.iterdir()):
        raise ValueError(f'unsupported ExeFS file set: {sorted(actual)}')
    modules = {name:nso(exefs/name) for name in sorted(set(BUILDS) | {'rtld'})}
    for name, bid in BUILDS.items():
        if not modules[name]['build_id'].startswith(bid):
            raise ValueError(f'unsupported executable build: {name} {modules[name]["build_id"]}')
    meta = npdm(exefs/'main.npdm')
    if pins_path:
        pins = json.loads(pins_path.read_text())
        for name, pin in pins['modules'].items():
            if any(modules[name][k] != pin[k] for k in ('size','sha256','build_id')):
                raise ValueError(f'original ExeFS hash mismatch: {name}')
        if meta != pins['npdm']:
            raise ValueError('original NPDM differs from pinned profile')
    rom = tree_manifest(romfs)
    if pins_path and any(rom[k] != pins['romfs'][k] for k in ('form','files','bytes','sha256')):
        raise ValueError('original RomFS hash mismatch')
    if not (romfs/'asset/MaterialEditor.rdb.bin').is_file():
        raise ValueError('missing KTGL shader resource')
    return dict(title_id=TITLE, version='1.3.0', version_code=327680,
                identity_basis='pinned NSO build IDs, NPDM and extracted-file SHA-256',
                container_integrity='unavailable: original containers not supplied',
                base_update_composition='not independently verifiable from extracted files',
                modules=modules, npdm=meta, romfs=rom)


def write_report(report, destination, public=False):
    report = dict(report)
    if public:
        report['romfs'] = {k:v for k,v in report['romfs'].items() if k != 'entries'}
    destination.parent.mkdir(parents=True,exist_ok=True)
    temporary = destination.with_suffix('.partial.json')
    temporary.write_text(json.dumps(report,indent=2)+'\n')
    temporary.replace(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs',type=Path)
    parser.add_argument('--report',required=True,type=Path)
    args=parser.parse_args()
    write_report(inspect(args.inputs),args.report)

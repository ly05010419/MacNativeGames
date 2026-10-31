#!/usr/bin/env python3
"""Prepare a PRIVATE player-cheat catalog from supplied TXT and verified retail NSO.

The output contains game instructions: keep it outside the source checkout. This
reads data-format records only; it never modifies an NSO or a rewritten image.
Optional metadata maps original entry titles to name/category/group/description,
with `exclude: true` to omit a record and `missing` for unavailable UI entries.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
from mod_exefs import Nso
import patch_sites


def parse_text(text):
    title = None
    words = []
    for line in text.splitlines() + ['[END]']:
        line = line.strip().lstrip('\ufeff')
        if line.startswith('[') and line.endswith(']'):
            if title and words and not any(x in title.lower() for x in ('restore code', 'sectionstart:', 'sectionend:')):
                yield title, words
            title, words = line[1:-1], []
        elif re.fullmatch(r'[0-9a-fA-F]{8}(?:\s+[0-9a-fA-F]{8})*', line):
            words.extend(line.upper().split())


def word_count(w):
    op, t = w >> 28, (w >> 24) & 15
    if op in (0, 1):
        return 4 if t == 8 else 3
    if op == 3:
        return 2 if t == 0 else 1
    if op in (4, 6):
        return 3
    if op in (5, 7):
        return 2
    if op == 9:
        return (3 if t == 8 else 2) if (w >> 8) & 15 else 1
    if op == 10:
        return 2 if ((w >> 8) & 15) in (2, 4, 5) else 1
    if w >> 24 == 0xC0:
        kind = (w >> 8) & 15
        return (3 if ((w >> 20) & 15) == 8 else 2) if kind == 4 else 2 if kind in (0, 2) else 1
    if op in (2, 8):
        return 1
    raise ValueError(f'unsupported opcode {w:08X}')


def direct_writes(words):
    raw = [int(x, 16) for x in words]
    at = 0
    modified = set()
    while at < len(raw):
        w = raw[at]
        n = word_count(w)
        if at + n > len(raw):
            raise ValueError('truncated record')
        op, reg = w >> 28, (w >> 16) & 15
        if op == 0 and ((w >> 20) & 15) == 0 and reg not in modified:
            width = (w >> 24) & 15
            address = ((w & 255) << 32) | raw[at + 1]
            value = ((raw[at + 2] << 32) | raw[at + 3]) if width == 8 else raw[at + 2]
            if width not in (1, 2, 4, 8):
                raise ValueError('invalid width')
            yield address, value.to_bytes(width, 'little')
        if op in (3, 4, 5, 7, 9):
            modified.add(reg)
        at += n


def build(nso, sources, version, metadata=None):
    metadata = metadata or {}
    image = nso.image()
    reserved = patch_sites.sites(version, nso.build_id)
    entries, guards = [], {}
    for path in sources:
        if path.stem.upper() != nso.build_id[:16].upper():
            raise ValueError(f'{path.name}: filename does not match retail NSO build ID')
        for title, words in parse_text(path.read_text(encoding='utf-8-sig')):
            meta = metadata.get('titles', {}).get(title, {})
            if meta.get('exclude'):
                continue
            record = dict(meta)
            record.update(id=hashlib.sha256((str(path.resolve()) + '\n' + title).encode()).hexdigest()[:20],
                          name=meta.get('name', title), words=words,
                          source=meta.get('source', f'{path.name} · {title}'))
            record.setdefault('category', '更多')
            writes = {}
            try:
                for offset, payload in direct_writes(words):
                    if offset + len(payload) > len(image):
                        raise ValueError('direct write outside retail image')
                    if patch_sites.overlaps([(offset, len(payload))], reserved):
                        raise ValueError('overlaps a runtime/rewriter patch site')
                    if offset < nso.segments[2].address:
                        if offset % 4 or len(payload) not in (4, 8):
                            raise ValueError('readonly write must contain aligned words')
                        for i in range(0, len(payload), 4):
                            guards[offset + i] = int.from_bytes(image[offset+i:offset+i+4], 'little')
                    for i, byte in enumerate(payload):
                        writes[str(offset+i)] = byte
                record['writes'] = writes
            except (ValueError, OverflowError) as exc:
                record.pop('words', None)
                record['reason'] = f'需适配：{exc}'
            entries.append(record)
    for name in metadata.get('missing', []):
        entries.append(dict(id='missing-' + hashlib.sha256(name.encode()).hexdigest()[:16], name=name,
                            category='更多', reason='待适配：当前金手指包未提供该项 1.2.1 代码'))
    return dict(schema=1, build_id=nso.build_id.upper(), version=version,
                entries=entries, guards=[dict(offset=o, original=v) for o, v in sorted(guards.items())])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--nso', required=True, type=Path)
    p.add_argument('--version', required=True, choices=patch_sites.VERSIONS)
    p.add_argument('--source', action='append', required=True, type=Path)
    p.add_argument('--metadata', type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    checkout = Path(__file__).resolve().parents[3]
    if a.output.resolve().is_relative_to(checkout):
        p.error('private catalog must be outside the source checkout')
    nso = Nso(a.nso.read_bytes())
    result = build(nso, a.source, a.version, json.loads(a.metadata.read_text()) if a.metadata else None)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(f'{len(result["entries"])} entries, {len(result["guards"])} original-word guards; private catalog written')


if __name__ == '__main__':
    main()

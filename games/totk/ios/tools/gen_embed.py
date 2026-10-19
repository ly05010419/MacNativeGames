#!/usr/bin/env python3
"""Emit the iOS executable sections for the already patched guest modules."""
import hashlib, json, sys
from pathlib import Path

PG = 0x4000
def up(n): return (n + PG - 1) & ~(PG - 1)

root, out = Path(sys.argv[1]), Path(sys.argv[2])
args = sys.argv[3:]
text_dir = None
if '--text-dir' in args:
    i = args.index('--text-dir')
    text_dir = Path(args[i + 1])
    del args[i:i + 2]
names = args
asm = ['// Generated from the locally verified game modules.']
entries = []
flags = []
for name in names:
    image = root / f'{name}.patched.bin'
    hdr = dict(line[2:].split() for line in (root / f'{name}.fixups.tsv').read_text().splitlines())
    data = image.read_bytes(); text_end = int(hdr['TEXT_END'], 16); patch = int(hdr['PATCH_OFF'], 16)
    text_image = (text_dir / f'{name}.x0.bin') if text_dir else image
    text_data = text_image.read_bytes() if text_dir else data
    key = int(hdr['KEY_PAGE_OFF'], 16); size = int(hdr.get('FILE_IMAGE_LEN', f'{len(data):x}'), 16)
    x0, x1 = up(text_end), up(len(data))
    if len(text_data) != x0:
        raise ValueError(f'{name}: exported x0 must be exactly {x0:#x} bytes, got {len(text_data):#x}')
    if patch < 0:
        file_end = int(hdr['FILE_IMAGE_LEN'], 16)
        if file_end % PG or x0 > file_end or key % PG: raise ValueError(f'{name}: invalid far patch layout')
        parts = [(f'__TOTKX,__{name}x0', 0, x0), (f'__TOTK{name}W,__img', x0, file_end), (f'__TOTKX,__{name}x1', file_end, x1)]
    else:
        if patch % PG or x0 > key or key + PG > patch: raise ValueError(f'{name}: invalid patch layout')
        parts = [(f'__TOTKX,__{name}x0', 0, x0), (f'__TOTK{name}W,__img', x0, patch), (f'__TOTKX,__{name}x1', patch, x1)]
    hashes=[]
    for sect, lo, hi in parts:
        if any(len(s) > 16 for s in sect.split(',')): raise ValueError(f'{name}: Mach-O section name too long: {sect}')
        asm += [f'    .section {sect}', '    .p2align 14']
        section_image = text_image if sect == f'__TOTKX,__{name}x0' else image
        if sect == f'__TOTKX,__{name}x0': chunk_source = text_data
        else: chunk_source = data
        real = max(0, min(hi, len(chunk_source))-lo)
        if real: asm += [f'    .incbin "{section_image.resolve()}", {lo:#x}, {real:#x}']
        if hi-lo > real: asm += [f'    .space {hi-lo-real:#x}']
        asm += ['    .p2align 14']
        chunk=chunk_source[lo:min(hi,len(chunk_source))] if lo < len(chunk_source) else b''
        if len(chunk)<hi-lo: chunk += bytes(hi-lo-len(chunk))
        hashes.append(hashlib.sha256(chunk).hexdigest())
    entries.append({'name':name,'x0':hashes[0],'w':hashes[1],'x1':hashes[2]})
    flags.append(f'-Wl,-segprot,__TOTK{name}W,rw,rw')
flags.append('-Wl,-segprot,__TOTKX,r-x,r-x')
out.write_text('\n'.join(asm)+'\n')
header = ['/* Generated locally; hashes bind app contents to the verified rewrites. */',
          'typedef struct { const char *name, *x0, *w, *x1; } IosEmbed;', 'static const IosEmbed ios_embed[] = {']
header += [f'{{"{e["name"]}","{e["x0"]}","{e["w"]}","{e["x1"]}"}},' for e in entries]
header += ['};']
(out.parent/'ios_embed.h').write_text('\n'.join(header)+'\n')
print(' '.join(flags))

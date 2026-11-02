#!/usr/bin/env python3
"""Bind the native loader to the verified images: build/game-data/<version>/runtime-tables.bin (stays local).

The loader reads the file at start (runtime/loader.c load_tables), so the program itself holds nothing of the game."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/d2r'))
from relr import decode_relr
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--version', choices=('v0','v112','v121','v143','botw160','botw190'), default='v0')
version = parser.parse_args().version
OUT = ROOT / f'build/game-data/{version}'
OUT.mkdir(parents=True, exist_ok=True)
report = json.loads((ROOT / ('analysis/phase12-validation.json' if version == 'v0' else f'analysis/{version}-port/phase12-validation.json')).read_text())
assert report['status'] == 'pass' and report['rom_version'] == version
if version == 'v112':
    expected = 'f04fbebdaacbcde0bcca20cbe0caff06bd51f7c8f904d217e5f17efe3308d1ac'
    assert report['module_file_sha256']['extracted/v112/exefs/main'] == expected, 'Unsupported 1.1.2 main image'
if version == 'v121':
    assert report['module_file_sha256']['extracted/v121/exefs/main'] == 'e5aa8d48e68cbb82ed4416c7b6b29a46fe4a998ee149d6dbf979164c628efcb6', 'Unsupported 1.2.1 main image'
if version == 'botw160':   # Breath of the Wild 1.6.0 (botw.md)
    assert report['module_file_sha256']['extracted/botw160/exefs/main'] == '15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b', 'Unsupported BotW 1.6.0 main image'
if version == 'botw190':   # Breath of the Wild 1.9.0 (docs/botw-v1.9.0-port.md)
    assert report['module_file_sha256']['extracted/botw190/exefs/main'] == 'f6f0b20bb1f2b67d164c24871cd473147eef1d285f12e7bd492e3126c6022e27', 'Unsupported BotW 1.9.0 main image'
if version == 'v143':
    assert report['module_file_sha256']['extracted/v143/exefs/main'] == 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e', 'Unsupported 1.4.3 main image'
modules = []
for name in ['main', 'subsdk0', 'sdk']:
    path = ROOT / f'rewritten/{version}/{name}.patched.bin'
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    assert digest == report['module_file_sha256'][str(path.relative_to(ROOT))], 'Unverified rewrite'
    inv = json.loads((ROOT / f'extracted/{version}/elf/{name}-inventory.json').read_text())
    layout = {}
    for line in (ROOT / f'rewritten/{version}/{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        layout[key] = int(value, 0)
    mod0 = struct.unpack_from('<I', image, 4)[0]
    assert image[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags = {}
    for offset in range(dyn, dyn + 4096 * 16, 16):
        tag, value = struct.unpack_from('<qQ', image, offset)
        if tag == 0: break
        tags[tag] = value
    else: raise ValueError('Unterminated dynamic table')
    if 4 in tags:
        nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
    else:
        # Newer SDK modules carry GNU hash only. NSO's verified dynsym extent
        # supplies the exact count, as in prepare_modules.py's inventory gate.
        raw_header = (ROOT / f'extracted/{version}/exefs/{name}').read_bytes()[:256]
        _, _, symbol_offset, symbol_size = struct.unpack_from('<IIII', raw_header, 0x90)
        assert layout['RO_OFF'] + symbol_offset == tags[6] and symbol_size % 24 == 0
        assert 0 < symbol_size <= layout['DATA_OFF'] - tags[6]
        nsym = symbol_size // 24
    assert tags[11] == 24 and tags[9] == 24 and tags[20] == 7
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        end = image.index(0, tags[5] + no, tags[5] + tags[10])
        symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size,
                            defined=bool(section), binding=info >> 4, type=info & 15))
    relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
              for where, addend in decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
    relocated = {reloc['where'] for reloc in relocs}
    for address, size in [(tags[7], tags[8]), (tags[23], tags[2])]:
        assert size % 24 == 0
        for offset in range(address, address + size, 24):
            where, info, addend = struct.unpack_from('<QQq', image, offset)
            kind, si = info & 0xffffffff, info >> 32
            assert kind in [257, 1025, 1026, 1027] and si < nsym
            assert layout['DATA_OFF'] <= where <= layout['KEY_PAGE_OFF'] - 8
            assert kind != 1027 or 0 <= addend < layout['KEY_PAGE_OFF']
            assert where not in relocated, 'Duplicate RELR/RELA relocation'
            relocated.add(where)
            relocs.append(dict(where=where, kind=kind, symbol=si, addend=addend))
    gates = []
    for line in (ROOT / f'rewritten/{version}/{name}.gates.tsv').read_text().splitlines():
        ret, kind, arg, original = line.split()
        assert kind in ['cntpct', 'cntvct', 'svc', 'spin']
        # timer: 0 svc, 1 cntpct / cntvct, 2 spin wait (patch_nso.py "Spin waits"; threads.c runtime_spin_wait)
        gates.append(dict(ret=int(ret, 0), timer={'svc': 0, 'spin': 2}.get(kind, 1), arg=int(arg), original=int(original, 0)))
    gates.sort(key=lambda g: g['ret'])   # loader.c runtime_dispatch looks a return address up by binary search
    assert len({g['ret'] for g in gates}) == len(gates), 'duplicate gate return address'
    # Match each compiler's actual DT_INIT state load before decoding it.
    flag_reg, load_offset = (19, 20) if version in ('v143', 'botw160', 'botw190') else (20, 24)
    a, w = struct.unpack_from('<I', image, tags[12]+16)[0], struct.unpack_from('<I', image, tags[12]+load_offset)[0]
    assert a & 0x9f00001f == 0x90000000 | flag_reg and w & 0xffc003ff == 0xb9400008 | (flag_reg << 5)
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    ctor_flag = ((tags[12]+16) & ~4095) + (imm << 12) + ((w >> 10) & 0xfff)*4
    assert layout['DATA_OFF'] <= ctor_flag < layout['KEY_PAGE_OFF']
    modules.append(dict(name=name, sha256=digest, size=len(image), ctor_flag=ctor_flag, layout=layout, tags=tags,
                        symbols=symbols, relocs=relocs, gates=gates))

# Find main's ORIGINAL PLT stub addressing the relocated GetSystemTick slot.
main = modules[0]
target = '_ZN2nn2os13GetSystemTickEv'
slot = next(r['where'] for r in main['relocs'] if main['symbols'][r['symbol']]['name'] == target and r['kind'] == 1026)
image = (ROOT / f'rewritten/{version}/main.patched.bin').read_bytes()
plt = []
for address in range(main['layout']['TEXT_END'] - 0x10000, main['layout']['TEXT_END'] - 12, 4):
    a, b, c, d = struct.unpack_from('<IIII', image, address)
    if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220: continue
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    referenced = (address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8
    if referenced == slot: plt.append(address)
assert len(plt) == 1, f'PLT matches {plt}'
main['probe_plt'] = plt[0]
main['probe_slot'] = slot

# runtime-tables.bin in the layouts of runtime/loader.c load_tables: a 64-byte header, one 208-byte record per module, each
# module's symbols, relocations and gates (24 bytes each, 8-byte aligned), then the symbol names.
HEADER, RECORD = 64, 208
strings, arrays, records = bytearray(), bytearray(), []
def place(blob):
    arrays.extend(bytes(-len(arrays) % 8))
    arrays.extend(blob)
    return HEADER + RECORD * len(modules) + len(arrays) - len(blob)
for m in modules:
    names = []
    for s in m['symbols']:
        names.append(len(strings))
        strings.extend(s['name'].encode() + b'\0')
    symbols = b''.join(struct.pack('<QQIBBBx', s['value'], s['size'], n, int(s['defined']), s['binding'], s['type'])
                       for s, n in zip(m['symbols'], names))
    relocs = b''.join(struct.pack('<QIIq', r['where'], r['kind'], r['symbol'], r['addend']) for r in m['relocs'])
    gates = b''.join(struct.pack('<QQII', g['ret'], g['original'], int(g['timer']), g['arg']) for g in m['gates'])
    l, t = m['layout'], m['tags']
    records.append(struct.pack('<16s72s13Q4I', m['name'].encode(), m['sha256'].encode(), m['size'], l['TEXT_END'], l['RO_OFF'],
                               l['DATA_OFF'], l['PATCH_OFF'] % 2**64, l['KEY_PAGE_OFF'], t.get(12, 0), m['ctor_flag'], t.get(25, 0),
                               t.get(27, 0), place(symbols), place(relocs), place(gates), len(m['symbols']), len(m['relocs']),
                               len(m['gates']), 0))
arrays.extend(bytes(-len(arrays) % 8))
strings_offset = HEADER + RECORD * len(modules) + len(arrays)
size = strings_offset + len(strings)
header = struct.pack('<8s8sIIQQQQQ', b'TOTKTAB1', version.encode(), len(modules), 1, main['probe_plt'], slot, strings_offset,
                     len(strings), size)
assert len(header) == HEADER and all(len(r) == RECORD for r in records)
tables = OUT / 'runtime-tables.bin'
tables.with_suffix('.partial').write_bytes(header + b''.join(records) + arrays + strings)
tables.with_suffix('.partial').replace(tables)
# The patch_nso.py that rewrote these modules (it records its own SHA-256 next to them): update_runtime.py refuses an app
# whose rewriter is not the repository's, since a runtime can rely on gates and fixes a newer rewriter puts in the code.
rewriter = (ROOT / f'rewritten/{version}/build_src.sha256').read_text().split()[0]
assert len(rewriter) == 64 and all(c in '0123456789abcdef' for c in rewriter), 'build_src.sha256'
(OUT / 'rewriter.txt').write_text(f'# SHA-256 of the patch_nso.py that rewrote rewritten/{version}\nREWRITER_SHA256 {rewriter}\n')
# Details are evidence, not additional source code.
(ROOT / ('analysis/runtime-layout.json' if version == 'v0' else f'analysis/{version}-port/runtime-layout.json')).write_text(json.dumps(modules, indent=2) + '\n')
print(json.dumps(dict(modules=len(modules), relocations=sum(len(m['relocs']) for m in modules), original_main_plt=hex(plt[0]), original_main_slot=hex(slot))))

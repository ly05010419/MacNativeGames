#!/usr/bin/env python3
"""Find every NVN shader in Xenoblade Chronicles 3's own files, without playing: the merged RomFS (bf3.arh / bf3.ard and loose
files), the add-ons' bf3_dlc0N archives and the standalone SLCT in main.

Monolith keeps its shaders in HCPS containers (.wishp, and inside models and map streams), in ARH1 / ARD archives whose entries
are XBC1 streams (stored, zlib or zstd), nested to any depth; effect packs (.wipac) also keep complete GLSLC outputs, some behind
a further `cmp\\0zlib` layer. Each shader is keyed as the runtime keys it (nvn_mtl.m shader_key): the SHA-256 of the control
section's first ctl_key_len bytes and the code area's first control+0x704 bytes, so the keys name the same cache entries the
game asks for.

The container readers are ported from seedds/nxnative (MIT), dm/gfx/tools/shader_split.py (arh_entries, xbc_data,
monolith_cmp_data, hcps_shaders, slct_shaders, monolith_shaders, glslc_*); see third_party/COPIES.md. Formats: ARH1,
https://github.com/roccodev/ard-tools/blob/master/docs/arh1.md; HCPS, https://github.com/ScanMountGoat/xc3_lib (spch.rs).

Output: shaders are yielded as (key, stage, control, code); nothing derived from the game is written by this module.
usage (census): monolith_shaders.py <work>/extracted/v221 [<work>/extracted/aoc/N ...] [--keys <cache dir>]
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_romfs import index_romfs  # noqa: E402

CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
GLSLC_MAGIC = b'\x91\x68\x86\x19'


class Damaged(ValueError):
    pass


def red(msg):
    raise Damaged(msg)


def zstd_mod():
    try:
        from compression import zstd
    except ImportError:
        from backports import zstd
    return zstd


def ctl_key_len(ctl):
    """nvn_mtl.m ctl_key_len: the section's own size (+0x20) when it is 0x7cc..0x87f, else 0x880."""
    n = struct.unpack_from('<I', ctl, 0x20)[0]
    return n if 0x7cc <= n < 0x880 else 0x880


def shader_key(ctl, code):
    """nvn_mtl.m shader_key."""
    length = struct.unpack_from('<I', ctl, 0x704)[0]
    return hashlib.sha256(ctl[:ctl_key_len(ctl)] + code[:length]).hexdigest()


def sph_stage(code):
    if len(code) < 0x34:
        return None
    w = struct.unpack_from('<I', code, 0x30)[0]
    return 'vs' if (w >> 10) & 0xf == 0 and w & 0x1f == 1 else SPH_STAGE.get((w >> 10) & 0xf)


def check(st, ctl, code):
    """Control section and code area consistent; the reason if not."""
    if ctl[:4] != CTL_MAGIC or len(ctl) < 0x880 and len(ctl) < ctl_key_len(ctl):
        return 'bad control section'
    if st == 'cs' and code[:4] in (CS_MAGIC, GFX_MAGIC) and code[:4] != CS_MAGIC or st != 'cs' and code[:4] != GFX_MAGIC:
        return 'code area header magic does not match the stage'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code):
        return 'control +0x704 does not match the code area length'
    return None


def glslc_stage(ctl, code):
    headerless_cs = code[:4] not in (CS_MAGIC, GFX_MAGIC) and len(ctl) >= 0x754 and all(struct.unpack_from('<3I', ctl, 0x748))
    return 'cs' if code[:4] == CS_MAGIC or headerless_cs else sph_stage(code) if code[:4] == GFX_MAGIC else None


def glslc_section(d, h, size):
    """The GPU code sections of the GLSLC output at h: [(control, code)], or None."""
    n, secs = struct.unpack_from('<I', d, h + 0x4c)[0], []
    if not 0 < n <= 16 or h + 0x90 * (n + 1) > len(d):
        return None
    for i in range(n):
        s = h + 0x90 + 0x90 * i
        ssz, soff, typ = struct.unpack_from('<3I', d, s)
        if typ:
            continue
        co, ko, kn, cn = struct.unpack_from('<4I', d, s + 0x30)
        if soff + ssz > size or co + cn > ssz or ko + kn > ssz:
            return None
        secs.append((d[h + soff + co:h + soff + co + cn], d[h + soff + ko:h + soff + ko + kn]))
    return secs


def glslc_shaders(d, skip=()):
    """Complete GLSLC outputs (the effect packs): one stage each, or several read by their section headers."""
    out = []
    for m in re.finditer(GLSLC_MAGIC, d):
        h = m.start()
        if any(a <= h < b for a, b in skip) or h + 0x48 > len(d):
            continue
        size = struct.unpack_from('<I', d, h + 0x44)[0]
        c = d.find(CTL_MAGIC, h, h + size) if 0x760 < size <= len(d) - h else -1
        if c < 0:
            continue
        if d.find(CTL_MAGIC, c + 4, h + size) >= 0:
            secs = glslc_section(d, h, size) or []
            for ctl, code in secs:
                st = glslc_stage(ctl, code)
                if st and not check(st, ctl, code):
                    out.append((st, ctl, code))
            continue
        nctl, ncode = (struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]) if c + 0x708 <= h + size else (0, 0)
        ctl, code = d[c:c + nctl], d[c + nctl:c + nctl + ncode]
        st = glslc_stage(ctl, code) if nctl >= 0x760 and not nctl & 3 and c + nctl + ncode <= h + size else None
        if not st or check(st, ctl, code):
            sec = glslc_section(d, h, size)
            if not sec or len(sec) != 1:
                continue
            (ctl, code), = sec
            st = glslc_stage(ctl, code)
            if not st or check(st, ctl, code):
                continue
        out.append((st, ctl, code))
    return out


def arh_entries(d):
    """ARH1 index -> [(path, ARD offset, stored size)]. The XOR trie gives the paths; the file table the bounds."""
    if len(d) < 48 or d[:4] != b'arh1':
        return []
    _, count, so, sn, no, nn, fo, fn, key = struct.unpack_from('<9I', d, 4)
    if nn != count * 8 or any(o > len(d) or n > len(d) - o for o, n in ((so, sn), (no, nn), (fo, fn * 24))):
        red('ARH1: index table out of bounds')
    key = (key ^ 0xf3f35353).to_bytes(4, 'little')

    def dec(o, n):   # whole u32 words are encrypted; a final partial word stays plain
        return bytes(v ^ key[i & 3] for i, v in enumerate(d[o:o + (n & ~3)])) + d[o + (n & ~3):o + n]
    strings = dec(so, sn)
    nodes = list(struct.iter_unpack('<ii', dec(no, nn)))
    names = {}
    for i, (nxt, prev) in enumerate(nodes):
        if nxt >= 0 or prev < 0:
            continue
        off = -nxt
        end = strings.find(b'\0', off) if off < len(strings) else -1
        if end < 0 or end + 5 > len(strings) or not strings[off:end].isascii():
            red('ARH1: invalid path suffix')
        fid = struct.unpack_from('<I', strings, end + 1)[0]
        prefix, cur, seen = [], i, set()
        while cur:
            if cur in seen or not 0 <= cur < len(nodes):
                red('ARH1: invalid path trie')
            seen.add(cur)
            parent = nodes[cur][1]
            if not 0 <= parent < len(nodes):
                red('ARH1: invalid path parent')
            ch = cur ^ nodes[parent][0]
            if not 0 <= ch < 128:
                red('ARH1: invalid path character')
            prefix.append(ch)
            cur = parent
        names[fid] = (bytes(reversed(prefix)) + strings[off:end]).decode('ascii')
    out = []
    for i in range(fn):
        off, size, raw, flags, fid = struct.unpack_from('<Q4I', d, fo + i * 24)
        if size:   # HasXbc1Header (bit 1): the stored size leaves out the 48-byte wrapper
            out.append((names.get(fid, f'#{fid}'), off, size + (48 if flags & 2 else 0)))
    return out


def xbc_data(d, off):
    """An XBC1 stream at off: (decoded bytes, end), or None for bytes that only look like a header."""
    if off + 48 > len(d) or d[off:off + 4] != b'xbc1':
        return None
    kind, raw, size = struct.unpack_from('<3I', d, off + 4)
    if kind not in (0, 1, 3) or not size or off + 48 + size > len(d):
        return None
    data = d[off + 48:off + 48 + size]
    if kind == 1:
        data = zlib.decompress(data)
    elif kind == 3:
        data = zstd_mod().decompress(data)
    if len(data) != raw:
        red(f'XBC1 @{off:#x}: expected {raw} bytes, decoded {len(data)}')
    return data, off + 48 + size


def monolith_cmp_data(d, off):
    """Firehawk effect resources: cmp\\0zlib, stored / raw sizes, a zlib payload."""
    if off + 16 > len(d) or d[off:off + 8] != b'cmp\0zlib':
        return None
    size, raw = struct.unpack_from('<2I', d, off + 8)
    if not size or size > len(d) - off - 16:
        return None
    z = zlib.decompressobj()
    try:
        data = z.decompress(d[off + 16:off + 16 + size]) + z.flush()
    except zlib.error as e:
        red(f'cmp zlib @{off:#x}: damaged payload ({e})')
    if not z.eof or z.unused_data or len(data) != raw:
        red(f'cmp zlib @{off:#x}: payload does not match stored/raw sizes')
    return data, off + 16 + size


def hcps_shaders(d, base):
    """HCPS programs: SLCT gives each variant's control and code ranges, each NVSD's last stage record their sizes."""
    def span(off, n):
        if off < base or off > len(d) or n > len(d) - off:
            red(f'HCPS @{base:#x}: section out of bounds')
        return d[off:off + n]
    span(base, 72)
    table, count = struct.unpack_from('<2I', d, base + 8)
    slct_off, slct_size, code_off, code_size, ctl_off, ctl_size = struct.unpack_from('<6I', d, base + 24)
    span(base + table, count * 8)
    slcts, codes, ctls = (span(base + o, n) for o, n in ((slct_off, slct_size), (code_off, code_size), (ctl_off, ctl_size)))
    if not code_size and not ctl_size:
        return []   # source-only GLSL programs, no NVN binaries
    out = []
    for i in range(count):
        s = struct.unpack_from('<I', d, base + table + 8 * i)[0]
        if s + 72 > len(slcts) or slcts[s:s + 4] != b'SLCT':
            red(f'HCPS: invalid SLCT {i}')
        num, ptr = struct.unpack_from('<2I', slcts, s + 16)
        c, cn, k, kn = struct.unpack_from('<4I', slcts, s + 40)
        ce, ke = c + cn, k + kn
        if s + ptr + num * 8 > len(slcts) or ce > len(ctls) or ke > len(codes):
            red(f'HCPS SLCT {i}: variant range out of bounds')
        for j in range(num):
            po, pn = struct.unpack_from('<2I', slcts, s + ptr + j * 8)
            p = s + po
            if pn < 32 or p + pn > len(slcts):
                red('HCPS: invalid NVSD')
            vo, vn = struct.unpack_from('<2I', slcts, p + 24)
            if not vn or vo + vn * 20 > pn:
                red('HCPS: invalid NVSD stage records')
            _, vs, ps, vc, pc = struct.unpack_from('<5I', slcts, p + vo + (vn - 1) * 20)
            pend, kend = c + vc + pc, k + vs + ps
            if pend > ce or kend > ke:
                red('HCPS: program exceeds SLCT range')
            # a vertex bundle can include a geometry stage: NVSD's vertex size covers both; each control gives its own code size
            while c < pend:
                if c + 0x708 > pend:
                    red('HCPS: truncated control')
                nc = struct.unpack_from('<I', ctls, c + 0x20)[0]
                nk = struct.unpack_from('<I', ctls, c + 0x704)[0]
                if nc < 0x760 or c + nc > pend or not nk or k + nk > kend:
                    red('HCPS: shader exceeds program range')
                ctl, code = ctls[c:c + nc], codes[k:k + nk]
                c += (nc + 7) & ~7
                k += nk
                st = glslc_stage(ctl, code)
                why = check(st, ctl, code) if st else 'unknown stage'
                if why:
                    red(f'HCPS SLCT {i} variant {j}: {why}')
                out.append((st, ctl, code))
            if k != kend:
                red('HCPS: unused program code bytes')
        if c != ce or k != ke:
            red(f'HCPS SLCT {i}: unused control/code bytes')
    return out


def slct_shaders(d, base):
    """A standalone SLCT (in main): its control / code offsets are relative to itself; read through an HCPS header."""
    if base + 72 > len(d):
        return []
    c, cn, k, kn = struct.unpack_from('<4I', d, base + 40)
    size = max(c + cn, k + kn)
    if min(c, k) < 72 or size > len(d) - base:
        return []
    header = bytearray(80)
    header[:4] = b'HCPS'
    struct.pack_into('<2I', header, 8, 72, 1)
    struct.pack_into('<6I', header, 24, 80, size, 80, size, 80, size)
    return hcps_shaders(bytes(header) + d[base:base + size], 0)


def monolith_shaders(d, out):
    """Decoded entries and their XBC1 / cmp streams, recursively, whatever the extension: models and map streams embed HCPS."""
    pos, compressed = 0, []
    markers = sorted((m.start(), magic == b'xbc1', len(magic)) for magic in (b'xbc1', b'cmp\0zlib') for m in re.finditer(re.escape(magic), d))
    for off, xbc, size in markers:
        if off < pos:
            continue
        result = xbc_data(d, off) if xbc else monolith_cmp_data(d, off)
        pos = off + size if result is None else result[1]
        if result is not None:
            compressed.append((off, pos))
            monolith_shaders(result[0], out)
    ci = 0
    for m in re.finditer(b'HCPS', d):
        base = m.start()
        while ci < len(compressed) and compressed[ci][1] <= base:
            ci += 1
        if ci < len(compressed) and compressed[ci][0] <= base:
            continue   # a literal inside a compressed stream, not a decoded container
        if base + 8 <= len(d) and struct.unpack_from('<I', d, base + 4)[0] in (1000, 1010):
            out.extend(hcps_shaders(d, base))
    out.extend(glslc_shaders(d, skip=compressed))


def scan_blob(args):
    """One job: (file, [(label, offset, size)]) -> {key: (stage, control, code)}, and the damaged entries."""
    path, items = args
    found, damaged = {}, []
    with open(path, 'rb') as f:
        for label, off, size in items:
            f.seek(off)
            out = []
            try:
                monolith_shaders(f.read(size), out)
            except (Damaged, zlib.error, struct.error) as e:
                damaged.append(f'{label}: {e}')
                continue
            for st, ctl, code in out:
                found.setdefault(shader_key(ctl, code), (st, ctl, code))
    return found, damaged


def jobs_for(folder, parts=64):
    """Scan jobs for an extracted folder: a romfs/romfs.bin image (its ARH1 / ARD and every other file), or a plain romfs/ tree."""
    romfs = Path(folder) / 'romfs'
    image = romfs / 'romfs.bin'
    files = []   # (container path, inner path, offset, size)
    if image.is_file():
        files = [(str(image), e['path'], e['offset'], e['size']) for e in index_romfs(image)]
    else:
        for p in sorted(romfs.rglob('*')):
            if p.is_file():
                files.append((str(p), str(p.relative_to(romfs)), 0, p.stat().st_size))
    jobs, loose = [], []
    by_name = {inner: (c, o, n) for c, inner, o, n in files}
    for c, inner, o, n in files:
        if inner.endswith('.arh'):
            ard = by_name.get(inner[:-4] + '.ard')
            if not ard:
                red(f'{inner}: ARH1 index has no ARD data file')
            with open(c, 'rb') as f:
                f.seek(o)
                entries = arh_entries(f.read(n))
            ac, ao, an = ard
            if any(eo > an or es > an - eo for _, eo, es in entries):
                red(f'{inner}: ARD entry out of bounds')
            items = [(f'{inner[:-4]}.ard/{name}', ao + eo, es) for name, eo, es in entries]
            step = max(1, (len(items) + parts - 1) // parts)
            jobs += [(ac, items[i:i + step]) for i in range(0, len(items), step)]
        elif not inner.endswith('.ard') and not inner.startswith(('movie/', 'sound/')):
            loose.append((c, (inner, o, n)))
    groups = {}
    for c, item in loose:
        groups.setdefault(c, []).append(item)
    for c, items in groups.items():
        step = max(1, (len(items) + parts - 1) // parts)
        jobs += [(c, items[i:i + step]) for i in range(0, len(items), step)]
    return jobs


def main_shaders(main_image):
    """The standalone SLCTs in main's decompressed image (main+0x14e01c0 in 2.2.1: the boot shaders)."""
    d = Path(main_image).read_bytes()
    out = []
    for m in re.finditer(b'SLCT\x03\x10\0\0', d):
        try:
            out.extend(slct_shaders(d, m.start()))
        except Damaged:
            pass
    return {shader_key(ctl, code): (st, ctl, code) for st, ctl, code in out}


def census(folders, main_image=None, workers=None):
    """{key: (stage, control, code)} over the folders (and main), and the damaged entries skipped."""
    found, damaged = {}, []
    jobs = [j for folder in folders for j in jobs_for(folder)]
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        for part, bad in pool.map(scan_blob, jobs):
            for k, v in part.items():
                found.setdefault(k, v)
            damaged += bad
    if main_image:
        for k, v in main_shaders(main_image).items():
            found.setdefault(k, v)
    return found, damaged


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folders', type=Path, nargs='+', help='extracted folders holding romfs/ (the game, each add-on)')
    ap.add_argument('--main', type=Path, help="main's decompressed image (rewritten/<version>/main.patched.bin)")
    ap.add_argument('--keys', type=Path, nargs='*', default=[], help='translation caches: report how many of their shaders the census found')
    a = ap.parse_args()
    found, damaged = census(a.folders, a.main)
    stages = {}
    for st, _, _ in found.values():
        stages[st] = stages.get(st, 0) + 1
    print(f'{len(found)} shaders: ' + ', '.join(f'{n} {s}' for s, n in sorted(stages.items())))
    for line in damaged[:10]:
        print('damaged, skipped:', line)
    if len(damaged) > 10:
        print(f'... {len(damaged)} damaged entries in all')
    for cache in a.keys:
        met = {m.group(1) for e in os.scandir(cache) if (m := re.match(r'^([0-9a-f]{64})\.variants$', e.name))}
        print(f'{cache}: {len(met)} shaders met in play, {len(met & found.keys())} found by the census, missing {len(met - found.keys())}')


if __name__ == '__main__':
    main()

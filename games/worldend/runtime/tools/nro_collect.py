#!/usr/bin/env python3
"""Collect the NRO modules a game registers (nn::ro): every SHA-256 its NRR files list (RomFS .nrr/*.nrr), found as loose files or inside the RomFS's archives.
Usage: nro_collect.py <RomFS> <out dir> [<nrr name>]
Writes <out dir>/<name>.nro for each registered NRO found and <out dir>/nro.tsv (name, SHA-256, size, NRR, where it was found); exits 1 when a registered hash is not found.
An online SDK (ONLINE_SDKS: Epic Online Services, by its export EOS_Platform_Create) is listed with the name 'refused' and not written: the host refuses to load it.
<nrr name> picks one NRR (default: every NRR; Super Smash Bros. Ultimate 13.0.5 ships 14, one per release, cross2_130004.nrr the newest, with the 98 current hashes).
The game passes nn::ro::LoadModule the NRO's bytes; the host finds the rewritten copy by that SHA-256 (hle_ro.c), so the names here are only labels: nro<index> in hash order.
Where: a loose file whose bytes at +0x10 are NRO0 (Hades and Hades II ship their EOS SDK so); the .nro files among the current files of an archive with an index (Super Smash Bros. Ultimate's data.arc,
read by dm/gfx/tools/smash_arc.py: 98 NROs, while the NRRs of every older release also register 666 superseded copies the archive still holds, which are left out); else a scan of the
other RomFS files for stored NROs and zstd frames that inflate to one (on data.arc: all 764 registered hashes in 7 minutes)."""
import hashlib, mmap, os, struct, sys
from multiprocessing import Pool

ZMAGIC = b'\x28\xb5\x2f\xfd'


def zstd_mod():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: sys.exit(f'[RED] Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    return zstd


def nrr_hashes(romfs, only=None):
    out = {}
    for dp, _, fs in os.walk(romfs):
        for f in sorted(fs):
            if not f.endswith('.nrr') or (only and f != only): continue
            d = open(os.path.join(dp, f), 'rb').read()
            if d[:4] != b'NRR0': continue
            off, cnt = struct.unpack_from('<II', d, 0x340)
            for i in range(cnt): out.setdefault(d[off + 32 * i:off + 32 * i + 32], f)
    return out


def nro_size(h):   # an NRO header's total size, or 0 when the bytes are not one
    if len(h) < 0x40 or h[0x10:0x14] != b'NRO0': return 0
    n = struct.unpack_from('<I', h, 0x18)[0]
    return n if 0x40 < n < 1 << 30 else 0


def frame(mm, p, zstd):
    dec = zstd.ZstdDecompressor(); parts, q = [], p
    while not dec.eof and q < len(mm) and q - p < (1 << 30):
        parts.append(dec.decompress(mm[q:q + (8 << 20)])); q += 8 << 20
    return b''.join(parts)


def scan(job):
    """NROs in [a, b) of one file: stored ones (NRO0 at +0x10) and zstd frames whose first block inflates to an NRO header. Returns [(sha256, path, offset, how, bytes)] of wanted ones"""
    path, a, b, want = job
    zstd = zstd_mod(); out = []
    with open(path, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        p = mm.find(b'NRO0', a, b)
        while p != -1:
            s = p - 0x10; n = nro_size(mm[s:s + 0x40]) if s >= 0 else 0
            if n and s + n <= len(mm):
                body = mm[s:s + n]; h = hashlib.sha256(body).digest()
                if h in want: out.append((h, path, s, 'stored', body))
            p = mm.find(b'NRO0', p + 4, b)
        p = mm.find(ZMAGIC, a, b)
        while p != -1:
            try:
                head = zstd.ZstdDecompressor().decompress(mm[p:p + 0x48000], max_length=0x40)
                if nro_size(head):
                    body = frame(mm, p, zstd); h = hashlib.sha256(body).digest()
                    if h in want: out.append((h, path, p, 'zstd', body))
            except Exception:
                pass
            p = mm.find(ZMAGIC, p + 4, b)
        mm.close()
    return out


# Online SDKs the host never loads, by what the module exports, whatever the game: Epic Online Services (Hades and Hades II load rom:/nro/EOSSDK-Switch-Shipping.nro; Hades' main
# imports only EOS Auth, Connect and PlayerDataStorage, Epic-account cross-save the host cannot serve). Running it would at best fail its login, at worst wait on time-outs or contact
# Epic's servers with a console identity the host makes up; refused, the game plays offline with local saves, as before the host loaded NROs
ONLINE_SDKS = {'EOS_Platform_Create': 'Epic Online Services SDK'}


def online_sdk(body):   # the online SDK an NRO is (by its exports), or None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso_syms
    try: names = {n for n, defined, _ in nso_syms.dynsyms(body) if defined}
    except Exception: return None
    return next((what for sym, what in ONLINE_SDKS.items() if sym in names), None)


def main():
    romfs, out = sys.argv[1], sys.argv[2]
    want = nrr_hashes(romfs, sys.argv[3] if len(sys.argv) > 3 else None)
    if not want: sys.exit(f'no NRR file with hashes in {romfs}')
    os.makedirs(out, exist_ok=True)
    found, big, indexed = {}, [], 0
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gfx', 'tools'))
    import smash_arc
    for dp, _, fs in os.walk(romfs):
        for f in sorted(fs):
            p = os.path.join(dp, f); sz = os.path.getsize(p)
            if f.endswith('.nrr') or sz < 0x40: continue
            with open(p, 'rb') as fh: head = fh.read(0x40)
            if nro_size(head) == sz:
                body = open(p, 'rb').read(); h = hashlib.sha256(body).digest()
                if h in want: found[h] = (os.path.relpath(p, romfs), 'loose', body)
            elif head[:8] == smash_arc.MAGIC:
                # an archive with an index (Super Smash Bros. Ultimate's data.arc): the .nro files among its current files. Its superseded copies, which every older release's NRR still
                # registers (764 hashes in 14 NRRs, 98 current), are never loaded and are left out
                indexed += 1; nro = smash_arc.hash40('nro')
                with open(p, 'rb') as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
                    for e in smash_arc.files(p):
                        if e[4] != nro: continue
                        body = smash_arc.read(m, e); h = hashlib.sha256(body).digest()
                        if h in want: found[h] = (f'{os.path.relpath(p, romfs)}#{e[0]:010x}', 'index', body)
                        else: print(f'[RED] {os.path.relpath(p, romfs)}#{e[0]:010x}: an NRO that no NRR registers')
            else: big.append((p, sz))
    if indexed:   # the NROs a game loads are the current files of its archives: the hashes not among them belong to superseded copies
        print(f'{len(found)} NROs among the current files of {indexed} indexed archive(s); {len(want) - len(found)} registered hashes are superseded copies, left out')
        want = {h: n for h, n in want.items() if h in found}
    left = {h for h in want if h not in found}
    if left and big:
        jobs = []
        for p, sz in big:
            step = max(64 << 20, (sz + 47) // 48)
            jobs += [(p, a, min(sz, a + step + 0x40), left) for a in range(0, sz, step)]
        print(f'{len(want)} registered NROs, {len(found)} loose; scanning {len(big)} files ({sum(s for _, s in big) / 2**30:.1f} GB) for the other {len(left)}', flush=True)
        with Pool(min(10, os.cpu_count() or 4)) as pool:
            for r in pool.imap_unordered(scan, jobs):
                for h, path, off, how, body in r:
                    found.setdefault(h, (f'{os.path.relpath(path, romfs)}+{off:#x}', how, body))
    rows, i, refused = [], 0, 0
    for h in sorted(found):
        where, how, body = found[h]
        what = online_sdk(body)
        if what:   # listed, not written: the host refuses to load it (hle_ro.c)
            rows.append(f'refused\t{h.hex()}\t{len(body)}\t{want[h]}\t{how} {where} ({what})'); refused += 1; continue
        name = f'nro{i}'; i += 1
        open(os.path.join(out, name + '.nro'), 'wb').write(body)
        rows.append(f'{name}\t{h.hex()}\t{len(body)}\t{want[h]}\t{how} {where}')
    if refused: print(f'{refused} NRO(s) of an online SDK refused, not rewritten (listed as refused in nro.tsv)')
    open(os.path.join(out, 'nro.tsv'), 'w').write('# name\tsha256\tsize\tnrr\twhere\n' + '\n'.join(rows) + '\n')
    miss = [h.hex() for h in want if h not in found]
    print(f'{len(found)} of {len(want)} registered NROs found ({sum(len(found[h][2]) for h in found) / 2**20:.1f} MB) → {out}')
    if miss: print(f'[RED] {len(miss)} registered NROs not found, first {miss[0]}'); sys.exit(1)


if __name__ == '__main__': main()

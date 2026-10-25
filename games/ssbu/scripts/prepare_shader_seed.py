#!/usr/bin/env python3
"""Prepare game shaders from the selected, verified local RomFS, without a played cache.

Every shader in the RomFS is found (Shader/ packages, SDK and boot packages, and the ones effects and UI layouts carry).
The startup shaders (Shader/, SDK, boot) are translated for the default bind state; then every bind state the shared
shader list (assets/shader_list.tsv, docs/shader-list.md) names for a shader of this game is translated too, so an
iPhone or iPad, which cannot translate while playing, carries what players have met. The output contains game-derived
data and belongs in the work folder, never in the repo.

Breath of the Wild (game='botw', games/botw/scripts/prepare_shader_seed.py): the same, with its own list and container
formats (Yaz0 instead of Zstandard, no dictionaries), its shaders in Pack/ (the boot packages' uking_*.sbfsha and the
engine's .sharcb inside System/*.ssarc), Effect/, Event/ and System/ (botw.md: a scan of every file of 1.6.0 found
them nowhere else), and the control section's own size (0x7d1, at +0x7c8) hashed into the key, as the runtime does.

Super Mario Odyssey (game='smo', this folder's copy): Yaz0 like Breath of the Wild, its shaders in ShaderData/,
EffectData/, LayoutData/, SystemData/ and the game module, keyed as they are (Breath of the Wild's control rewrite does
not apply: with it 177 of the 870 keys a first session recorded matched, without it all 870, 2026-10-22). The startup
shaders are ShaderData/ and SystemData/.

Super Smash Bros. Ultimate (game='ssbu', the default here): the RomFS is one data.arc, and its shaders are BNSH files
inside the archive's Zstandard frames (effects: EFFN / VFXB; 2026-10-24, every one of 24 sampled shaders the runtime
met was found there). The archive is not parsed: every frame is found by its magic and decoded to its end, in parallel
over parts of the archive, and BNSH files outside frames are read too. Every shader is translated for the default bind
state; Smash's depth mode is near = 0 (no NEAR_IS_MINUS_W, hle_nvn.c), so the requests say depth_minus_w false. The
chain is the runtime's own (dmsh_chain.h), which the Smash translator build has its own name for.
"""
import argparse
import bisect
import concurrent.futures
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_list import ShaderList   # noqa: E402

def chain_name():
    """The translation chain the runtime names its cache by (third_party/d2r_gfx/gfx/nvn/dmsh_chain.h DMSH_CHAIN): the
    seed import rejects another chain (totk_shader_seed.h), so the name is read from there rather than kept here."""
    header = Path(__file__).resolve().parents[1]/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
    found = re.search(r'#define DMSH_CHAIN "([^"]+)"', header.read_text())
    if not found:
        raise SystemExit(f'prepare_shader_seed: no DMSH_CHAIN in {header}')
    return found.group(1)


CHAIN = chain_name()
MIN_SHADERS = 8000
ZSTD = b'\x28\xb5\x2f\xfd'
YAZ0 = b'Yaz0'
ANSWERS = dict(tex={}, img={}, attr={}, fout={})
LIST = Path(__file__).resolve().parents[1]/'assets/shader_list.tsv'
# Per game: the RomFS folders that hold shaders (None: every file), the packages whose shaders are translated for the
# default bind state (the startup ones), the fewest startup shaders a seed may have, and whether every shader found is
# translated for the default bind state too (all_default; docs/shader-variants.md: since the translator takes texture
# kinds from the instructions and vertex input types from their use, the default state serves almost every draw).
GAMES = {
    'totk': dict(folders=None, min_shaders=MIN_SHADERS, list=LIST, all_default=True),
    'botw': dict(folders=('Pack/', 'Effect/', 'Event/', 'System/'), min_shaders=2000,
                 list=Path(__file__).resolve().parents[2]/'botw/assets/shader_list.tsv', all_default=False),
    'smo': dict(folders=('ShaderData/', 'EffectData/', 'LayoutData/', 'SystemData/'), min_shaders=5000, list=LIST, all_default=True),
    # 13.0.2: 35,348 shaders in data.arc (27,585 in BNSH files, the rest in SSBH shader files), 3,210 of the 3,212 a
    # session met; the other two are in the game module without a control section beside them (2026-10-24).
    'ssbu': dict(folders=None, min_shaders=30000, list=LIST, all_default=True, arc=True, depth_minus_w=False),
}
DEFAULTS = dict(tex='2d', img={}, attr='float', fout='float')


def suffix(answers):
    return hashlib.sha256(json.dumps(answers, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:12]


SUFFIX = suffix(ANSWERS)


def deps_match(deps, answers):
    """Whether a variant translated with deps serves these answers: totk_shader_rt.m deps_match, which the runtime picks
    variants with. "*" / "*_depth": any texture kind, without / with a depth format; an unanswered vertex input is unbound."""
    for kind, default in DEFAULTS.items():
        have = answers.get(kind, {})
        for slot, want in deps.get(kind, {}).items():
            if slot not in have and kind == 'attr':
                continue
            got = have.get(slot, default)
            if want in ('*', '*_depth'):
                if not isinstance(got, str) or got.endswith('_depth') != (want == '*_depth'):
                    return False
            elif got != want:
                return False
    return True


def answers_for(deps):
    """The bind state to translate a listed row with: its deps, a "*" slot answered as the plain 2D texture it stands for."""
    answers = {**ANSWERS, **deps}
    answers['tex'] = {slot: {'*': '2d', '*_depth': '2d_depth'}.get(kind, kind) for slot, kind in answers['tex'].items()}
    return answers


def digest(data):
    return hashlib.sha256(data).hexdigest()


def shader_key_scope(keys, version, source_romfs_sha256, *, chain=CHAIN):
    """The raw shader keys belonging to this game, including shaders not translated into the seed.

    This is deliberately independent of the startup/bind-state translation plan. A valid shader may fail translation,
    or only be needed when entering a later area; it still belongs to this game and may have usable cached variants.
    It can also be used to publish metadata from a previously saved, verified inventory without translating it again.
    """
    if chain != CHAIN:
        raise ValueError('shader key scope translation chain mismatch')
    if not isinstance(version, str) or not version or version != version.strip():
        raise ValueError('invalid shader key scope game version')
    if not isinstance(source_romfs_sha256, str) or not re.fullmatch('[0-9a-f]{64}', source_romfs_sha256):
        raise ValueError('invalid shader key scope RomFS hash')
    if isinstance(keys, (str, bytes)):
        raise ValueError('shader key scope requires a collection of raw shader hashes')
    values = set()
    for key in keys:
        if not isinstance(key, str) or not re.fullmatch('[0-9a-f]{64}', key):
            raise ValueError('invalid raw shader key')
        values.add(key)
    if not values:
        raise ValueError('empty shader key scope')
    return dict(schema=1, chain=chain, version=version, source_romfs_sha256=source_romfs_sha256, keys=sorted(values))


def file_identity(path):
    """What changes whenever a file's bytes may have: device, inode, size, modification and change time (writing moves
    both times; resetting the modification time still moves the change time)."""
    s = os.stat(path)
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


# <output>.verified.json: each seed file's identity and the SHA-256 it had when it was last checked. A whole-seed reuse
# hashed all 65,436 files again (30 s), and build_app.py did the same twice more (2026-10-21); a file whose identity is
# the recorded one keeps its digest. build_app.py reads the same record (seed_digests).
def verified_digests(output):
    """name -> SHA-256 for the files of output whose identity is the one recorded with it."""
    try:
        record = json.loads(output.with_name(output.name+'.verified.json').read_text())
        if record.get('schema') != 1:
            return {}
        known = {}
        for name, entry in record['files'].items():
            try:
                if Path(name).name == name and entry[:-1] == file_identity(output/name):
                    known[name] = entry[-1]
            except OSError:
                pass
        return known
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def record_verified(output, files):
    """Record the identity of output's files with the digests they were just checked or written with."""
    record = dict(schema=1, files={name: file_identity(output/name)+[sha] for name, sha in sorted(files.items())})
    path = output.with_name(output.name+'.verified.json')
    path.with_name(path.name+'.tmp').write_text(json.dumps(record, separators=(',', ':')))
    path.with_name(path.name+'.tmp').replace(path)


def reusable_seed(output, identity, min_shaders=MIN_SHADERS):
    """Return a verified seed manifest, or None when it must be prepared again."""
    marker = output/'seed-manifest.json'
    if not marker.is_file():
        return None
    try:
        old = json.loads(marker.read_text())
        files = old.get('files', {})
        # Older seeds lack the complete raw-key inventory. Their successful translation rows are not a substitute.
        if not isinstance(files, dict) or 'shader-keys.json' not in files:
            return None
        if not all(old.get(k) == v for k, v in identity.items()) or old.get('shaders', 0) < min_shaders:
            return None
        known = verified_digests(output)
        if not all((output/name).is_file() and (known.get(name) or digest((output/name).read_bytes())) == sha
                   for name, sha in files.items()):
            return None
        record_verified(output, files)
        scope = json.loads((output/'shader-keys.json').read_text())
        if scope.get('schema') != 1 or scope.get('version') != identity['version'] or \
                scope.get('source_romfs_sha256') != identity['source_romfs_sha256']:
            return None
        if shader_key_scope(scope['keys'], scope['version'], scope['source_romfs_sha256'], chain=scope.get('chain')) != scope:
            return None
        return old
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None


# Two caches beside the seed folder (<output>.inventory, <output>.translations), so a packaging whose shared list grew
# scans nothing and translates only the new bind states. The list changes almost daily, and the whole-seed reuse above
# then read the RomFS again (49 s) and translated all 35,275 jobs again (95 s) for 447 new rows (2026-10-21).
INVENTORY_SCHEMA = 1
TRANSLATIONS_SCHEMA = 1


def translation_request(variant, stage, control, code, answers, code_path, depth_minus_w=True):
    """What the translator is asked for a job (the guest code written to code_path). A cached translation depends on this
    function's source (TranslationCache identity), not on the rest of this script: the variant name already names the
    shader's control and code and the bind state, and a change elsewhere here re-translated all 31,000 jobs (5 min).
    depth_minus_w: the game's NVN depth mode is NEAR_IS_MINUS_W (the runtime asks the same, totk_shader_rt.m), part of
    the cache identity too, since the variant name does not hold it."""
    c1, length = struct.unpack_from('<II', control, 0x700)
    code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                   depth_minus_w=depth_minus_w, **answers)
    if stage == 'cs':
        request['local'] = list(struct.unpack_from('<III', control, 0x748))
    return request


def translations_folder(beside, identity):
    """Where the translations live: beside the seed (<output>.translations), or with TOTK_TRANSLATION_CACHE set, in a
    folder there named after the identity, which packagings of other work folders (an APFS clone, another session's)
    share. The shared folder must be on the seed's volume (its files are hard links). The three most recently used
    identities are kept there."""
    shared = os.environ.get('TOTK_TRANSLATION_CACHE')
    if not shared:
        return beside, False
    root = Path(shared)
    folder = root/digest(json.dumps(identity, sort_keys=True).encode())[:16]
    folder.mkdir(parents=True, exist_ok=True)
    os.utime(folder)
    for old in sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True)[3:]:
        shutil.rmtree(old, ignore_errors=True)
    return folder, True


def cached_inventory(romfs, modules, game, folder, identity):
    """inventory(), kept in <folder>: inventory.json names the identity it was read with (the RomFS, the game modules,
    this script), each shader's stage, source and the offsets of its control and code in shaders.bin, and the SHA-256
    of shaders.bin. Any difference reads the RomFS again."""
    try:
        saved = json.loads((folder/'inventory.json').read_text())
        if saved.get('identity') == identity:
            blob = (folder/'shaders.bin').read_bytes()
            if digest(blob) == saved['shaders_sha256']:
                shaders = {key: (stage, blob[c:c+cn], blob[d:d+dn], source)
                           for key, (stage, source, c, cn, d, dn) in saved['shaders'].items()}
                print(f'reusing the shader inventory of this RomFS ({len(shaders)} shaders)', flush=True)
                return None, shaders, set(saved['startup']), saved['archives']
    except (OSError, ValueError, KeyError, TypeError):
        pass
    index, shaders, startup, archives = inventory(romfs, modules, game)
    parts, table, at = [], {}, 0
    for key, (stage, control, code, source) in sorted(shaders.items()):
        table[key] = [stage, source, at, len(control), at+len(control), len(code)]
        parts += [bytes(control), bytes(code)]
        at += len(control)+len(code)
    blob = b''.join(parts)
    staging = folder.with_name(folder.name+'.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    (staging/'shaders.bin').write_bytes(blob)
    (staging/'inventory.json').write_text(json.dumps(dict(identity=identity, shaders_sha256=digest(blob), shaders=table,
                                                          startup=sorted(startup), archives=archives), separators=(',', ':')))
    shutil.rmtree(folder, ignore_errors=True)
    staging.rename(folder)
    return index, shaders, startup, archives


class TranslationCache:
    """Each translation job's result by variant name (<key>-<suffix of its bind state>) in <folder>: <variant>_b.metal and
    _b.json as the service wrote them (hard links to the seed's own files, which nothing writes again), or
    <variant>.failed with the service's error. cache.json names what a result depends on besides the job (the chain, the
    translator, SPIRV-Cross, this script, which builds the requests); a different identity empties the folder. Entries
    that no job of the last preparation asked for are removed after it. digests.json keeps the SHA-256 of each file, so
    the seed manifest does not read tens of thousands of unchanged files again."""

    def __init__(self, folder, identity, shared=False):
        self.folder, self.shared = folder, shared   # shared (translations_folder): other work folders' entries stay
        try:
            same = json.loads((folder/'cache.json').read_text()) == identity
        except (OSError, ValueError):
            same = False
        if not same:
            shutil.rmtree(folder, ignore_errors=True)
            folder.mkdir(parents=True)
            (folder/'cache.json').write_text(json.dumps(identity))
        try:
            self.digests = json.loads((folder/'digests.json').read_text()) if same else {}
        except (OSError, ValueError):
            self.digests = {}

    def take(self, variant, staging):
        """The service's reply for a job translated before (its files linked into staging), or None. A staging file that
        is already the cached one (the previous seed's, kept by prepare) is left as it is."""
        failed = self.folder/(variant+'.failed')
        if failed.is_file():
            return dict(ok=False, error=json.loads(failed.read_text()).get('error'))
        files = [variant+'_b.metal', variant+'_b.json']
        try:
            cached = [os.stat(self.folder/name).st_ino for name in files]
        except FileNotFoundError:
            return None
        for name, inode in zip(files, cached):
            try:
                if os.stat(staging/name).st_ino == inode:
                    continue
                (staging/name).unlink()
            except FileNotFoundError:
                pass
            os.link(self.folder/name, staging/name)
        return dict(ok=True)

    def put(self, variant, reply, staging):
        if not reply.get('ok'):
            (self.folder/(variant+'.failed')).write_text(json.dumps(dict(error=reply.get('error'))))
            return
        for name in (variant+'_b.metal', variant+'_b.json'):
            (self.folder/name).unlink(missing_ok=True)
            try:
                os.link(staging/name, self.folder/name)
            except FileExistsError:   # another packaging sharing the folder put the same translation
                pass
            self.digests[name] = digest((staging/name).read_bytes())

    def digest(self, name, path):
        """The SHA-256 of a cached file that path is a link to (take / put made it one)."""
        if name not in self.digests:
            self.digests[name] = digest(path.read_bytes())
        return self.digests[name]

    def prune(self, variants):
        for p in ([] if self.shared else self.folder.iterdir()):
            for end in ('_b.metal', '_b.json', '.failed'):
                if p.name.endswith(end) and p.name[:-len(end)] not in variants:
                    p.unlink()
                    self.digests.pop(p.name, None)
        (self.folder/'digests.json').write_text(json.dumps(self.digests, separators=(',', ':')))


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset+size]


def control_key_len(control):
    """How many bytes of a control section the runtime hashes into the shader key (nvn_mtl.m prog_get ctl_key_len): the
    section's own size, the word at +0x20 (0x880 in Tears of the Kingdom's archives and agl packages; 0x7d1 and 0x848 in
    Breath of the Wild, where the game keeps each section in its own allocation and what follows it is not the
    shader's), never more than 0x880."""
    size = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return size if 0x7cc <= size < 0x880 else 0x880


def bound_control(control, game='totk', embedded=True):
    """The control section as the game hands it to NVN. Breath of the Wild rewrites the 0x848-byte sections of the agl
    programs it loads from embedded BNSH files before binding: a flag at +0x14, the size copies at +0x6f0 and +0x7c8,
    three words at +0x788..+0x794 and the 24 bytes at +0x7d0 (995 of 997 shaders of a recorded session, the other two
    a code shared with another shader); the agl packages' own records (embedded=False) are bound as they are (158 of
    160; botw.md). The key must hash what NVN sees."""
    if game != 'botw' or not embedded or len(control) != 0x848:
        return control
    c = bytearray(control)
    c[0x14:0x16] = bytes(2); c[0x6f0:0x6f4] = bytes(4); c[0x7c8:0x7cc] = bytes(4); c[0x7d0:0x7e8] = bytes(0x18)
    c[0x788] = 0; c[0x790] = 0x1c; c[0x794] = 0
    return bytes(c)


def yaz0(data):
    """Yaz0 (Breath of the Wild's compression): a code byte, then 8 items, a literal byte or a back reference of
    (distance, length)."""
    if data[:4] != YAZ0:
        raise ValueError('expected Yaz0')
    size = struct.unpack_from('>I', data, 4)[0]
    if size > 512 << 20:
        raise ValueError('unbounded Yaz0 file')
    out, o, i, n = bytearray(size), 0, 16, len(data)
    try:
        while o < size:
            code = data[i]; i += 1
            for bit in (0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1):
                if o >= size:
                    break
                if code & bit:
                    out[o] = data[i]; o += 1; i += 1
                    continue
                first = data[i]; dist = ((first & 0xF) << 8 | data[i+1]) + 1; length = first >> 4; i += 2
                if length:
                    length += 2
                else:
                    length = data[i] + 0x12; i += 1
                length = min(length, size - o)
                src = o - dist
                if src < 0:
                    raise ValueError('Yaz0 back reference before the start')
                if dist >= length:
                    out[o:o+length] = out[src:src+length]
                else:
                    for k in range(length):
                        out[o+k] = out[src+k]
                o += length
    except IndexError:
        raise ValueError('truncated Yaz0 file') from None
    return bytes(out)


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian+'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian+'H', data, header+6)[0]
    names = header+12+count*16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian+'4I', checked(data, header+12+i*16, 16))
        off = names+(attribute & 0xffffff)*4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start+lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/'):   # Breath of the Wild's gamedata.ssarc names its members "/<name>.bgdata"
            name = name[1:]
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start+lo, hi-lo)
    return result


def bnsh_shaders(data, game='totk'):
    """BNSH absolute offsets are relative to each embedded BNSH, not BFSHA."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count*64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations+i*64+16)[0]
        checked(data, program, 56)
        for stage, relative in [('vs', 8), ('ps', 40), ('cs', 48)]:
            offset = struct.unpack_from('<Q', data, program+relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset+8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < 0x7cc:
                # The SDK utility BNSH uses an older, shorter control layout.
                # Do not invent bytes to make it look like the game's NVN ABI.
                continue
            key_len = control_key_len(control)   # 0x880 (some sections are 0x88c), Breath of the Wild's 0x7d1
            if len(control) < key_len:
                continue
            control = bound_control(control[:key_len], game)   # what NVN and the runtime's key read
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            code = code[:length]
            yield digest(control+code), stage, control, code


def sharcb_shaders(data, game='totk'):
    """agl shader packages (.sharcb, magic BAHS): the records between the header's start and the code pool are
    <u64 code offset in the pool, u32 control size (0x880 or 0x88c; 0x7d1 in Breath of the Wild), i32 offset of the
    control from the record>; a record may reuse an earlier record's control. Control (the bytes control_key_len names)
    and code hash to the runtime's key."""
    if len(data) < 32 or data[:4] != b'BAHS':
        raise ValueError('expected an agl shader package')
    start, pool_size, pool = struct.unpack_from('<3I', data, 20)
    if not 32 <= start <= pool or pool + pool_size > len(data):
        raise ValueError('agl shader package layout outside the file')
    seen = set()
    for size in (b'\x80\x08\x00\x00', b'\x8c\x08\x00\x00', b'\xd1\x07\x00\x00', b'\x48\x08\x00\x00'):   # 0x880 / 0x88c; Breath of the Wild's 0x7d1 / 0x848
        at = start
        while (at := data.find(size, at, pool)) >= 0:
            record, at = at - 8, at + 4
            if record < start or record % 4:
                continue
            code_off = struct.unpack_from('<Q', data, record)[0]
            control_at = record + struct.unpack_from('<i', data, record+12)[0]
            key_len = control_key_len(data[control_at:control_at+0x880]) if control_at >= start else 0
            if not key_len or not start <= control_at <= pool - key_len or code_off >= pool_size:
                continue
            control = bound_control(data[control_at:control_at+key_len], game, embedded=False)
            c1, length = struct.unpack_from('<II', control, 0x700)
            stage = {0: 'vs', 1: 'ps', 5: 'cs'}.get(struct.unpack_from('<I', control, 0x714)[0])
            if not stage or not 0 < length <= pool_size - code_off or c1 > length:
                continue
            code = data[pool+code_off:pool+code_off+length]
            key = digest(control+code)
            if key not in seen:
                seen.add(key)
                yield key, stage, control, code


def startup_package(name, game='totk'):
    """The packages the startup shaders come from; effects and UI layouts carry more. Breath of the Wild: the boot and
    title packages (the materials, terrain, trees, grass and the engine's own shaders) and the System/ ones."""
    if game == 'botw':
        return name in ('Pack/Bootup.pack', 'Pack/Bootup_Graphics.pack', 'Pack/TitleBG.pack') or name.startswith('System/')
    if game == 'smo':
        return name.startswith(('ShaderData/', 'SystemData/'))
    return (name == 'Lib/NintendoSDK/TextureShader.bnsh' or name == 'Pack/Bootup.Nin_NX_NVN.pack.zs' or
            name.startswith('Shader/') and name.endswith('.zs'))


def decoder(romfs, read, game='totk'):
    """decode(raw) -> the file's content: Zstandard with the game's dictionaries (Tears of the Kingdom), Yaz0 (Breath of
    the Wild, Super Mario Odyssey); other data unchanged."""
    if game in ('botw', 'smo'):
        return lambda raw: yaz0(raw) if raw[:4] == YAZ0 else raw
    import compression.zstd as zstd
    dictionaries = {}
    for raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
        d = zstd.ZstdDict(raw)
        dictionaries[d.dict_id] = d
    def decode(raw):
        if raw[:4] != ZSTD:
            return raw
        info = zstd.get_frame_info(raw)
        if info.decompressed_size is None or info.decompressed_size > 512 << 20:
            raise ValueError('unbounded shader package')
        return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)
    return decode


ARC_WINDOW = 64 << 20
ARC_FRAME_MAX = 2 << 30   # no frame of Smash 13.0.2 comes near; a larger one stops the walk instead of being skipped


SHDR_SIZE_WORD = re.compile(rb'[\xcc-\xff]\x07\x00\x00|[\x00-\x80]\x08\x00\x00', re.S)   # 0x7cc..0x880, little-endian


def shdr_shaders(data, at):
    """Smash's own shader files (nushdb: SSBH 'HBSS' with 'SHDR' at +0x10; `at` is that tag): {u64 offset relative to
    itself, u64 count} at +8 locates 0x38-byte entries {name, type, unknown, buffer {offset relative to itself, size}};
    each buffer is an NVN shader binary (magic 0x19866891) whose control sections carry their own size in the word at
    +0x20 (0x850 in 13.0.2) and are followed by their code. A control is taken where that word, the stage at +0x714
    (0 / 1 / 5) and the code area at +0x700 / +0x704 agree and the code fits in the buffer; the runtime's key of a
    shader met in play matched this layout (2026-10-24). -> ([(key, stage, control, code)], refused buffers)"""
    found, refused = [], 0
    rel, count = struct.unpack_from('<QQ', data, at+8)
    table = at+8+rel
    if count > 1 << 16 or table+count*0x38 > len(data):
        return found, 1
    for i in range(count):
        e = table+i*0x38
        off, size = struct.unpack_from('<QQ', data, e+0x10)
        start = e+0x10+off
        if not size or start+size > len(data):
            refused += 1
            continue
        blob, rows = memoryview(data)[start:start+size], 0
        for m in SHDR_SIZE_WORD.finditer(blob):
            x = m.start()-0x20
            if x < 0 or x % 4:
                continue
            ctl_size = struct.unpack_from('<I', blob, x+0x20)[0]
            if x+ctl_size > size or ctl_size < 0x718:
                continue
            c1, length = struct.unpack_from('<II', blob, x+0x700)
            stage = {0: 'vs', 1: 'ps', 5: 'cs'}.get(struct.unpack_from('<I', blob, x+0x714)[0])
            if not stage or not length or c1 > length or x+ctl_size+length > size or (stage != 'cs' and c1 < 0x30):
                continue
            control, code = bytes(blob[x:x+ctl_size]), bytes(blob[x+ctl_size:x+ctl_size+length])
            found.append((digest(control[:control_key_len(control)]+code), stage, control, code))
            rows += 1
        refused += not rows
    return found, refused


def arc_shaders(data, source, game):
    """The BNSH files and SSBH shader files in data -> [(key, stage, control, code, source)]; what a parser refuses is
    left out (counted)."""
    found, refused, pos = [], 0, data.find(b'BNSH\0\0\0\0')
    while pos >= 0:
        size = struct.unpack_from('<I', data, pos+28)[0] if pos+32 <= len(data) else 0
        try:
            found += [(*row, source) for row in bnsh_shaders(checked(data, pos, size), game)]
        except (ValueError, struct.error):
            refused += 1
        pos = data.find(b'BNSH\0\0\0\0', pos+8)
    pos = data.find(b'RDHS')
    while pos >= 0:
        if pos >= 0x10 and data[pos-0x10:pos-0xc] == b'HBSS' and pos+0x18 <= len(data):
            rows, bad = shdr_shaders(data, pos)
            found += [(*row, source) for row in rows]
            refused += bad
        pos = data.find(b'RDHS', pos+4)
    return found, refused


def arc_part(job):
    """One part of data.arc, [start, end) -> the shaders of every Zstandard frame that starts there, the frames' spans, the
    raw BNSH positions there and counters. A frame is found by its magic and accepted only when it decodes to its end;
    the walk then continues after it, so nothing inside a frame is taken for a frame."""
    import compression.zstd as zstd
    image, base, size, start, end, game = job
    shaders, spans, raw, frames, out_bytes, refused = [], [], [], 0, 0, 0
    with open(image, 'rb') as f:
        def read(at, n):
            f.seek(base+at)
            return f.read(max(0, min(n, size-at)))
        for at in range(start, end, ARC_WINDOW):   # BNSH files stored as they are
            chunk = read(at, ARC_WINDOW+8)
            pos = chunk.find(b'BNSH\0\0\0\0')
            while 0 <= pos < ARC_WINDOW and at+pos < end:
                raw.append(at+pos)
                pos = chunk.find(b'BNSH\0\0\0\0', pos+1)
        wstart, window = start, read(start, ARC_WINDOW)
        pos = start
        while pos < end:
            if pos-wstart > len(window)-4:
                if len(window) < ARC_WINDOW:
                    break
                wstart, window = pos, read(pos, ARC_WINDOW)
            p = window.find(ZSTD, pos-wstart)
            if p < 0:
                if len(window) < ARC_WINDOW:
                    break
                pos = wstart+len(window)-3
                continue
            at = wstart+p
            if at >= end:
                break
            view, d, data, fed, ok = memoryview(window)[p:], zstd.ZstdDecompressor(), bytearray(), 0, True
            try:
                while not d.eof:
                    piece = view[fed:fed+(1 << 20)] if fed < len(view) else read(at+fed, 1 << 20)
                    if not len(piece):
                        ok = False
                        break
                    fed += len(piece)
                    data += d.decompress(piece)
                    if len(data) > ARC_FRAME_MAX:
                        raise ValueError(f'data.arc frame at {at:#x} decodes to more than {ARC_FRAME_MAX >> 30} GiB')
            except zstd.ZstdError:
                ok = False
            if not ok:
                pos = at+1
                continue
            length = fed-len(d.unused_data)
            frames += 1; out_bytes += len(data); spans.append((at, length))
            found, bad = arc_shaders(bytes(data), f'data.arc+{at:#x}', game)
            refused += bad
            if found:
                shaders.append((at, length, digest(read(at, length)), found))
            pos = at+length
    return shaders, spans, raw, dict(frames=frames, out_bytes=out_bytes, refused=refused)


def arc_inventory(romfs, entries, game):
    """Every shader in data.arc (Super Smash Bros. Ultimate) -> shaders {key: (stage, control, code, source)}, archives
    [{path, sha256}] (the frames, or raw BNSH spans, that hold them)."""
    arc = entries['data.arc']
    image, size = str(romfs/'romfs.bin'), arc['size']
    parts = max(1, min(8, os.cpu_count() or 4))
    edges = [size*i//parts for i in range(parts+1)]
    started = time.monotonic()
    with concurrent.futures.ProcessPoolExecutor(parts) as pool:
        results = list(pool.map(arc_part, [(image, arc['offset'], size, edges[i], edges[i+1], game) for i in range(parts)]))
    shaders, archives, spans, raw, totals = {}, [], [], [], dict(frames=0, out_bytes=0, refused=0)
    for found, part_spans, part_raw, counts in results:
        for at, length, sha, rows in found:
            archives.append(dict(path=f'data.arc+{at:#x}', size=length, sha256=sha))
            for key, stage, control, code, source in rows:
                shaders.setdefault(key, (stage, control, code, source))
        spans += part_spans; raw += part_raw
        for k in totals:
            totals[k] += counts[k]
    spans.sort()
    starts = [s for s, _ in spans]
    def inside(p):   # compressed bytes can spell the magic: only BNSH outside every frame are read as they are
        i = bisect.bisect_right(starts, p)-1
        return i >= 0 and p < spans[i][0]+spans[i][1]
    outside = [p for p in raw if not inside(p)]
    with open(image, 'rb') as f:
        for pos in outside:   # a BNSH between frames: its size is in its header
            f.seek(arc['offset']+pos+28)
            head = f.read(4)
            length = struct.unpack('<I', head)[0] if len(head) == 4 else 0
            if not 32 <= length <= min(size-pos, ARC_FRAME_MAX):
                totals['refused'] += 1
                continue
            f.seek(arc['offset']+pos)
            data = f.read(length)
            rows, bad = arc_shaders(data, f'data.arc@{pos:#x}', game)
            totals['refused'] += bad
            if rows:
                archives.append(dict(path=f'data.arc@{pos:#x}', size=length, sha256=digest(data)))
            for key, stage, control, code, source in rows:
                shaders.setdefault(key, (stage, control, code, source))
    print(f'data.arc: {totals["frames"]} frames ({totals["out_bytes"] >> 20} MiB decoded), {len(raw)} BNSH magics in the '
          f'raw archive ({len(outside)} of them outside frames), {len(archives)} frames or files holding shaders, '
          f'{totals["refused"]} BNSH refused by the parser; {len(shaders)} unique shaders; '
          f'{time.monotonic()-started:.0f} s', flush=True)
    return shaders, archives


def inventory(romfs, modules=None, game='totk'):
    """Every shader in the RomFS and the game modules -> (stage, control, code, source path), the keys translated for
    the default bind state (startup packages, agl packages, the modules), the archives. modules: rewritten/<version>."""
    index = json.loads((romfs/'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    folders = GAMES[game]['folders']
    shaders, startup, archives = {}, set(), []
    with (romfs/'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise ValueError('short RomFS shader read')
            return raw
        arc = GAMES[game].get('arc')
        decode = (lambda raw: raw) if arc else decoder(romfs, read, game)
        def visit(name, data, keys, depth=0):
            if data[:4] == b'BAHS':   # agl: the engine's own shaders (title screen, post effects, lighting)
                for key, stage, control, code in sharcb_shaders(data, game):
                    shaders.setdefault(key, (stage, control, code, name))
                    keys.add(key); startup.add(key)
                # Breath of the Wild's agl packages keep most of their programs as embedded BNSH files: found below.
            if data[:4] == b'SARC':
                if depth > 4:
                    return
                for member, raw in sarc(data).items():
                    visit(name+'/'+member, decode(raw), keys, depth+1)
                return
            # BFSHA and BNSH files, and effect / layout resources, hold complete, independently addressed BNSH files.
            pos = 0
            while True:
                pos = data.find(b'BNSH\0\0\0\0', pos)
                if pos < 0:
                    break
                size = struct.unpack_from('<I', checked(data, pos+28, 4))[0]
                for key, stage, control, code in bnsh_shaders(checked(data, pos, size), game):
                    shaders.setdefault(key, (stage, control, code, name))
                    keys.add(key)
                pos += max(size, 8)
        if arc:   # one data.arc: every shader in it is a startup shader (all_default)
            found, archives = arc_inventory(romfs, entries, game)
            shaders.update(found); startup |= set(found)
        for name in sorted(entries) if not arc else []:
            if folders and not name.startswith(folders):
                continue
            raw = read(name)
            # Only containers can hold shaders: compressed files, packages and shader archives.
            if raw[:4] not in (ZSTD, YAZ0, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
                continue
            data = decode(raw)
            keys = set()
            visit(name, data, keys)
            if startup_package(name, game):
                startup |= keys
            if keys or startup_package(name, game):
                archives.append(dict(path=name, sha256=digest(raw)))
        for module in sorted(Path(modules).glob('*.patched.bin')) if modules else []:
            keys = set()   # the game module embeds a few BNSH files
            visit('module:'+module.name[:-len('.patched.bin')], module.read_bytes(), keys)
            startup |= keys
        print(f'discovered {len(shaders)} unique shaders, {len(startup)} of them translated for the default bind state',
              flush=True)
    if not startup:
        raise ValueError('no startup shaders discovered')
    return index, shaders, startup, archives


def located(locators, read, game='totk'):
    """Shaders the shared list locates outside BNSH files (agl .sharcb, the game module), read where it says and kept
    only when control and code hash to the listed key (another game version's bytes never are). read(source) -> bytes."""
    found = {}
    for key, source, control_at, code_at, length in sorted(locators):
        data = read(source)
        if data is None or not 0 < length <= 16 << 20:
            continue
        control, code = data[control_at:control_at+0x880], data[code_at:code_at+length]
        key_len = control_key_len(control) if len(control) >= 0x7cc else 0
        control = control[:key_len]
        if key_len and digest(control+code) != key:   # a program the game rewrites (an embedded BNSH one)?
            control = bound_control(control, game)
        if not key_len or len(control) != key_len or len(code) != length or digest(control+code) != key:
            continue
        stage = {0: 'vs', 1: 'ps', 5: 'cs'}.get(struct.unpack_from('<I', control, 0x714)[0])
        c1, size = struct.unpack_from('<II', control, 0x700)
        if stage and size == length and 0 <= c1 <= length:
            found[key] = (stage, control, code, source)
    return found


def source_reader(romfs, modules, game='totk'):
    """read(source) for locator rows: a RomFS path, each '/' after a package entering it (.zs / Yaz0 decompressed), or
    module:<name>, a game module's image (rewritten/<version>/<name>.patched.bin). None when it is not there."""
    index = json.loads((romfs/'index.json').read_text())
    entries = {r['path']: r for r in index['files']}
    stream = (romfs/'romfs.bin').open('rb')
    def raw(name):
        row = entries[name]; stream.seek(row['offset']); return stream.read(row['size'])
    decode = (lambda data: data) if GAMES[game].get('arc') else decoder(romfs, raw, game)   # data.arc: no packages to enter
    cache = {}
    def read(source):
        if source in cache:
            return cache[source]
        data = None
        if source.startswith('module:'):
            path = Path(modules)/f'{source[7:]}.patched.bin' if modules else None
            data = path.read_bytes() if path and path.is_file() else None
        else:
            parts = source.split('/')
            for cut in range(len(parts), 0, -1):
                if '/'.join(parts[:cut]) in entries:
                    data = decode(raw('/'.join(parts[:cut])))
                    for member in parts[cut:]:
                        members = sarc(data) if data[:4] == b'SARC' else {}
                        if member not in members:
                            data = None; break
                        data = decode(members[member])
                    break
        cache[source] = data
        return data
    return read



def builtin_variants(seed, key):
    """The variants a seed made from the game code alone for a shader: its default bind state, and with compare slots
    (shadow maps) the one with them bound to depth textures; [] when the seed has no default variant of it. A seed
    prepared with the shared list also holds listed variants of the shader; those are not built in."""
    index = Path(seed)/f'{key}.variants'
    try:
        rows = {r['variant']: r for r in json.loads(index.read_text()) if isinstance(r, dict) and 'variant' in r}
    except (OSError, ValueError):
        return []
    default = rows.get(key+'-'+SUFFIX)
    if not default:
        return []
    try:
        refl = json.loads((Path(seed)/f'{key}-{SUFFIX}_b.json').read_text())
    except (OSError, ValueError):
        refl = {}
    found = [default]
    if refl.get('compare_slots'):
        depth = answers_for({'tex': {str(s): '*_depth' for s in refl['compare_slots']}})
        if rows.get(key+'-'+suffix(depth)):
            found.append(rows[key+'-'+suffix(depth)])
    return found


def builtin_serves(seed, key, deps):
    """Whether the seed's built-in variants serve a shared-list row (key, deps), as the runtime would pick them: such a
    row is a default variant, which every build makes by itself (user 2026-10-22: the list keeps non-default ones)."""
    answers = answers_for(json.loads(deps) if isinstance(deps, str) else deps)
    return any(deps_match(v.get('deps', {}), answers) for v in builtin_variants(seed, key))

def plan(shaders, startup, listed, all_default=False):
    """Translation jobs (key, answers, variant, why): the default bind state for every startup shader (every shader found
    with all_default), then each bind state the shared list names for a shader found in this game ('listed'). A listed
    state a variant made before already serves is not translated (prepare), and one whose translation gives the deps of
    an earlier variant is dropped, since only the translator knows a shader's deps. A default translation that
    compare-samples textures (shadow maps) adds a job with those slots bound to depth textures ('compare', prepare)."""
    jobs = [(key, ANSWERS, key+'-'+SUFFIX, 'default') for key in sorted(shaders if all_default else startup)]
    for key, deps in sorted(listed):
        if key in shaders:
            answers = answers_for(json.loads(deps))
            jobs.append((key, answers, key+'-'+suffix(answers), 'listed'))
    return jobs


def write_aliases(path, aliases):
    """<seed>.aliases.tsv: each listed variant the seed does not carry, because a variant it does carry serves the same
    bind state, and that variant. The packaging renames the warm-up rows through it (shader_list.py rename), since
    rows recorded on a player's Mac name the variant translated there."""
    path.with_name(path.name+'.tmp').write_text(''.join(f'{old}\t{new}\n' for old, new in sorted(aliases.items())))
    path.with_name(path.name+'.tmp').replace(path)


def prepare(romfs, output, translator, spirv_cross, version, shader_list=None, modules=None, game='totk'):
    shader_list = shader_list or GAMES[game]['list']
    min_shaders = GAMES[game]['min_shaders']
    shared = ShaderList.load(shader_list)
    listed = shared.variants
    identity = dict(schema=1, chain=CHAIN, version=version, game=game,
                    source_romfs_sha256=json.loads((romfs/'index.json').read_text())['romfs_sha256'],
                    translator_sha256=digest(translator.read_bytes()),
                    spirv_cross_sha256=digest(spirv_cross.read_bytes()),
                    preparation_sha256=digest(Path(__file__).read_bytes()),
                    shader_list_sha256=digest(Path(shader_list).read_bytes()) if Path(shader_list).is_file() else None)
    old = reusable_seed(output, identity, min_shaders)
    if old is not None:
        print(f'reusing {old["shaders"]} verified prepared startup shaders', flush=True)
        return old
    # The previous seed becomes the staging folder: most of its files are this run's too (links into the translation
    # cache), so they stay where they are instead of 65,000 deletions and links (53 s on the external volume, 2026-10-21).
    # Without its manifest it is not a seed until this run writes one; what this run does not want is removed at the end.
    staging = output.with_name(output.name+'.partial')
    if output.is_dir():
        if staging.exists():
            shutil.rmtree(staging)
        output.rename(staging)
    staging.mkdir(parents=True, exist_ok=True)
    (staging/'seed-manifest.json').unlink(missing_ok=True)
    scanned =dict(schema=INVENTORY_SCHEMA, game=game, source_romfs_sha256=identity['source_romfs_sha256'],
                   modules={p.name: digest(p.read_bytes()) for p in sorted(Path(modules).glob('*.patched.bin'))} if modules else {},
                   preparation_sha256=identity['preparation_sha256'])
    _, shaders, startup, archives = cached_inventory(romfs, modules, game, output.with_name(output.name+'.inventory'), scanned)
    depth_minus_w = GAMES[game].get('depth_minus_w', True)
    translations = dict(schema=TRANSLATIONS_SCHEMA, chain=CHAIN, translator_sha256=identity['translator_sha256'],
                        spirv_cross_sha256=identity['spirv_cross_sha256'],
                        request_sha256=digest(inspect.getsource(translation_request).encode()), depth_minus_w=depth_minus_w)
    folder, common = translations_folder(output.with_name(output.name+'.translations'), translations)
    cache = TranslationCache(folder, translations, shared=common)
    extra = located({where for where in shared.locators if where[0] not in shaders}, source_reader(romfs, modules, game), game)
    shaders.update(extra)
    print(f'{len(extra)} more shaders read where the shared list locates them (agl .sharcb, game module)', flush=True)
    jobs = plan(shaders, startup, listed, GAMES[game]['all_default'])
    aliases = {}   # listed variant -> the variant serving its bind state (write_aliases)
    # One translator service per core. A shader's jobs (its default bind state, then its listed ones) run in order on
    # one service, because whether a listed job is translated and kept depends on that shader's earlier results; the
    # files each job writes are named after its own shader. The results are then taken in the order of `jobs`, so the
    # output is the same as translating everything one at a time.
    by_key = {}
    for index, job in enumerate(jobs):
        by_key.setdefault(job[0], []).append(index)
    outcome = [None]*len(jobs)   # ('ok', variant, rows) / ('failed', record) / None (skipped or a duplicate of deps)
    derived = {}   # key -> results of the jobs a translation added (its compare-sampled slots bound to depth textures)
    produced = {}   # key -> {canonical deps: variant}; a shader's .variants rows
    lock = threading.Lock()
    pending = iter(by_key.items())
    done = [0, 0, 0]   # completed, failed, of them taken from the translation cache
    stop = threading.Event()   # a service failed: the others stop after their current shader
    started = time.monotonic()

    def worker():
        process = None   # started at this worker's first job the translation cache does not hold
        try:
            while not stop.is_set():
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, indices = item
                mine = {}
                queue = [(index, *jobs[index][1:]) for index in indices]
                while queue:
                    index, answers, variant, why = queue.pop(0)
                    stage, control, code, source = shaders[key]
                    if why != 'default':   # a variant made already serves this bind state, as the runtime would pick it
                        serving = next((v for d, v in mine.items() if deps_match(json.loads(d), answers)), None)
                        if serving:
                            with lock:
                                aliases[variant] = serving
                            continue
                    reply = cache.take(variant, staging)
                    reused = reply is not None
                    if not reused:
                        if process is None:
                            process = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross),
                                                        '--out', str(staging), '--chain', CHAIN],
                                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                        code_path = staging/(variant+'.code')
                        request = translation_request(variant, stage, control, code, answers, code_path, depth_minus_w)
                        process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
                        response = process.stdout.readline()
                        if not response:
                            raise ValueError('shader preparation service exited unexpectedly')
                        reply = json.loads(response)
                        code_path.unlink()
                        cache.put(variant, reply, staging)
                    if reply.get('ok'):
                        refl = json.loads((staging/(variant+'_b.json')).read_text())
                        if refl['chain'] != CHAIN or refl['stage'] != stage:
                            raise ValueError('prepared shader identity mismatch')
                        deps = json.dumps(refl['deps'], sort_keys=True, separators=(',', ':'))
                        if deps in mine:   # same deps as a variant already made: keep one
                            for name in (variant+'_b.metal', variant+'_b.json'):
                                (staging/name).unlink(missing_ok=True)
                            with lock:
                                aliases[variant] = mine[deps]
                            continue
                        mine[deps] = variant
                        rows =[f'S\t{variant}_b\t'+str({'vs': 0, 'ps': 1, 'cs': 5}[stage])]
                        if stage == 'cs':
                            rows.append(f'C\t{variant}_b')
                        result = ('ok', variant, rows)
                        if why == 'default' and refl.get('compare_slots'):   # its shadow maps bound too: next, before listed rows
                            depth = answers_for({'tex': {str(s): '*_depth' for s in refl['compare_slots']}})
                            queue.insert(0, (None, depth, key+'-'+suffix(depth), 'compare'))
                    else:
                        result = ('failed', dict(key=key, variant=variant, why=why, stage=stage, source=source,
                                                 error=reply.get('error')))
                    if index is None:
                        with lock:
                            derived.setdefault(key, []).append(result)
                    else:
                        outcome[index] = result
                    with lock:
                        done[0 if reply.get('ok') else 1] += 1
                        done[2] += reused
                        if (done[0]+done[1]) % 100 == 0:
                            print(f'prepared {done[0]}/{len(jobs)}, skipped {done[1]}, {done[2]} from the translation cache; '
                                  f'{time.monotonic()-started:.1f}s', flush=True)
                if mine:   # the shader's index, once all its jobs are done
                    (staging/(key+'.variants')).write_text(json.dumps([dict(variant=v, deps=json.loads(d)) for d, v in
                                                                       mine.items()], separators=(',', ':'))+'\n')
                    with lock:
                        produced[key] = mine
        except BaseException:
            stop.set()
            raise
        finally:
            if process is not None:
                process.stdin.close(); process.wait()

    # At most 8 services, as compile_seed.py compiles: each translator holds its own working memory.
    workers = max(1, min(8, os.cpu_count() or 4, len(by_key)))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for future in [pool.submit(worker) for _ in range(workers)]:
            future.result()
    failed, rows, completed = [], [], []
    for result in outcome+[result for key in sorted(derived) for result in derived[key]]:
        if result and result[0] == 'ok':
            completed.append(result[1]); rows.extend(result[2])
        elif result:
            failed.append(result[1])
    ready = sum(1 for key in startup if key in produced)
    if ready < min_shaders:
        raise ValueError(f'only {ready} shaders prepared; at least {min_shaders} are required for first-launch warm-up')
    # Only ship completed runtime inputs. Failed translations can leave MSL
    # behind before publishing reflection; those must not become cached shaders.
    wanted = set()
    for variant in completed:
        wanted.update([variant+'_b.metal', variant+'_b.json', variant.split('-')[0]+'.variants'])
    for p in staging.iterdir():
        if p.name not in wanted:
            p.unlink(missing_ok=True)
    scope = shader_key_scope(shaders, version, identity['source_romfs_sha256'])
    (staging/'shader-keys.json').write_text(json.dumps(scope, separators=(',', ':'))+'\n')
    (staging/'pso_manifest.tsv').write_text('# Prepared from this app\'s local game data; '+CHAIN+'\n'+'\n'.join(rows)+'\n')
    # On filesystems without native extended-attribute support (exFAT, FAT32), macOS can
    # transparently create "._name" AppleDouble sidecar files for anything written here,
    # including pso_manifest.tsv itself. Exclude them so the manifest's own file list never
    # ships a name the runtime's strict filename check (totk_shader_seed.h) would reject.
    wanted_final = wanted | {'pso_manifest.tsv', 'shader-keys.json'}
    # Each completed variant's two files are links to the translation cache, whose digests it keeps; the rest are read.
    linked = {variant+end for variant in completed for end in ('_b.metal', '_b.json')}
    report = dict(**identity, shaders=len(completed), discovered=len(shaders), located=len(extra), startup_shaders=ready,
                  all_default=GAMES[game]['all_default'],   # every shader has its default variant: the runtime contributes only other states
                  listed_variants=sum(1 for _, _, _, why in jobs if why == 'listed'), skipped=failed,
                  archives=archives, seconds=time.monotonic()-started,
                  files={p.name: cache.digest(p.name, p) if p.name in linked else digest(p.read_bytes())
                         for p in sorted(staging.iterdir()) if p.name in wanted_final})
    (staging/'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    write_aliases(output.with_name(output.name+'.aliases.tsv'), {old: new for old, new in aliases.items() if new in completed})
    if output.exists():
        shutil.rmtree(output)
    staging.rename(output)
    cache.prune({variant for _, _, variant, _ in jobs} |
                {r[1] if r[0] == 'ok' else r[1]['variant'] for results in derived.values() for r in results})
    record_verified(output, report['files'])
    print(f'startup seed ready: {len(completed)} shader variants ({ready} startup shaders, the rest from the shared list), '
          f'{len(failed)} skipped; {done[2]} translations taken from the cache, {done[0]+done[1]-done[2]} translated', flush=True)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['romfs', 'output', 'translator', 'spirv-cross']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--shader-list', type=Path, help='the shared shader list (default: the game\'s, assets/shader_list.tsv)')
    p.add_argument('--modules', type=Path, help='rewritten/<version>: the game module images the list locates shaders in')
    p.add_argument('--game', choices=sorted(GAMES), default='ssbu', help='ssbu (default here), smo, totk or botw: the game the RomFS belongs to')
    a = p.parse_args()
    prepare(a.romfs.resolve(), a.output.resolve(), a.translator.resolve(), a.spirv_cross.resolve(), a.version,
            a.shader_list.resolve() if a.shader_list else None, a.modules.resolve() if a.modules else None, a.game)


if __name__ == '__main__':
    main()

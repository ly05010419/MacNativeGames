#!/usr/bin/env python3
"""Extract and translate MCD's Pak v8 shader libraries without playing the game.

Reads the bundled RomFS via its work symlink. No keys, formal caches or saves are
read/written. Only the unencrypted, uncompressed shader-library entries used by
this game are supported; unknown archive formats fail closed. UE shader-library
payloads are zlib-decoded with explicit bounds. NVN control/code bytes (including
the control-key tail read by this runtime) are preserved, never fabricated.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import uuid
import subprocess
import zlib

CHAIN = 'totk-native-v4-unverified'
MAX_INDEX = 128 << 20
MAX_LIBRARY = 256 << 20
MAX_SHADER = 16 << 20
MAGIC = b'\x34\x12\x76\x98'
ANSWERS = dict(tex={}, img={}, attr={}, fout={})


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        if not 0 <= n <= len(self.data)-self.at:
            raise ValueError('truncated metadata')
        b = self.data[self.at:self.at+n]
        self.at += n
        return b

    def unpack(self, fmt):
        v = struct.unpack(fmt, self.take(struct.calcsize(fmt)))
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.unpack('<i')
        if not 0 < abs(n) <= 65536:
            raise ValueError('invalid FString length')
        b = self.take(n if n > 0 else -2*n)
        tail = 1 if n > 0 else 2
        if b[-tail:] != bytes(tail):
            raise ValueError('unterminated FString')
        return b[:-tail].decode('utf-8' if n > 0 else 'utf-16-le')


def pak_libraries(read, size):
    """MCD uses the early v8 footer (4 methods) and one-byte method entries."""
    footer = Reader(read(size-189, 189))
    footer.take(16)  # GUID intentionally not emitted
    if footer.unpack('<B'):
        raise ValueError('encrypted Pak index unsupported')
    if footer.unpack('<II') != (0x5a6f12e1, 8):
        raise ValueError('expected MCD Pak v8')
    off, length = footer.unpack('<QQ')
    digest = footer.take(20)
    if not 0 < length <= MAX_INDEX:
        raise ValueError('unbounded Pak index')
    data = read(off, length)
    if hashlib.sha1(data).digest() != digest:
        raise ValueError('Pak index SHA1 mismatch')
    r = Reader(data)
    mount, count = r.string(), r.unpack('<i')
    if not 0 <= count <= 2000000:
        raise ValueError('invalid Pak file count')
    names, libraries = set(), []
    for _ in range(count):
        name = r.string()
        if name in names:
            raise ValueError('duplicate Pak entry')
        names.add(name)
        offset, packed, unpacked, method = r.unpack('<QQQB')
        digest = r.take(20)
        if method:
            blocks = r.unpack('<i')
            if not 0 <= blocks <= 65536:
                raise ValueError('invalid block count')
            r.take(blocks*16)
        flags, block_size = r.unpack('<BI')
        if not name.endswith('.ushaderbytecode'):
            continue
        if method or flags or packed != unpacked or not 0 < unpacked <= MAX_LIBRARY:
            raise ValueError('unsupported shader-library storage')
        stored = read(offset, 50)
        if struct.unpack_from('<QQQB', stored) != (0, packed, unpacked, method) or stored[25:45] != digest or stored[45:] != struct.pack('<BI', flags, block_size):
            raise ValueError('stored/index entry disagreement')
        payload = read(offset+50, packed)
        if hashlib.sha1(payload).digest() != digest:
            raise ValueError('shader library SHA1 mismatch')
        libraries.append((name, payload))
    if r.at != len(data):
        raise ValueError('unexpected Pak index trailing bytes')
    return dict(mount=mount, files=count, index_sha1=hashlib.sha1(data).hexdigest()), libraries


def shader_records(data):
    r = Reader(data)
    version, count = r.unpack('<II')
    if version != 1 or not 0 < count <= 1000000:
        raise ValueError('unsupported shader library')
    entries = []
    for _ in range(count):
        identity = r.take(20)  # UE source identity, NOT a digest of the cooked bytes
        off, packed, size, frequency = r.unpack('<QIIB')
        if not 0 < packed <= MAX_SHADER or not 0 < size <= MAX_SHADER:
            raise ValueError('unbounded shader payload')
        entries.append((identity, off, packed, size, frequency))
    base, previous_end = r.at, 0
    for identity, off, packed, size, frequency in entries:
        if off != previous_end or base+off+packed > len(data):
            raise ValueError('shader library payload range invalid')
        previous_end = off+packed
        raw = data[base+off:base+off+packed]
        if packed != size:
            decoder = zlib.decompressobj()
            payload = decoder.decompress(raw, size+1)
            if len(payload) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise ValueError('invalid zlib shader payload')
        else:
            payload = raw
        yield identity.hex(), frequency, payload
    if base+previous_end != len(data):
        raise ValueError('shader library trailing bytes')


def nvn_shader(payload, frequency):
    """Validate serialized control plus preceding code's explicit size prefix.

    The runtime hashes 0x880 bytes here, although this ABI's control is 0x848
    bytes; the subsequent assembly prefix is part of that key too. All 96 unique
    shaders captured in an isolated title run matched these bytes exactly.
    """
    if payload[:4] != b'LSLG':
        raise ValueError('expected Switch GLSL shader wrapper')
    candidates, at = [], 0
    while (at := payload.find(MAGIC, at)) >= 0:
        control_at, at = at, at+4
        if control_at+0x880 > len(payload):
            continue
        control = payload[control_at:control_at+0x880]
        if struct.unpack_from('<I', control, 0x14)[0] != 0x848:
            continue
        c1, length = struct.unpack_from('<II', control, 0x700)
        stage = struct.unpack_from('<I', control, 0x714)[0]
        start = control_at-4-length
        expected = {0: 0, 3: 1, 4: 2, 5: 5}.get(frequency)
        if stage != expected or not 0 <= c1 <= length <= MAX_SHADER or not length or start < 4:
            continue
        if struct.unpack_from('<I', payload, start-4)[0] != length:
            continue
        # Size prefix of the serialized control+assembly allocation.
        allocation = struct.unpack_from('<I', payload, control_at-4)[0]
        if allocation < 0x880 or control_at+allocation > len(payload):
            continue
        code = payload[start:start+length]
        candidates.append((sha(control+code), stage, control, code))
    if len(candidates) != 1:
        raise ValueError('ambiguous or missing serialized NVN control/code')
    return candidates[0]


def inventory(index, image):
    entries = json.loads(index.read_text())['files']
    shaders, archives, seen = {}, [], set()
    image_size = image.stat().st_size
    with image.open('rb') as f:
        for entry in entries:
            if not entry['path'].endswith('.pak'):
                continue
            start, size = entry['offset'], entry['size']
            if start < 0 or size < 189 or start+size > image_size:
                raise ValueError('Pak outside RomFS')
            def read(off, n):
                if off < 0 or n < 0 or off+n > size:
                    raise ValueError('read outside Pak')
                f.seek(start+off)
                b = f.read(n)
                if len(b) != n:
                    raise ValueError('short RomFS read')
                return b
            meta, libraries = pak_libraries(read, size)
            meta.update(path=entry['path'], libraries=[])
            for name, data in libraries:
                if name in seen:
                    raise ValueError('duplicate shader library across Paks needs explicit patch precedence')
                seen.add(name)
                count = 0
                for identity, frequency, payload in shader_records(data):
                    key, stage, control, code = nvn_shader(payload, frequency)
                    if key not in shaders:
                        shaders[key] = (stage, control, code)
                    count += 1
                meta['libraries'].append(dict(path=name, records=count, sha256=sha(data)))
            archives.append(meta)
    if not shaders:
        raise ValueError('no shaders found')
    return shaders, archives


def big_shared(control):
    """Compute kernel's shared memory bytes (control+0x754, rounded up to 16) when above 4 KiB default, else 0."""
    raw = struct.unpack_from('<I', control, 0x754)[0]
    if raw & 3 or raw > 0x7fffffff:
        raise ValueError('invalid compute shared-memory size')
    shared = (raw + 15) // 16 * 16
    return shared if raw <= 32768 and shared > 4096 else 0


def translation_request(key, stage, control, code):
    # Match runtime hashing: depth mode is a request setting, not a binding answer.
    answers = dict(ANSWERS)
    if stage == 5 and big_shared(control):
        answers['shared'] = big_shared(control)
    variant = key+'-'+sha(json.dumps(answers, sort_keys=True, separators=(',', ':')).encode())[:12]
    c1 = struct.unpack_from('<I', control, 0x700)[0]
    if stage != 5 and (c1 < 0x30 or len(code) < 0x30):
        raise ValueError('invalid graphics code header')
    request = dict(variant=variant, stage={0:'vs', 1:'ps', 5:'cs'}[stage],
                   c1=code[c1:].hex(), depth_minus_w=False, **answers)
    if stage == 5:
        request['local'] = list(struct.unpack_from('<III', control, 0x748))
        if big_shared(control):
            request['shared'] = big_shared(control)
    return request


def seed_files(output):
    manifest = json.loads((output/'seed-manifest.json').read_text())
    if manifest.get('schema') != 1 or manifest.get('chain') != CHAIN:
        raise ValueError('invalid seed manifest identity')
    files = manifest['files']
    if not isinstance(files, dict) or not files or manifest['shaders'] != sum(n.endswith('.variants') for n in files):
        raise ValueError('invalid seed file inventory')
    for name, digest in files.items():
        if Path(name).name != name or name in ('.', '..') or (output/name).is_symlink():
            raise ValueError('invalid seed filename')
        if sha((output/name).read_bytes()) != digest:
            raise ValueError('seed file digest mismatch: '+name)
    return files


def prepare_cached(index, image, output, translator, spirv_cross, jobs=4):
    # Reverify archive payloads on reuse: unchanged index alone is insufficient.
    _, archives = inventory(index, image)
    identity = preparation_identity(index, translator, spirv_cross)
    if output.is_symlink():
        raise ValueError('seed output must not be a symlink')
    try:
        report = json.loads((output/'offline-report.json').read_text())
        if report['identity'] == identity and report['archives'] == archives:
            seed_files(output)
            return report
    except (OSError, ValueError, KeyError, TypeError):
        pass
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name+'-staging-', dir=output.parent) as tmp:
        staging = Path(tmp)/'new'
        report = prepare(index, image, staging, translator, spirv_cross, jobs)
        seed_files(staging)
        # Preserve the previous generated seed; never delete an existing output.
        old = output.with_name(output.name+'.previous-'+uuid.uuid4().hex)
        if output.exists(): output.rename(old)
        try:
            staging.rename(output)
        except BaseException:
            if old.exists(): old.rename(output)
            raise
    return report


def preparation_identity(index, translator, spirv_cross):
    return dict(translator_sha256=sha(translator.read_bytes()), spirv_cross_sha256=sha(spirv_cross.read_bytes()),
                scanner_sha256=sha(Path(__file__).read_bytes()), index_sha256=sha(index.read_bytes()))


def prepare(index, image, output, translator, spirv_cross, jobs=4):
    if output.exists():
        raise ValueError('output must be a new independent directory')
    shaders, archives = inventory(index, image)
    output.mkdir(parents=True)
    keys = sorted(shaders)
    errors, unsupported, rows = {}, [], []
    identity = preparation_identity(index, translator, spirv_cross)
    def worker(batch):
        result = []
        with (output/f'worker-{batch}.log').open('w') as log:
            p = subprocess.Popen([str(translator.resolve()), '--service', '--spirv-cross', str(spirv_cross.resolve()), '--out', str(output.resolve()), '--chain', CHAIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                for key in keys[batch::jobs]:
                    stage, control, code = shaders[key]
                    if stage not in (0, 1, 5):
                        result.append((key, 'unsupported', stage)); continue
                    req = translation_request(key, stage, control, code)
                    variant = req['variant']
                    path = output/(variant+'.code')
                    path.write_bytes(code if stage == 5 else code[0x30:])
                    req['code'] = str(path.resolve())
                    p.stdin.write(json.dumps(req)+'\n'); p.stdin.flush()
                    response = p.stdout.readline()
                    if not response:
                        raise ValueError('translation service exited')
                    reply = json.loads(response); path.unlink()
                    if not reply.get('ok'):
                        result.append((key, 'failed', reply.get('error'))); continue
                    reflection = json.loads((output/(variant+'_b.json')).read_text())
                    if reflection.get('chain') != CHAIN or reflection.get('key') != variant+'_b' or reflection.get('stage') != req['stage']:
                        raise ValueError('translated shader identity mismatch')
                    if stage == 5 and reflection['deps'].get('shared') != req['shared']:
                        # Older pinned translators ignore this field. Do not publish
                        # their fallback-sized compute shaders as verified seeds.
                        for extension in ('_b.metal', '_b.json'):
                            (output/(variant+extension)).unlink(missing_ok=True)
                        result.append((key, 'failed', 'translator did not preserve shared-memory dependency'))
                        continue
                    (output/(key+'.variants')).write_text(json.dumps([dict(variant=variant, deps=reflection['deps'])])+'\n')
                    result.append((key, 'ok', (variant,stage)))
            finally:
                p.stdin.close(); p.wait(timeout=30)
        return result
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        batches = list(pool.map(worker, range(jobs)))
    for batch in batches:
        for key, state, value in batch:
            if state == 'failed': errors[key] = value
            elif state == 'unsupported': unsupported.append(dict(key=key,stage=value))
            else:
                variant, stage = value
                rows.append(f'S\t{variant}_b\t{stage}')
                if stage == 5: rows.append(f'C\t{variant}_b')
    (output/'pso_manifest.tsv').write_text('\n'.join(sorted(rows))+'\n')
    files = {p.name:sha(p.read_bytes()) for p in sorted(output.iterdir()) if p.name.endswith(('_b.metal','_b.json','.variants')) or p.name=='pso_manifest.tsv'}
    manifest = dict(schema=1, chain=CHAIN, shaders=sum(name.endswith('.variants') for name in files), files=files,
                    source='romfs-pak-shader-libraries', all_default=False)
    report = dict(identity=identity, archives=archives, raw_shaders=len(shaders), prepared=manifest['shaders'], errors=errors, unsupported=unsupported,
                  scope='All shader-library records in verified Pak indices; embedded material maps and all binding/graphics PSO combinations are not claimed.')
    (output/'offline-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (output/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('index','image','output','translator','spirv-cross'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--reuse', action='store_true', help='verify and reuse, or regenerate preserving the previous seed')
    a=p.parse_args()
    if not 1 <= a.jobs <= 8: p.error('jobs must be 1..8')
    report=(prepare_cached if a.reuse else prepare)(a.index,a.image,a.output,a.translator,a.spirv_cross,a.jobs)
    print(json.dumps({k:report[k] for k in ('raw_shaders','prepared')})+f" failures={len(report['errors'])} unsupported={len(report['unsupported'])}")

if __name__=='__main__': main()

"""Test game files made the way real ones are, for the identification tests (tools/builder_jobs/identify.py): an NSP
(PFS0) or a game card image (XCI: HEAD, root HFS0, secure HFS0) holding Meta NCAs, each an NCA3 whose header is
AES-XTS-encrypted with a test header_key and whose section 0 is an AES-CTR PFS0 holding one content meta (.cnmt),
its key in the key area under a test key_area_key_application. The keys are made up here; nothing comes from a
console. A package's file name is free, so the tests can give it a name that says another game."""
import hashlib
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HEADER_KEY = bytes(range(32))
KEY_AREA_KEY = bytes(range(0x40, 0x50))
CTR_KEY = bytes(range(0x80, 0x90))
KINDS = {'base': 0x80, 'update': 0x81, 'dlc': 0x82, 'delta': 0x83}


def keys_text(generations=(0,)):
    """A keys file with the test header_key and the key area keys of these generations (0-based)."""
    lines = ['header_key = ' + HEADER_KEY.hex()]
    lines += [f'key_area_key_application_{g:02x} = ' + KEY_AREA_KEY.hex() for g in generations]
    lines += [f'titlekek_{g:02x} = ' + KEY_AREA_KEY.hex() for g in generations]
    return '\n'.join(lines) + '\n'


def _crypt(key, mode, data, encrypt=True):
    c = Cipher(algorithms.AES(key), mode)
    ctx = c.encryptor() if encrypt else c.decryptor()
    return ctx.update(data) + ctx.finalize()


def pfs0(files, stride=24, magic=b'PFS0'):
    """A PFS0 (or, with stride 64 and magic HFS0, an HFS0) of [(name, bytes)]."""
    names = b''
    offsets = []
    for name, _ in files:
        offsets.append(len(names))
        names += name.encode() + b'\0'
    names += b'\0' * (-len(names) % 16)
    table, data = b'', b''
    for (name, body), name_at in zip(files, offsets):
        entry = struct.pack('<QQI', len(data), len(body), name_at)
        table += entry + b'\0' * (stride - len(entry))
        data += body
    return magic + struct.pack('<III', len(files), len(names), 0) + table + names + data


def meta_nca(title, kind='base', version=0, generation=0):
    """A Meta NCA of this title ID (16 hex digits), content meta kind and version, of this key generation (0-based)."""
    tid = int(title, 16)
    cnmt = struct.pack('<QIB', tid, version, KINDS[kind]) + b'\0' * (0x20 - 13) + b'\0' * 0x40
    pfs = pfs0([(f'Application_{title.lower()}.cnmt', cnmt)])
    hashes = hashlib.sha256(pfs).digest()
    section = hashes + b'\0' * (0x200 - len(hashes)) + pfs
    section += b'\0' * (-len(section) % 0x200)
    start = 0xC00 // 0x200
    end = start + len(section) // 0x200
    ctr = bytes(range(0xA0, 0xA8))
    fs = bytearray(0x200)
    fs[0:2] = (2).to_bytes(2, 'little')
    fs[2], fs[3], fs[4] = 1, 2, 3   # PFS0, hierarchical SHA-256, AES-CTR
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 0x1000, 2, 0, 32, 0x200, len(pfs))
    fs[0x140:0x148] = ctr
    size = 0xC00 + len(section)
    gen = generation + 1 if generation else 0   # header fields: 0 and 1 both mean the first generation
    hdr = bytearray(0xC00)
    hdr[0x200:0x204] = b'NCA3'
    hdr[0x205], hdr[0x206], hdr[0x207] = 1, min(gen, 2), 0
    struct.pack_into('<QQ', hdr, 0x208, size, tid)
    hdr[0x220] = gen if gen > 2 else 0
    struct.pack_into('<II', hdr, 0x240, start, end)
    hdr[0x280:0x2A0] = hashlib.sha256(bytes(fs)).digest()
    key_area = b'\0' * 32 + CTR_KEY + b'\0' * 16
    hdr[0x300:0x340] = _crypt(KEY_AREA_KEY, modes.ECB(), key_area)
    hdr[0x400:0x600] = fs
    header = b''.join(_crypt(HEADER_KEY, modes.XTS(i.to_bytes(16, 'big')), bytes(hdr[i * 0x200:(i + 1) * 0x200]))
                      for i in range(6))
    iv = ctr[::-1] + (0xC00 // 16).to_bytes(8, 'big')
    return header + _crypt(CTR_KEY, modes.CTR(iv), section)


def ticket(title):
    """A common ticket (RSA-2048 / SHA-256 signature) whose rights ID is this title's."""
    body = 0x140
    data = bytearray(body + 0x2C0)
    data[0:4] = (0x10004).to_bytes(4, 'little')
    data[body + 0x160:body + 0x170] = int(title, 16).to_bytes(8, 'big') + b'\0' * 8
    return bytes(data)


def nsp(path, metas=(), tickets=()):
    """An NSP at path: a Meta NCA for each (title, kind, version[, generation]) and a ticket for each title."""
    files = [(f'{i:032x}.cnmt.nca', meta_nca(*m)) for i, m in enumerate(metas)]
    files += [(f'{t.lower()}{0:016x}.tik', ticket(t)) for t in tickets]
    path.write_bytes(pfs0(files))
    return path


def xci(path, metas=()):
    """A game card image at path: its secure partition holds a Meta NCA for each (title, kind, version[, generation])."""
    secure = pfs0([(f'{i:032x}.cnmt.nca', meta_nca(*m)) for i, m in enumerate(metas)], 64, b'HFS0')
    root = pfs0([('secure', secure)], 64, b'HFS0')
    head = bytearray(0x200)
    head[0x100:0x104] = b'HEAD'
    struct.pack_into('<Q', head, 0x130, 0x200)
    path.write_bytes(bytes(head) + root)
    return path

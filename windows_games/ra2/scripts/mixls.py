"""List the entries of a Red Alert 2 MIX file (development aid).

    python3 -I scripts/mixls.py [--game <game dir>] <file.mix> [name ...]

Prints each entry's id, offset and size; with names, which of them the MIX
holds (ids are the CRC-32 of the upper-cased name, padded as Westwood's
tools do). Encrypted headers (flag 0x00020000) are decrypted as the game
does: the 80-byte key source is two blocks raised to Westwood's public RSA
exponent, which gives the 56-byte Blowfish key; Blowfish works on
standard big-endian blocks. Blowfish's tables are pi's hex digits,
computed here. The public key is read from the player's own game.exe or
gamemd.exe (--game, by default the MIX file's folder): nothing of the game
is kept in this script.
"""

import base64
import binascii
import re
import struct
import sys
from pathlib import Path

FLAG_CHECKSUM, FLAG_ENCRYPTED = 0x00010000, 0x00020000
EXECUTABLES = ('game.exe', 'gamemd.exe')


def public_key(game_dir):
    """Westwood's public RSA modulus, from the player's own game.exe or
    gamemd.exe: the executables carry it as a small INI ("[PublicKey]", then
    "1=" and the base64 text of a DER INTEGER of 40 bytes); the exponent is
    0x10001."""
    files = {f.name.lower(): f for f in Path(game_dir).iterdir() if f.is_file()}
    for name in EXECUTABLES:
        if name not in files:
            continue
        data = files[name].read_bytes()
        for m in re.finditer(rb'\[PublicKey\]\r?\n1=([A-Za-z0-9+/=]+)', data):
            der = base64.b64decode(m.group(1))
            if der[0] == 2 and der[1] == 40:
                return int.from_bytes(der[2:42], 'big')
    raise SystemExit(f'{game_dir}: no game.exe or gamemd.exe holding the MIX public key')


def blowfish_key(key_source, n):
    out = b''
    for i in range(0, 80, 40):
        m = int.from_bytes(key_source[i:i + 40], 'little')
        out += pow(m, 0x10001, n).to_bytes(39, 'little')
    return out[:56]


def _pi_words(count):
    """The first `count` 32-bit words of pi's fractional part (Blowfish's
    P-array and S-boxes, in that order), by Machin's formula."""
    bits = 32 * count + 64
    one = 1 << bits

    def arctan_inv(x):
        total, term, k, sign = 0, one // x, 1, 1
        x2 = x * x
        while term:
            total += sign * (term // k)
            term //= x2
            k += 2
            sign = -sign
        return total

    pi = 16 * arctan_inv(5) - 4 * arctan_inv(239)
    frac = pi - (3 << bits)
    frac >>= 64
    return [(frac >> (32 * (count - 1 - i))) & 0xFFFFFFFF for i in range(count)]


class Blowfish:
    def __init__(self, key):
        words = _pi_words(18 + 4 * 256)
        self.p = words[:18]
        self.s = [words[18 + 256 * i:18 + 256 * (i + 1)] for i in range(4)]
        k = 0
        for i in range(18):
            v = 0
            for _ in range(4):
                v = (v << 8) | key[k % len(key)]
                k += 1
            self.p[i] ^= v
        l = r = 0
        for i in range(0, 18, 2):
            l, r = self._enc(l, r)
            self.p[i], self.p[i + 1] = l, r
        for box in self.s:
            for i in range(0, 256, 2):
                l, r = self._enc(l, r)
                box[i], box[i + 1] = l, r

    def _f(self, x):
        s = self.s
        h = (s[0][x >> 24] + s[1][(x >> 16) & 0xFF]) & 0xFFFFFFFF
        return ((h ^ s[2][(x >> 8) & 0xFF]) + s[3][x & 0xFF]) & 0xFFFFFFFF

    def _enc(self, l, r):
        for i in range(16):
            l ^= self.p[i]
            r ^= self._f(l)
            l, r = r, l
        l, r = r, l
        return l ^ self.p[17], r ^ self.p[16]

    def _dec(self, l, r):
        for i in range(17, 1, -1):
            l ^= self.p[i]
            r ^= self._f(l)
            l, r = r, l
        l, r = r, l
        return l ^ self.p[0], r ^ self.p[1]

    def decrypt(self, data):
        """ECB, standard (big-endian) Blowfish blocks."""
        out = bytearray()
        for i in range(0, len(data) - len(data) % 8, 8):
            l, r = struct.unpack_from('>II', data, i)
            l, r = self._dec(l, r)
            out += struct.pack('>II', l, r)
        return bytes(out)


def mix_id(name):
    s = name.upper().encode('ascii')
    n = len(s)
    a = n >> 2
    if n & 3:
        s += bytes([n - (a << 2)])
        s += bytes([s[a << 2]]) * (3 - (n & 3))
    return binascii.crc32(s) & 0xFFFFFFFF


def entries(path, key=None):
    with open(path, 'rb') as f:
        return parse(f.read(), key)


def parse(data, key=None):
    """Each entry's (offset, size) in `data` (a whole MIX file), by id. `key`
    (public_key) is needed only for an encrypted header."""
    flags = struct.unpack_from('<I', data, 0)[0]
    if flags & ~(FLAG_CHECKSUM | FLAG_ENCRYPTED):  # an old, flagless header
        count, size = struct.unpack_from('<HI', data, 0)
        table, body = data[6:6 + 12 * count], 6 + 12 * count
    elif flags & FLAG_ENCRYPTED:
        if key is None:
            raise ValueError('an encrypted MIX header: pass the public key (public_key(<game dir>))')
        bf = Blowfish(blowfish_key(data[4:84], key))
        first = bf.decrypt(data[84:92])
        count, size = struct.unpack_from('<HI', first, 0)
        nbytes = (6 + 12 * count + 7) & ~7
        header = bf.decrypt(data[84:84 + nbytes])
        table, body = header[6:6 + 12 * count], 84 + nbytes
    else:
        count, size = struct.unpack_from('<HI', data, 4)
        table, body = data[10:10 + 12 * count], 10 + 12 * count
    out = {}
    for i in range(count):
        id_, off, sz = struct.unpack_from('<III', table, 12 * i)
        out[id_] = (body + off, sz)
    return out


def main():
    args = sys.argv[1:]
    game = None
    if args[:1] == ['--game']:
        game, args = args[1], args[2:]
    path, names = args[0], args[1:]
    data = Path(path).read_bytes()
    flags = struct.unpack_from('<I', data, 0)[0]
    encrypted = not flags & ~(FLAG_CHECKSUM | FLAG_ENCRYPTED) and flags & FLAG_ENCRYPTED
    ents = parse(data, public_key(game or Path(path).parent) if encrypted else None)
    print(f'{path}: {len(ents)} entries')
    if names:
        for n in names:
            e = ents.get(mix_id(n))
            print(f'  {n:24s} {"%08X" % mix_id(n)}  ' +
                  (f'offset {e[0]:#x} size {e[1]}' if e else 'not present'))
    else:
        for id_, (off, sz) in sorted(ents.items(), key=lambda kv: kv[1][0]):
            print(f'  {id_:08X} {off:#10x} {sz}')


if __name__ == '__main__':
    main()

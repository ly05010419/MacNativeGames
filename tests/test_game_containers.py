"""Container integration tests using small, synthetic encrypted NCAs; no game data or real keys.

Run with the build environment's Python: python -m unittest discover -s tests -v
"""
import hashlib
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
sys.path[:0] = [str(ROOT), str(SCRIPTS)]
import build_app
from extract_exefs import container_entries, partition_entries, load_keys
from extract_app_icon import control_nca
from extract_shared_fonts import Slice, nca_romfs, firmware_data_ncas, FONTS
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HEADER = bytes(range(32))
AREA = bytes(range(32, 48))
TITLEKEK = bytes(range(48, 64))
CONTENT = bytes(range(64, 80))
RIGHTS = bytes.fromhex('0100f2c0115b60000000000000000001')
TITLE = 0x0100F2C0115B6000


def encrypt(key, mode, data):
    enc = Cipher(algorithms.AES(key), mode).encryptor()
    return enc.update(data) + enc.finalize()


def partition(files, hfs=False):
    stride = 64 if hfs else 24
    strings = b''
    table = bytearray()
    payload = b''
    for name, data in files:
        table.extend(struct.pack('<QQII', len(payload), len(data), len(strings), 0) + bytes(stride - 24))
        strings += name.encode() + b'\0'
        payload += data
    return (b'HFS0' if hfs else b'PFS0') + struct.pack('<III', len(files), len(strings), 0) + table + strings + payload


def romfs():
    name = b'icon_AmericanEnglish.dat'
    payload = b'synthetic icon payload'
    dirs = struct.pack('<6I', 0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)
    files = struct.pack('<IIQQII', 0, 0xffffffff, 0, len(payload), 0xffffffff, len(name)) + name
    dataoff = 80 + len(dirs) + len(files)
    head = struct.pack('<10Q', 80, 80, 0, 80, len(dirs), 104, 0, 104, len(files), dataoff)
    return head + dirs + files + payload


def ivfc(image):
    layers = [image]
    for _ in range(5):
        data = layers[0]
        layers.insert(0, b''.join(hashlib.sha256(data[i:i+4096].ljust(4096, b'\0')).digest()
                                  for i in range(0, len(data), 4096)))
    fs = bytearray(512)
    fs[2:5] = bytes([0, 3, 3])
    fs[8:12] = b'IVFC'
    struct.pack_into('<III', fs, 12, 0x20000, 32, 7)
    section = b''
    for i, data in enumerate(layers):
        struct.pack_into('<QQII', fs, 0x18+i*24, len(section), len(data), 12, 0)
        section += data.ljust((len(data)+4095)//4096*4096, b'\0')
    fs[0xc8:0xe8] = hashlib.sha256(layers[0].ljust(4096, b'\0')).digest()
    return fs, section


def nca(managed=False, title=TITLE, content=0):
    pfs = partition([('main', b'synthetic main'), ('main.npdm', b'synthetic metadata')])
    fs = bytearray(512)
    fs[2:5] = bytes([1, 2, 3])
    hashes = hashlib.sha256(pfs).digest()
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 4096, 2, 0, len(hashes), 512, len(pfs))
    exefs = hashes.ljust(512, b'\0') + pfs.ljust(512, b'\0')
    romfs_header, image = ivfc(romfs())
    sections = [(fs, exefs), (romfs_header, image)] if content == 0 else [(romfs_header, image)]
    hdr = bytearray(0xc00)
    hdr[0x200:0x204] = b'NCA3'
    hdr[0x205] = content
    hdr[0x206] = 1
    struct.pack_into('<Q', hdr, 0x210, title)
    if managed:
        hdr[0x230:0x240] = RIGHTS
    else:
        hdr[0x300:0x340] = encrypt(AREA, modes.ECB(), bytes(32) + CONTENT + bytes(16))
    body = b''
    for i, (section_header, data) in enumerate(sections):
        start = 0xc00 + len(body)
        struct.pack_into('<II', hdr, 0x240 + i*16, start//512, (start+len(data))//512)
        hdr[0x400+i*512:0x600+i*512] = section_header
        hdr[0x280+i*32:0x2a0+i*32] = hashlib.sha256(section_header).digest()
        body += encrypt(CONTENT, modes.CTR(bytes(8) + (start//16).to_bytes(8, 'big')), data)
    struct.pack_into('<Q', hdr, 0x208, len(hdr)+len(body))
    encrypted = b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), hdr[i*512:(i+1)*512]) for i in range(6))
    return encrypted + body


def ticket(personalized=False, bad_key=False):
    data = bytearray(0x2c0)
    struct.pack_into('<I', data, 0, 0x10004)
    data[0x180:0x190] = encrypt(TITLEKEK, modes.ECB(), bytes(16) if bad_key else CONTENT)
    data[0x281] = int(personalized)
    data[0x2a0:0x2b0] = RIGHTS
    return bytes(data)


def game_image(kind, managed=False, **kwargs):
    files = [('program.nca', nca(managed, **kwargs)), ('control.nca', nca(managed, content=2))]
    if managed:
        files.append(('common.tik', ticket()))  # Deliberately after the NCAs.
    if kind == 'nsp':
        return partition(files)
    head = bytearray(0x200)
    head[0x100:0x104] = b'HEAD'
    struct.pack_into('<Q', head, 0x130, 0x200)
    return head + partition([('secure', partition(files, hfs=True))], hfs=True)


class Containers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.keys = self.path / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in
                                      [('header_key', HEADER), ('key_area_key_application_00', AREA), ('titlekek_00', TITLEKEK)]))

    def extract(self, game, dest='extracted', success=True):
        out = self.path / dest / 'exefs'
        p = subprocess.run([sys.executable, SCRIPTS / 'extract_exefs.py', game, '--keys', self.keys, '--output', out],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode == 0, success, p.stdout + p.stderr)
        # Tool logs/manifests must never contain key bytes.
        for secret in (CONTENT, TITLEKEK, HEADER):
            self.assertNotIn(secret.hex(), p.stdout + p.stderr)
        return out, p

    def test_full_extraction_xci_nsp_and_common_ticket(self):
        for kind, managed in [('xci', False), ('nsp', False), ('nsp', True)]:
            with self.subTest(kind=kind, managed=managed):
                stem = f'{kind}-{managed}'
                game = self.path / f'{stem}.{kind}'
                game.write_bytes(game_image(kind, managed))
                out, _ = self.extract(game, stem)
                self.assertEqual((out / 'main').read_bytes(), b'synthetic main')
                manifest = out.parent / 'extraction-manifest.json'
                self.assertTrue(build_app.source_matches(manifest, game))
                self.assertEqual(json.loads(manifest.read_text())['container'], kind)
                romout = out.parent / 'romfs'
                p = subprocess.run([sys.executable, SCRIPTS / 'extract_romfs.py', '--keys', self.keys,
                                    '--manifest', manifest, '--output', romout], capture_output=True, text=True)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertEqual((romout / 'romfs.bin').read_bytes(), romfs())
                self.assertEqual(json.loads((romout / 'index.json').read_text())['ivfc_blocks'], [1]*6)
                offset, tickets = control_nca(game, load_keys(self.keys), f'{TITLE:016x}')
                control = self.path / f'{stem}-control.bin'
                nca_romfs(Slice(game, offset), load_keys(self.keys), control, tickets)
                self.assertEqual(control.read_bytes(), romfs())
                old = json.loads(manifest.read_text())
                old['source_mtime_ns'] -= 1
                manifest.write_text(json.dumps(old))
                self.assertFalse(build_app.source_matches(manifest, game))

    def test_missing_personalized_conflicting_and_wrong_tickets(self):
        variants = [([], 'Missing common ticket'),
                    ([('a.tik', ticket(True))], 'Personalized'),
                    ([('a.tik', ticket()), ('b.tik', ticket(bad_key=True))], 'Conflicting'),
                    ([('a.tik', ticket(bad_key=True))], 'hash-table mismatch')]
        for i, (tickets, message) in enumerate(variants):
            with self.subTest(message=message):
                game = self.path / f'bad{i}.nsp'
                game.write_bytes(partition([('program.nca', nca(True))] + tickets))
                _, result = self.extract(game, f'bad{i}', False)
                self.assertIn(message, result.stderr)

    def test_missing_titlekek(self):
        self.keys.write_text(f'header_key = {HEADER.hex()}\n')
        game = self.path / 'base.nsp'
        game.write_bytes(game_image('nsp', True))
        _, result = self.extract(game, success=False)
        self.assertIn('Missing titlekek_00', result.stderr)

    def test_reject_update_and_other_title(self):
        for title in (TITLE + 0x800, 0x0100000000000000):
            game = self.path / 'update.nsp'
            game.write_bytes(game_image('nsp', title=title))
            _, result = self.extract(game, success=False)
            self.assertIn('Expected Tears of the Kingdom base game', result.stderr)

    def test_update_metadata_with_base_program_id(self):
        for metadata in ([('update.cnmt.nca', nca(title=TITLE + 0x800, content=1))],
                         [('base.cnmt.nca', nca(content=1)), ('dlc.cnmt.nca', nca(title=TITLE + 0x1001, content=1))]):
            game = self.path / 'update.nsp'
            game.write_bytes(partition([('program.nca', nca())] + metadata))
            _, result = self.extract(game, success=False)
            self.assertIn('Expected base game metadata', result.stderr)

    def test_corruption_rejected_by_exefs_and_romfs_integrity(self):
        game = self.path / 'corrupt.nsp'
        image = bytearray(game_image('nsp'))
        _, entries = container_entries(io.BytesIO(image), len(image))
        offset = entries[0][1]
        image[offset + 0xc00] ^= 1
        game.write_bytes(image)
        _, result = self.extract(game, success=False)
        self.assertIn('hash-table mismatch', result.stderr)
        image = bytearray(game_image('nsp'))
        image[offset + 0x1000 + 5*4096 + 120] ^= 1
        game.write_bytes(image)
        out, _ = self.extract(game)
        p = subprocess.run([sys.executable, SCRIPTS / 'extract_romfs.py', '--keys', self.keys, '--manifest',
                            out.parent / 'extraction-manifest.json', '--output', out.parent / 'romfs'], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('IVFC level 5', p.stderr)

    def test_invalid_partition_tables(self):
        valid = partition([('a', b'123'), ('b', b'456')])
        bad_offset = bytearray(valid)
        struct.pack_into('<Q', bad_offset, 16, 10**9)
        overlap = bytearray(valid)
        struct.pack_into('<Q', overlap, 40, 1)
        unterminated = bytearray(partition([('a', b'')]))
        unterminated[-1] = ord('b')
        for data in (valid[:12], valid[:-1], bad_offset, overlap, unterminated,
                     partition([('../a', b'')]), partition([('a', b''), ('a', b'')])):
            with self.subTest(data_length=len(data)), self.assertRaises(ValueError):
                partition_entries(io.BytesIO(data), 0, len(data))

    def test_fonts_verify_bytes_and_complete_manifest(self):
        folder = self.path / 'fonts'
        folder.mkdir()
        expected = {}
        for i in range(6):
            data = bytes([i])*16
            (folder / f'{i}-font{i}.bfttf').write_bytes(data)
            expected[f'font{i}'] = hashlib.sha256(data).hexdigest()
        with patch.dict(build_app.EXPECTED, {'shared_fonts_sha256': expected}):
            report = build_app.font_report(folder)
            (folder / 'manifest.json').write_text(json.dumps(report))
            self.assertTrue(build_app.fonts_valid(folder))
            report['fonts'].pop()
            (folder / 'manifest.json').write_text(json.dumps(report))
            self.assertFalse(build_app.fonts_valid(folder))
            (folder / '0-font0.bfttf').write_bytes(b'corrupt')
            with self.assertRaises(build_app.BuildError):
                build_app.font_report(folder)

    def test_npdm_signature_accepts_another_genuine_dump(self):
        # The pinned hash names one dump; Nintendo's signature over the ACID block names every genuine one.
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        # The signed body is acid->size bytes starting at acid+0x100 and includes the ACID header itself.
        body = bytearray(bytes(range(256)) + b'access control')
        body[0x100:0x104] = b'ACID'
        struct.pack_into('<I', body, 0x104, len(body))
        signature = key.sign(bytes(body), padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        npdm = bytearray(0x180 + len(body))
        npdm[:4] = b'META'
        struct.pack_into('<II', npdm, 0x78, 0x80, 0x100 + len(body))
        npdm[0x80:0x180] = signature
        npdm[0x180:0x180 + len(body)] = body
        path = self.path / 'main.npdm'
        path.write_bytes(npdm)
        with patch.object(build_app, 'NINTENDO_ACID_MODULUS', key.public_key().public_numbers().n.to_bytes(256, 'big')):
            self.assertTrue(build_app.npdm_signed(path))
            tampered = bytearray(npdm)
            tampered[0x180] ^= 1
            path.write_bytes(tampered)
            self.assertFalse(build_app.npdm_signed(path))
            path.write_bytes(npdm)
            struct.pack_into('<I', npdm, 0x04, 1)
            path.write_bytes(npdm)
            self.assertFalse(build_app.npdm_signed(path))

    def test_resigned_npdm_is_accepted_only_with_matching_code(self):
        # Dump tools that change an NCA's key generation sign the npdm's ACID again (#65, #80): not Nintendo's signature,
        # but the title's own npdm; the game never reads it, so it passes when every code module matches.
        def npdm(program_id):
            data = bytearray(0x200)
            data[:4] = b'META'
            struct.pack_into('<II', data, 0x70, 0x100, 0x40)   # ACI0 at 0x100
            data[0x100:0x104] = b'ACI0'
            struct.pack_into('<Q', data, 0x110, program_id)
            return bytes(data)
        exefs = self.path / 'exefs'
        exefs.mkdir()
        code = {name: f'{name} bytes'.encode() for name in ('main', 'rtld', 'sdk', 'subsdk0')}
        for name, data in code.items():
            (exefs / name).write_bytes(data)
        (exefs / 'main.npdm').write_bytes(npdm(0x0100F2C0115B6000))
        expected = {name: hashlib.sha256(data).hexdigest() for name, data in code.items()}
        expected['main.npdm'] = '0' * 64   # the reference dump's file is another one
        self.assertEqual(build_app.npdm_program_id(exefs / 'main.npdm'), '0100F2C0115B6000')
        self.assertEqual(build_app.npdm_kind(exefs / 'main.npdm', '0100f2c0115b6000'), 'resigned')
        self.assertTrue(build_app.exefs_matches(exefs, expected, '0100F2C0115B6000'))
        self.assertFalse(build_app.exefs_matches(exefs, expected, '01007EF00011E000'))   # another title's npdm
        (exefs / 'sdk').write_bytes(b'patched sdk')
        self.assertFalse(build_app.exefs_matches(exefs, expected, '0100F2C0115B6000'))   # code that differs still stops it
        (exefs / 'main.npdm').write_bytes(b'META' + bytes(0x7c))   # no ACI0
        self.assertIsNone(build_app.npdm_kind(exefs / 'main.npdm', '0100F2C0115B6000'))

    def test_font_from_another_firmware_is_bound_by_its_bytes(self):
        folder = self.path / 'fonts'
        folder.mkdir()
        for i in range(6):
            (folder / f'{i}-font{i}.bfttf').write_bytes(struct.pack('>III', 0x7F9A0218, 12, 0x00010000) + bytes([i]) * 8)
        pinned = {f'font{i}': '0' * 64 for i in range(6)}
        with patch.dict(build_app.EXPECTED, {'shared_fonts_sha256': pinned}):
            report = build_app.font_report(folder)
            self.assertEqual([row['sha256'] for row in report['fonts']],
                             [hashlib.sha256((folder / f'{i}-font{i}.bfttf').read_bytes()).hexdigest() for i in range(6)])
            (folder / '0-font0.bfttf').write_bytes(b'not a font')
            with self.assertRaises(build_app.BuildError):
                build_app.font_report(folder)

    def test_nsp_missing_fonts_stops_before_build(self):
        game = self.path / 'game.nsp'
        game.write_bytes(game_image('nsp'))
        with patch.object(build_app, 'prerequisites') as prereq:
            with self.assertRaisesRegex(build_app.BuildError, 'NSP files do not include system shared fonts'):
                build_app.main([str(game), '-k', str(self.keys)])
            prereq.assert_not_called()

    def test_firmware_flat_and_registered_directories(self):
        for installed in (False, True):
            folder = self.path / str(installed)
            folder.mkdir()
            for title in {title for _, _, title, _ in FONTS}:
                path = folder / f'{title}.nca'
                if installed:
                    path.mkdir()
                    path = path / '00'
                path.write_bytes(nca(title=int(title, 16), content=4))
            found = firmware_data_ncas(folder, load_keys(self.keys))
            self.assertEqual(set(found), {title for _, _, title, _ in FONTS})
            next(iter(found.values())).unlink()
            with self.assertRaisesRegex(ValueError, 'missing shared-font'):
                firmware_data_ncas(folder, load_keys(self.keys))

    def test_mac_only(self):
        with patch('build_app.platform.system', return_value='iOS'), self.assertRaisesRegex(build_app.BuildError, 'Mac'):
            build_app.prerequisites(None)
        with patch('build_app.platform.system', return_value='Darwin'), patch('build_app.platform.machine', return_value='x86_64'), self.assertRaisesRegex(build_app.BuildError, 'Apple silicon'):
            build_app.prerequisites(None)


if __name__ == '__main__':
    unittest.main()

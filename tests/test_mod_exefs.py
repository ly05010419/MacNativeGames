"""mod_exefs.py: merging a mod's main code patches (IPS / IPS32 / pchtxt) into an extracted retail main, checked
against a synthetic NSO module (no game data)."""
import argparse
import contextlib
import hashlib
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT/'games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
import mod_exefs            # noqa: E402
import patch_sites          # noqa: E402

try:
    import lz4.block
except ImportError:
    lz4 = None

BUILD_ID = bytes.fromhex('db0507f36b84200ac05f75d76ed5026f')
TEXT_SIZE, RODATA_ADDRESS, RODATA_SIZE, DATA_ADDRESS, DATA_SIZE, BSS = 0x400, 0x2000, 0x100, 0x3000, 0x40, 0x200
TEXT = bytes((index * 7 + 3) & 0xff for index in range(TEXT_SIZE))
RODATA = bytes((index * 11 + 5) & 0xff for index in range(RODATA_SIZE))
DATA = bytes((index * 13 + 9) & 0xff for index in range(DATA_SIZE))


def nso_bytes(*, compress=False, digests=True):
    """A synthetic NSO module: three segments, a 20-byte build ID, optional LZ4 segments and header digests."""
    segments = [(TEXT, 0), (RODATA, RODATA_ADDRESS), (DATA, DATA_ADDRESS)]
    stored, csz = [], []
    for data, _ in segments:
        payload = lz4.block.compress(data, store_size=False) if compress else data
        stored.append(payload)
        csz.append(len(payload) if compress else len(data))
    header = bytearray(0x100)
    header[0:4] = b'NSO0'
    struct.pack_into('<I', header, 0x0c, (0b111 if compress else 0) | ((0b111 << 3) if digests else 0))
    offset = 0x100
    for index, ((data, address), payload) in enumerate(zip(segments, stored)):
        struct.pack_into('<IIII', header, 0x10 + 16*index, offset, address, len(data), address)
        header[0xa0 + 32*index:0xc0 + 32*index] = hashlib.sha256(data).digest()
        offset += len(payload)
    struct.pack_into('<I', header, 0x3c, BSS)
    header[0x40:0x50] = BUILD_ID
    struct.pack_into('<III', header, 0x60, *csz)
    return bytes(header) + b''.join(stored)


def image_of(raw):
    return bytes(mod_exefs.Nso(raw).image())


def ips(records, *, magic=b'PATCH', offset_size=3, end=b'EOF', pad=b''):
    out = bytearray(magic + pad)
    for offset, payload in records:
        out += offset.to_bytes(offset_size, 'big') + len(payload).to_bytes(2, 'big') + payload
    return bytes(out + end)


def ips_rle(offset, count, value, *, magic=b'PATCH', offset_size=3):
    return bytes(magic + offset.to_bytes(offset_size, 'big') + b'\0\0' + count.to_bytes(2, 'big')
                 + bytes([value]) + b'EOF')


def pchtxt(records, *, build_id=BUILD_ID.hex(), enabled=True, stop=True):
    lines = [f'@nsobid-{build_id}', '@flag offset_shift 0x100']
    if enabled:
        lines.append('@enabled')
    lines += [f'{address:x} {payload.hex()}' for address, payload in records]
    if stop:
        lines.append('@stop')
    return '\n'.join(lines) + '\n'


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.exefs = self.root/'exefs'
        self.exefs.mkdir()
        self.mod = self.root/'Mod'
        (self.mod/'exefs').mkdir(parents=True)
        self.output = self.root/'merged/main'
        self.report = self.root/'merged/mod-exefs.json'

    def tearDown(self):
        self.tmp.cleanup()

    def retail(self, **kwargs):
        raw = nso_bytes(**kwargs)
        (self.exefs/'main').write_bytes(raw)
        return raw

    def patch(self, text, name='main.pchtxt'):
        path = self.mod/'exefs'/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text) if isinstance(text, bytes) else path.write_text(text)
        return path

    def apply(self, *, version='v121', sites=None):
        if sites is not None:
            original, patch_sites.sites = patch_sites.sites, lambda *a, **k: sites
            self.addCleanup(lambda: setattr(patch_sites, 'sites', original))
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            report = mod_exefs.apply(argparse.Namespace(exefs=self.exefs, mod=self.mod, version=version,
                                                        output=self.output, report=self.report))
        self.output_text = stream.getvalue()
        return report

    def merged(self):
        return json.loads(self.report.read_text())

    # ---- pchtxt ----
    def test_pchtxt_patch_reaches_the_image(self):
        retail = self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd'), (RODATA_ADDRESS, b'\x01\x02')]))
        report = self.apply()
        image, before = image_of(self.output.read_bytes()), image_of(retail)
        self.assertEqual(image[0x40:0x44], b'\xaa\xbb\xcc\xdd')
        self.assertEqual(image[RODATA_ADDRESS:RODATA_ADDRESS + 2], b'\x01\x02')
        self.assertEqual(image[:0x40], before[:0x40])                       # everything else is the retail image
        self.assertEqual(image[0x44:RODATA_ADDRESS], before[0x44:RODATA_ADDRESS])
        self.assertEqual(image[RODATA_ADDRESS + 2:], before[RODATA_ADDRESS + 2:])
        self.assertEqual(report['status'], 'pass')
        self.assertEqual(report['patched_main_sha256'], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(report['retail_main_sha256'], hashlib.sha256(retail).hexdigest())
        self.assertEqual(report['build_id'], BUILD_ID.hex())
        self.assertEqual(report['patches'][0]['edits'][0], dict(address='0x40', size=4))
        self.assertIsNone(report['rewritten_main_sha256'])

    def test_pchtxt_stops_at_stop_and_needs_enabled(self):
        self.retail()
        text = f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x100\n@enabled\n40 aabbccdd\n@stop\n44 eeff\n'
        self.patch(text)
        self.apply()
        image = image_of(self.output.read_bytes())
        self.assertEqual(image[0x40:0x44], b'\xaa\xbb\xcc\xdd')
        self.assertEqual(image[0x44:0x46], TEXT[0x44:0x46])

    def test_pchtxt_disabled_section_is_not_applied(self):
        self.retail()
        text = f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x100\n@enabled\n40 aabbccdd\n@disabled\n44 eeff\n@stop\n'
        self.patch(text)
        self.apply()
        image = image_of(self.output.read_bytes())
        self.assertEqual(image[0x40:0x44], b'\xaa\xbb\xcc\xdd')
        self.assertEqual(image[0x44:0x46], TEXT[0x44:0x46])     # the section after @disabled is left alone

    def test_pchtxt_enabled_after_disabled_applies_again(self):
        self.retail()
        text = (f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x100\n@enabled\n40 aabbccdd\n@disabled\n44 eeff\n@enabled\n4a 0102\n@stop\n')
        self.patch(text)
        self.apply()
        image = image_of(self.output.read_bytes())
        self.assertEqual(image[0x40:0x44], b'\xaa\xbb\xcc\xdd')
        self.assertEqual(image[0x44:0x46], TEXT[0x44:0x46])
        self.assertEqual(image[0x4a:0x4c], b'\x01\x02')

    def test_pchtxt_offset_shift_of_the_nso_header_is_accepted(self):
        self.retail()
        text = f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x100\n@enabled\n40 aabbccdd\n@stop\n'
        self.patch(text)
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    def test_pchtxt_print_values_flag_is_accepted(self):
        # the header community pchtxt files ship with (the 20/30/60 fps and quality patches all start this way)
        self.retail()
        text = (f'@nsobid-{BUILD_ID.hex()}\n\n// a comment\n\n@flag print_values\n@flag offset_shift 0x100\n\n'
                f'// a section\n@enabled\n40 aabbccdd\n@stop\n')
        self.patch(text)
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    def test_pchtxt_without_offset_shift_is_refused(self):
        self.retail()
        self.patch(f'@nsobid-{BUILD_ID.hex()}\n@flag print_values\n@enabled\n40 aabbccdd\n@stop\n')
        with self.assertRaisesRegex(mod_exefs.ModError, r'no "@flag offset_shift 0x100" before the first patch line'):
            self.apply()

    def test_pchtxt_another_offset_shift_is_refused(self):
        self.retail()
        text = f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x0\n@enabled\n40 aabbccdd\n@stop\n'
        self.patch(text)
        with self.assertRaisesRegex(mod_exefs.ModError, r'@flag offset_shift 0x0 is not supported'):
            self.apply()

    def test_pchtxt_other_flag_is_refused(self):
        self.retail()
        self.patch(f'@nsobid-{BUILD_ID.hex()}\n@flag some_flag 1\n@enabled\n40 aabbccdd\n@stop\n')
        with self.assertRaisesRegex(mod_exefs.ModError, '@flag some_flag is not supported'):
            self.apply()

    def test_pchtxt_for_another_build_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')], build_id='00' * 16))
        with self.assertRaisesRegex(mod_exefs.ModError, 'another build of main'):
            self.apply()

    def test_pchtxt_without_nsobid_is_refused(self):
        self.retail()
        self.patch('@enabled\n40 aabbccdd\n@stop\n')
        with self.assertRaisesRegex(mod_exefs.ModError, '@nsobid'):
            self.apply()

    def test_pchtxt_accepts_the_20_byte_build_id(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')], build_id=BUILD_ID.hex() + '00000000'))
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    def test_pchtxt_malformed_line_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')]).replace(f'40 aa', '40 zz'))
        with self.assertRaisesRegex(mod_exefs.ModError, 'main address'):
            self.apply()

    # ---- IPS ----
    def test_ips32_patch_counts_the_nso_header(self):
        self.retail()
        self.patch(ips([(0x100 + 0x40, b'\x11\x22\x33\x44')], magic=b'IPS32', offset_size=4, end=b'EEOF'),
                   name='main.ips32')
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\x11\x22\x33\x44')

    def test_ips32_with_a_padding_byte_is_read(self):
        self.retail()
        self.patch(ips([(0x100 + 0x40, b'\x11\x22')], magic=b'IPS32', offset_size=4, end=b'EEOF', pad=b'\0'),
                   name='main.ips32')
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x42], b'\x11\x22')

    def test_ips_rle_record(self):
        self.retail()
        self.patch(ips_rle(0x100 + 0x40, 4, 0x5a), name='main.ips')
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'ZZZZ')

    def test_ips_offset_inside_the_header_is_refused(self):
        self.retail()
        self.patch(ips([(0x40, b'\x11')]), name='main.ips')
        with self.assertRaisesRegex(mod_exefs.ModError, 'inside the NSO header'):
            self.apply()

    def test_ips_that_is_not_a_patch_is_refused(self):
        self.retail()
        self.patch(b'nonsense', name='main.ips')
        with self.assertRaisesRegex(mod_exefs.ModError, 'not an IPS patch'):
            self.apply()

    def test_ips_named_by_the_build_id_is_a_main_patch(self):
        self.retail()
        self.patch(ips([(0x100 + 0x40, b'\x11\x22')], magic=b'IPS32', offset_size=4, end=b'EEOF'),
                   name=BUILD_ID.hex().upper() + '.ips')        # how Atmosphere and the mod managers name it
        report = self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x42], b'\x11\x22')
        self.assertEqual(report['patches'][0]['file'], f'exefs/{BUILD_ID.hex().upper()}.ips')

    def test_a_patch_for_another_module_is_noted_and_not_merged(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        self.patch(ips([(0x100 + 0x50, b'\x11')], magic=b'IPS32', offset_size=4, end=b'EEOF'), name='sdk.ips')
        report = self.apply()
        self.assertEqual(len(report['patches']), 1)                                  # only main.pchtxt was merged
        self.assertIn('sdk.ips', ' '.join(report['notes']))
        self.assertEqual(image_of(self.output.read_bytes())[0x50:0x51], TEXT[0x50:0x51])

    def test_an_ips_named_after_another_build_is_not_merged(self):
        self.retail()
        self.patch(ips([(0x100 + 0x40, b'\x11\x22')], magic=b'IPS32', offset_size=4, end=b'EEOF'),
                   name='11' * 16 + '.ips')
        report = self.apply()                       # a note, not a failure: the mod's romfs/ still goes in
        self.assertEqual(report['status'], 'skipped')
        self.assertIn('another version of main', ' '.join(report['notes']))
        self.assertFalse(self.output.exists())

    def test_an_ips_without_a_module_name_is_not_merged(self):
        self.retail()
        self.patch(ips([(0x100 + 0x40, b'\x11\x22')], magic=b'IPS32', offset_size=4, end=b'EEOF'), name='patch.ips')
        report = self.apply()
        self.assertEqual(report['status'], 'skipped')
        self.assertIn('rename this one', ' '.join(report['notes']))

    def test_a_pchtxt_is_read_whatever_its_name(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]), name='TOTK-Ultrawide.pchtxt')
        self.apply()
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    # ---- checks ----
    def test_a_reserved_site_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        site = patch_sites.Site('test site', 0x40, 4)
        with self.assertRaisesRegex(mod_exefs.ModError, 'overlaps a site'):
            self.apply(sites=[site])

    def test_the_20_byte_build_id_still_finds_the_rewriters_cuts(self):
        # The build passes the 20 bytes at 0x40 while patch_nso.py's tables are keyed on the first 16: the report's
        # count must include the CUTS/OBS/PARAMS sites, or a mod patch on one is only stopped later by the rewriter.
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        report = self.apply(version='v0')
        self.assertEqual(report['build_id'], BUILD_ID.hex())                # the report keeps the 16-byte id
        self.assertEqual(report['reserved_sites'], len(patch_sites.sites('v0', BUILD_ID.hex())))
        self.assertGreater(report['reserved_sites'], len(patch_sites.sites('v0')))

    def test_an_edit_outside_the_segments_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x800, b'\xaa')]))          # the padding after text
        with self.assertRaisesRegex(mod_exefs.ModError, 'padding'):
            self.apply()
        self.patch(pchtxt([(0x9000, b'\xaa')]))         # past the image
        with self.assertRaisesRegex(mod_exefs.ModError, 'outside main'):
            self.apply()

    def test_two_patches_touching_the_same_bytes_are_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        self.patch(ips([(0x100 + 0x42, b'\x01\x02')], magic=b'IPS32', offset_size=4, end=b'EEOF'),
                   name='main.ips32')
        with self.assertRaisesRegex(mod_exefs.ModError, 'both change'):
            self.apply()

    def test_a_patch_that_changes_nothing_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, TEXT[0x40:0x44])]))
        with self.assertRaisesRegex(mod_exefs.ModError, 'change no byte'):
            self.apply()

    def test_an_extra_code_module_is_skipped_with_a_note(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')]))
        (self.mod/'exefs/subsdk9').write_bytes(b'NSO0')
        report = self.apply()
        notes = ' '.join(report['notes'])
        self.assertIn('exefs/subsdk9: not loaded', notes)
        self.assertIn('never runs', notes)
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x41], b'\xaa')     # the patch still merges

    def test_a_whole_replacement_module_is_refused(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')]))
        (self.mod/'exefs/main').write_bytes(nso_bytes())
        with self.assertRaisesRegex(mod_exefs.ModError, 'whole replacement module'):
            self.apply()

    def test_an_npdm_is_only_noted(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')]))
        (self.mod/'exefs/main.npdm').write_bytes(b'META')
        (self.mod/'romfs/System').mkdir(parents=True)
        report = self.apply()
        notes = ' '.join(report['notes'])
        self.assertIn('main.npdm', notes)
        self.assertIn('romfs/', notes)

    def test_a_mod_folder_without_exefs_is_refused(self):
        self.retail()
        empty = self.root/'OtherMod'
        empty.mkdir()
        with self.assertRaisesRegex(mod_exefs.ModError, 'no exefs/'):
            mod_exefs.apply(argparse.Namespace(exefs=self.exefs, mod=empty, version='v121',
                                              output=self.output, report=self.report))

    def test_a_pchtxt_that_applies_nothing_is_a_note_not_a_failure(self):
        # every section @disabled: the file is kept around but switched off, which must not stop a build that also
        # carries the mod's romfs/
        self.retail()
        self.patch('@nsobid-' + BUILD_ID.hex() + '\n@disabled\n40 aabbccdd\n@stop\n')
        report = self.apply()
        self.assertEqual(report['status'], 'skipped')
        self.assertIn('every section is @disabled', ' '.join(report['notes']))
        self.assertFalse(self.output.exists())

    def test_an_exefs_without_a_main_patch_merges_nothing(self):
        # A mod whose exefs/ holds only a module the app does not load (or a main.npdm) is not a build failure: its
        # romfs/ half still goes in, and the report says nothing was merged.
        self.retail()
        report = self.apply()
        self.assertEqual(report['status'], 'skipped')
        self.assertFalse(self.output.exists())          # no merged module: the app is built from the retail main
        self.assertIn('no code patch for main', ' '.join(report['notes']))
        self.assertEqual(json.loads(self.report.read_text())['status'], 'skipped')

    def test_an_exefs_with_only_unloaded_files_is_skipped_with_a_note_for_each(self):
        self.retail()
        self.patch(ips([(0x40, b'\xaa')]), name='subsdk9.ips')
        (self.mod/'exefs'/'main.npdm').write_bytes(b'nx-deadbeef')
        report = self.apply()
        self.assertEqual(report['status'], 'skipped')
        notes = ' '.join(report['notes'])
        self.assertIn('subsdk9.ips: not merged (a patch for subsdk9', notes)
        self.assertIn('main.npdm: not merged', notes)
        self.assertIn('no code patch for main', notes)

    def test_a_pchtxt_for_another_build_is_still_refused(self):
        # the skip above is for "nothing to merge", never for a patch that was made for another main
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')], build_id='00' * 16))
        with self.assertRaisesRegex(mod_exefs.ModError, 'another build of main'):
            self.apply()

    def test_a_retail_main_with_a_broken_segment_digest_is_refused(self):
        raw = bytearray(nso_bytes())
        raw[0xa0] ^= 0xff                                # the header's own digest of the text segment
        (self.exefs/'main').write_bytes(bytes(raw))
        self.patch(pchtxt([(0x40, b'\xaa')]))
        with self.assertRaisesRegex(mod_exefs.ModError, 'own SHA-256'):
            self.apply()

    def test_the_merged_module_is_a_normal_nso(self):
        retail = self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        self.apply()
        merged = self.output.read_bytes()
        self.assertEqual(merged[:4], b'NSO0')
        self.assertEqual(struct.unpack_from('<I', retail, 0x0c)[0] & 0b111, 0)     # nothing was compressed to begin with
        self.assertEqual(struct.unpack_from('<I', merged, 0x0c)[0] & 0b111, 0)
        self.assertEqual(merged[0x40:0x50], BUILD_ID)
        segments = [struct.unpack_from('<IIII', merged, 0x10 + 16*i) for i in range(3)]
        self.assertEqual([s[1] for s in segments], [0, RODATA_ADDRESS, DATA_ADDRESS])   # addresses unchanged
        self.assertEqual([s[2] for s in segments], [TEXT_SIZE, RODATA_SIZE, DATA_SIZE])
        for index, (file_offset, _, size, _) in enumerate(segments):
            self.assertEqual(hashlib.sha256(merged[file_offset:file_offset + size]).digest(),
                             merged[0xa0 + 32*index:0xc0 + 32*index])

    @unittest.skipUnless(lz4, 'the lz4 module (games/totk/requirements.txt) is needed for a compressed module')
    def test_a_compressed_module_is_read_and_written_uncompressed(self):
        self.retail(compress=True)
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        report = self.apply()
        self.assertEqual(report['compressions'], 3)
        merged = self.output.read_bytes()
        self.assertEqual(struct.unpack_from('<I', merged, 0x0c)[0] & 0b111, 0)
        self.assertEqual(image_of(merged)[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    # ---- command line ----
    def test_the_command_line_applies_a_patch(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xde\xad\xbe\xef')]))
        done = subprocess.run([sys.executable, str(SCRIPTS/'mod_exefs.py'), 'apply', '--exefs', str(self.exefs),
                               '--mod', str(self.mod), '--version', 'v121', '--output', str(self.output)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('1 edits, 4 bytes', done.stdout)
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xde\xad\xbe\xef')
        self.assertTrue((self.output.parent/'mod-exefs.json').is_file())

    def test_the_command_line_reports_a_refusal(self):
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa')], build_id='11' * 16))
        done = subprocess.run([sys.executable, str(SCRIPTS/'mod_exefs.py'), 'apply', '--exefs', str(self.exefs),
                               '--mod', str(self.mod), '--version', 'v121', '--output', str(self.output)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 1)
        self.assertIn('another build of main', done.stderr)
        self.assertFalse(self.output.exists())

    def test_botw190_takes_the_same_patch_and_reserves_its_own_sites(self):
        # Breath of the Wild 1.9.0 (docs/mods.md): the build id of the synthetic module is not the game's, so only the
        # version's own reserved list (game_patches_botw.c) is in play; main+0x40 is no site of it.
        self.retail()
        self.patch(pchtxt([(0x40, b'\xaa\xbb\xcc\xdd')]))
        report = self.apply(version='botw190')
        self.assertEqual(report['version'], 'botw190')
        self.assertEqual(report['reserved_sites'], len(patch_sites.sites('botw190', BUILD_ID.hex())))
        self.assertEqual(image_of(self.output.read_bytes())[0x40:0x44], b'\xaa\xbb\xcc\xdd')

    def test_botw190_refuses_a_site_the_runtime_checks(self):
        # 0x151d484 is the first word of the real-time frame step (game_patches_botw.c patch_realtime): the synthetic
        # module is too small to hold it, so the reserved list is asked directly the way apply() asks it.
        reserved = patch_sites.sites('botw190', BUILD_ID.hex())
        hits = patch_sites.overlaps([(0x151d484, 4)], reserved)
        self.assertTrue(hits)
        self.assertIn('game_patches_botw.c', hits[0][2].label)

    def test_the_sites_command_lists_a_version(self):
        done = subprocess.run([sys.executable, str(SCRIPTS/'mod_exefs.py'), 'sites', '--version', 'v121'],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('0x008f273c', done.stdout)


if __name__ == '__main__':
    unittest.main()

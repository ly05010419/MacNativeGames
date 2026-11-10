"""Extracted-input negative controls, using synthetic data only."""
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(GAME/'scripts'))
import inspect_inputs as inputs


class InputTests(unittest.TestCase):
    def test_nso_segment_hash_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'main'; b=bytearray(256+12); b[:4]=b'NSO0'
            struct.pack_into('<I',b,12,0x38)
            for i in range(3):
                struct.pack_into('<III',b,0x10+16*i,256+4*i,4*i,4)
                struct.pack_into('<I',b,0x60+4*i,4)
                b[256+4*i:260+4*i]=bytes([i])*4
                b[0xa0+32*i:0xc0+32*i]=hashlib.sha256(bytes([i])*4).digest()
            p.write_bytes(b)
            self.assertTrue(all(s['header_hash_verified'] for s in inputs.nso(p)['segments']))
            b[256]^=1;p.write_bytes(b)
            with self.assertRaisesRegex(ValueError,'NSO segment hash mismatch'): inputs.nso(p)
            p.write_bytes(b[:256])
            with self.assertRaisesRegex(ValueError,'truncated NSO segment'): inputs.nso(p)

    def test_version_comes_from_main(self):
        self.assertEqual(inputs.version_of(inputs.VERSIONS['1.4.1']['main'] + '00'*16)['tag'],'v458752')
        self.assertEqual(inputs.version_of(inputs.VERSIONS['1.3.0']['main'])['tag'],'v327680')
        with self.assertRaisesRegex(ValueError,'unsupported executable build'): inputs.version_of('00'*16)

    def test_each_version_has_its_pins(self):
        for version in inputs.VERSIONS.values():
            pins=json.loads(inputs.pins_of(version).read_text())
            self.assertEqual((pins['version'],pins['version_code']),(version['name'],version['code']))
            self.assertTrue(pins['modules']['main']['build_id'].startswith(version['main']))
            for name,bid in inputs.COMMON.items(): self.assertTrue(pins['modules'][name]['build_id'].startswith(bid))

    def test_npdm_title_memory_and_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'main.npdm';b=bytearray(256);b[:4]=b'META'
            struct.pack_into('<II',b,0x70,128,128);b[128:132]=b'ACI0'
            struct.pack_into('<Q',b,144,int(inputs.TITLE,16));p.write_bytes(b)
            self.assertEqual(inputs.npdm(p)['sys_resource'],0)
            struct.pack_into('<I',b,0x14,0x1000000);p.write_bytes(b)
            with self.assertRaisesRegex(ValueError,'system resource'): inputs.npdm(p)
            struct.pack_into('<I',b,0x14,0);struct.pack_into('<Q',b,144,1);p.write_bytes(b)
            with self.assertRaisesRegex(ValueError,'program ID'): inputs.npdm(p)
            struct.pack_into('<I',b,0x70,250);p.write_bytes(b)
            with self.assertRaisesRegex(ValueError,'ACI0 range'): inputs.npdm(p)

    def test_tree_identity_includes_bytes_and_rejects_links(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);p=root/'data';p.write_bytes(b'one')
            a=inputs.tree_manifest(root);p.write_bytes(b'two')
            self.assertNotEqual(a['sha256'],inputs.tree_manifest(root)['sha256'])
            (root/'link').symlink_to(p)
            with self.assertRaisesRegex(ValueError,'symlink'): inputs.tree_manifest(root)
            with self.assertRaisesRegex(ValueError,'directory tree'): inputs.tree_manifest(p)

    def test_dlc_is_pinned_and_only_for_141(self):
        from unittest import mock
        v141,v130=inputs.VERSIONS['1.4.1'],inputs.VERSIONS['1.3.0']
        with tempfile.TemporaryDirectory() as tmp:
            game=Path(tmp)/'game';(game/'assets').mkdir(parents=True)
            dlc=Path(tmp)/'dlc';(dlc/'2').mkdir(parents=True);(dlc/'2/DLCDATA.dat').write_bytes(b'\2\0\0\0')
            tree={k:v for k,v in inputs.tree_manifest(dlc/'2').items() if k!='entries'}
            (game/'assets/expected-dlc.json').write_text(json.dumps({'2':tree}))
            with mock.patch.object(inputs,'GAME',game):
                self.assertEqual(inputs.inspect_dlc(dlc,v141),{'2':tree})
                with self.assertRaisesRegex(ValueError,'1.4.1 only'): inputs.inspect_dlc(dlc,v130)
                (dlc/'2/DLCDATA.dat').write_bytes(b'\3\0\0\0')
                with self.assertRaisesRegex(ValueError,'DLC hash mismatch: 2'): inputs.inspect_dlc(dlc,v141)
                (dlc/'2').rename(dlc/'9')
                with self.assertRaisesRegex(ValueError,'unsupported DLC folder: 9'): inputs.inspect_dlc(dlc,v141)
                (dlc/'9/DLCDATA.dat').unlink();(dlc/'9').rmdir()
                with self.assertRaisesRegex(ValueError,'numbered folder'): inputs.inspect_dlc(dlc,v141)

    def test_dlc_pins_cover_the_four_add_ons(self):
        pins=json.loads((GAME/'assets/expected-dlc.json').read_text())
        self.assertEqual(sorted(pins),['1','2','3','4'])

    def test_unknown_module_build_rejected_before_romfs_read(self):
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'ExeFS').mkdir()
            for n in ('main','subsdk0','sdk','rtld','main.npdm'):(root/'ExeFS'/n).touch()
            with mock.patch.object(inputs,'nso',return_value={'build_id':'0'*64}), mock.patch.object(inputs,'tree_manifest') as tree:
                with self.assertRaisesRegex(ValueError,'unsupported executable build'): inputs.inspect(root)
                tree.assert_not_called()

    def test_rewriter_asserts_original_game_patch_bytes(self):
        import lz4.block
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);b=bytearray(256);b[:4]=b'NSO0'
            b[0x40:0x50]=bytes.fromhex(inputs.BUILDS['main'])
            struct.pack_into('<I',b,12,1)
            text=bytes(0x8d9620);packed=lz4.block.compress(text,store_size=False)
            struct.pack_into('<III',b,0x10,256,0,len(text))
            for i in (1,2):struct.pack_into('<III',b,0x10+16*i,256+len(packed),len(text),0)
            struct.pack_into('<III',b,0x60,len(packed),0,0)
            p=root/'main';p.write_bytes(b+packed)
            result=subprocess.run([sys.executable,str(GAME/'runtime/tools/patch_nso.py'),str(p),str(root),'main'],capture_output=True,text=True,env={k:v for k,v in os.environ.items() if k not in ('DM_GAMES_DIR','DM_GAMES_FIRST')})
            self.assertNotEqual(result.returncode,0)
            self.assertIn('cut original bytes mismatch 0x8d9610',result.stderr)
            self.assertFalse((root/'main.patched.bin').exists())


if __name__=='__main__':unittest.main()

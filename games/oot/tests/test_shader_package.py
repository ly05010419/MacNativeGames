"""Integrity and nested-signature checks on small synthetic resource fixtures."""
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest

GAME=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('oot_package',GAME/'build.py')
package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.tree=Path(self.temp.name)
        self.seed=self.tree/'build/shader-seed/oot';self.seed.mkdir(parents=True)
        self.libs=self.tree/'build/shader-libs';self.libs.mkdir(parents=True)
        (self.tree/'build/game-data/oot').mkdir(parents=True)
        self.source=self.seed/('a'*64+'-'+'b'*12+'_b.metal')
        self.source.write_text('kernel void main0() {}\n')
        source_hash=package.sha(self.source)
        library=self.libs/(source_hash+'.metallib');library.write_bytes(b'synthetic resource; not executed')
        self.recorded_library=self.libs/('c'*64+'.metallib')
        self.recorded_library.write_bytes(b'synthetic recorded binding variant')
        chain=re.search(r'#define DMSH_CHAIN "([^"]+)"\s*/\* game: oot\b',(GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text())[1]
        romfs=self.tree/'extracted/oot/romfs';romfs.mkdir(parents=True)   # the image this tree imported names the seed (#2009)
        (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='d'*64,romfs_size=16,files=[])))
        self.metadata=dict(schema=1,version='oot',source_romfs_sha256='d'*64,
                           translator_sha256=package.sha(GAME/'prebuilt/macos-arm64/totk_xlate'),
                           chain=chain,all_default=True,nvn_depth_mode='minus-w',corpus_shaders=1,
                           translated_default=1,shaders=1,files={self.source.name:source_hash})
        (self.seed/'seed-manifest.json').write_text(json.dumps(self.metadata))
        self.library_metadata=dict(schema=1,complete=True,compiler='synthetic fixture -fpreserve-invariance',
                                   sources={self.source.name:source_hash},
                                   files={library.name:package.sha(library),self.recorded_library.name:package.sha(self.recorded_library)})
        (self.libs/'libraries-manifest.json').write_text(json.dumps(self.library_metadata))

    def test_changed_bytes_invalidate_identity_receipt(self):
        package.verify_shader_resources(self.tree)
        prior=self.source.stat()
        self.source.write_text('kernel void wrong() {}\n')
        os.utime(self.source,ns=(prior.st_atime_ns,prior.st_mtime_ns))
        with self.assertRaisesRegex(ValueError,'damaged shader seed'):
            package.verify_shader_resources(self.tree)

    def test_incomplete_libraries_are_not_packaged(self):
        self.library_metadata['complete']=False
        (self.libs/'libraries-manifest.json').write_text(json.dumps(self.library_metadata))
        with self.assertRaisesRegex(ValueError,'incomplete'):
            package.verify_shader_resources(self.tree)

    def test_compiler_without_invariance_cannot_be_packaged(self):
        self.library_metadata['compiler']='synthetic fixture'
        (self.libs/'libraries-manifest.json').write_text(json.dumps(self.library_metadata))
        with self.assertRaisesRegex(ValueError,'lack position invariance'):
            package.verify_shader_resources(self.tree)

    def test_recorded_binding_library_is_also_verified(self):
        package.verify_shader_resources(self.tree)
        self.recorded_library.write_bytes(b'corrupt binding library')
        with self.assertRaisesRegex(ValueError,'damaged Metal library'):
            package.verify_shader_resources(self.tree)

    def test_seed_from_another_romfs_is_rejected(self):
        self.metadata['source_romfs_sha256']='0'*64
        (self.seed/'seed-manifest.json').write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError,'translation context'):
            package.verify_shader_resources(self.tree)

    def test_seed_from_another_translator_is_rejected(self):
        self.metadata['translator_sha256']='0'*64
        (self.seed/'seed-manifest.json').write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError,'translation context'):
            package.verify_shader_resources(self.tree)

    @unittest.skipUnless(os.uname().sysname=='Darwin','macOS code signing')
    def test_nested_bundle_reuses_signature_and_relative_links(self):
        record=package.verify_shader_resources(self.tree)
        bundle=package.shader_bundle(self.tree,record)
        signature=bundle/'Contents/_CodeSignature/CodeResources';identity=signature.stat()
        self.assertEqual(package.shader_bundle(self.tree,record),bundle)
        self.assertEqual(signature.stat().st_ino,identity.st_ino)
        parent=self.tree/'SigningFixture.bundle';resources=parent/'Contents/Resources/totk'
        package.build_helpers().place_shader_bundle(bundle,parent,resources)
        (parent/'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleIdentifier='org.macnativegames.tests.shader-parent',CFBundlePackageType='BNDL')))
        subprocess.run(['codesign','--force','--sign','-',parent],check=True,capture_output=True)
        subprocess.run(['codesign','--verify','--strict','--deep',parent],check=True,capture_output=True)
        link=resources/'build/shader-seed'
        self.assertTrue(link.is_symlink())
        self.assertFalse(os.path.isabs(os.readlink(link)))
        self.assertTrue((link/self.source.name).is_file())
        self.assertEqual((resources/'build/shader-libs'/self.recorded_library.name).read_bytes(),
                         self.recorded_library.read_bytes())


if __name__=='__main__':
    unittest.main()

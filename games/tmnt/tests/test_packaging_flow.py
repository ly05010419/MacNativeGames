"""Behavior checks for TMNT packaging, migration and Launcher update decisions."""
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / 'games/tmnt/scripts'
sys.path[:0] = [str(SCRIPTS), str(ROOT)]
from tmnt_bundle import Digests, install_app
from tmnt_pipelines import check_archive_lists, list_digest
from tmnt_shaders import shaders
from tools.builder_jobs import repo, installed, build_job


class PackagingFlow(unittest.TestCase):
    def test_digest_reuses_unchanged_file_and_invalidates_write(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp); file=folder/'seed'; file.write_bytes(b'old')
            cache=Digests(folder/'digests.json'); first=cache.sha256(file); cache.save()
            cache=Digests(folder/'digests.json')
            with mock.patch.object(hashlib,'file_digest',side_effect=AssertionError('unnecessary reread')):
                self.assertEqual(cache.sha256(file),first)
            file.write_bytes(b'new')
            self.assertNotEqual(cache.sha256(file),first)

    def test_publication_keeps_app_inode(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); old=root/'game.app'; new=root/'partial.app'
            for p,text in ((old,'old'),(new,'new')):
                (p/'Contents').mkdir(parents=True);(p/'Contents/payload').write_text(text)
            inode=old.stat().st_ino
            install_app(new,old,verify=lambda _: True,remove=lambda _: None)
            self.assertEqual(old.stat().st_ino,inode)
            self.assertEqual((old/'Contents/payload').read_text(),'new')

    def test_archive_rejects_different_list_and_corrupt_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); archive=root/'archive';archive.mkdir()
            lists=root/'build';lists.mkdir();(lists/'pso_manifest.tsv').write_text('R\tone\n')
            binary=archive/'one.metalarchive';binary.write_bytes(b'archive')
            report={'pso_manifest.tsv':list_digest(lists),'requested':1,'built':1,
                    'files':{binary.name:hashlib.sha256(binary.read_bytes()).hexdigest()}}
            (archive/'lists.json').write_text(json.dumps(report))
            check_archive_lists(archive,lists)
            binary.write_bytes(b'corrupt')
            with self.assertRaises(ValueError):check_archive_lists(archive,lists)
            binary.write_bytes(b'archive');(lists/'pso_manifest.tsv').write_text('R\ttwo\n')
            with self.assertRaises(ValueError):check_archive_lists(archive,lists)

    def test_shader_inventory_checks_both_size_fields(self):
        control=bytearray(0x880);struct.pack_into('<I',control,0,0x98761234)
        struct.pack_into('<I',control,0x20,0x880);struct.pack_into('<II',control,0x700,0x30,0x40)
        struct.pack_into('<I',control,0x714,1)
        blob=struct.pack('<I',len(control))+control+struct.pack('<I',0x40)+bytes(0x40)
        self.assertEqual(len(list(shaders(blob))),1)
        with self.assertRaises(ValueError):list(shaders(blob[:-1]))
        self.assertEqual(list(shaders(b'\0\0\0\0'+blob[4:])),[])

    def test_full_build_reuses_work_root_without_overwriting(self):
        import runpy
        entry = runpy.run_path(str(ROOT/'games/tmnt/build.py'))
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); work=root/'work'; work.mkdir()
            marker=work/'old-result'; marker.write_text('keep')
            game=root/'game.nsp'; game.touch(); keys=root/'prod.keys'; keys.touch()
            argv=['build.py',str(game),'-k',str(keys),'--work',str(work),'-o',str(root/'TMNT.app')]
            def extract(argv, **kwargs):
                name=Path(argv[1]).name
                if name=='extract_exefs.py':
                    output=Path(argv[argv.index('--output')+1]);output.mkdir()
                    (output/'main').write_text('verified fixture')
                    (output.parent/'extraction-manifest.json').write_text('{}')
                if name=='extract_romfs.py':
                    output=Path(argv[argv.index('--output')+1]);output.mkdir()
                    (output/'romfs.bin').write_text('verified fixture')
                    (output/'index.json').write_text('{}')
            for iteration in range(2):
                with mock.patch.object(sys,'argv',argv), mock.patch('subprocess.run',side_effect=extract) as run:
                    entry['main']()
                    calls=run.call_args_list
                    self.assertEqual(len(calls),3 if iteration==0 else 1)
                    args=calls[-1].args[0]
                    self.assertEqual(args[args.index('--record-work')+1],work.resolve())
            self.assertEqual(marker.read_text(),'keep')
            self.assertEqual(len(list(work.glob('full-*'))),0)
            self.assertTrue((work/'full/exefs/main').is_file())

    def test_fresh_build_stages_nro_before_external_link_check(self):
        import build_private_bundle as builder
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); exefs=root/'exefs'; exefs.mkdir()
            romfs=root/'romfs'; romfs.mkdir(); (romfs/'romfs.bin').write_bytes(b'fixture'); (romfs/'index.json').write_text('{"romfs_sha256":"test"}')
            work=root/'work'; out=root/'TMNT.app'; linked=[]
            def script(name,*args):
                if name=='rewrite_modules.py': (work/'rewritten').mkdir()
                if name in ('module_metadata.py','prepare_host_tables.py','prepare_nro_digest.py'):
                    target=Path(args[args.index('--out')+1]); target.parent.mkdir(parents=True,exist_ok=True); target.write_text('header')
                if name=='build_shader_tools.py': (work/'shader-tools').mkdir()
                if name=='prepare_resources.py':
                    folder=work/'resources'; folder.mkdir()
                    for n in ('romfs_config.h','shared_fonts_config.h'): (folder/n).write_text('header')
                if name=='prepare_nro.py':
                    folder=work/'nro-prepared';folder.mkdir();(folder/'module.nro').write_bytes(b'nro')
                if name=='rewrite_nro.py':
                    folder=work/'nro-rewritten';folder.mkdir()
                    for suffix in ('patched.bin','fixups.tsv','gates.tsv'): (folder/f'nro.{suffix}').write_text(suffix)
            def call(*args):
                if args[-1]=='link':
                    linked.append(args)
                    self.assertFalse(out.exists())
                    ident=hashlib.sha256(b'nro').hexdigest()
                    for suffix in ('patched.bin','fixups.tsv','gates.tsv'):
                        self.assertEqual((Path(args[1])/'nro'/f'{ident}.{suffix}').read_text(),suffix)
            args=Namespace(work=work,out=out,exefs=exefs,romfs=romfs,fonts=None,dotnet=None,app=True,link_romfs=False)
            with mock.patch.object(builder,'script',side_effect=script), mock.patch.object(builder,'call',side_effect=call), mock.patch.object(builder,'check_supported_romfs'), mock.patch.object(builder,'verify_exefs'), mock.patch('tmnt_work.source_identity',return_value={'tools':'fixture'}):
                builder.build(args)
            self.assertEqual(len(linked),1)

    def test_launcher_routes_tmnt(self):
        self.assertEqual(repo.game_of_title('0100FE701475A000'),'tmnt')
        self.assertIn('org.macnativegames.tmnt.private',installed.bundle_ids())
        with tempfile.TemporaryDirectory() as temp:
            request={'plan':{'game':'tmnt','base':'base.nsp','keys':'prod.keys','blocks':[]},'output_dir':temp,'work_root':str(Path(temp)/'work')}
            script,args,work,app,kind,game=build_job.command(request)
            self.assertEqual(script,ROOT/'games/tmnt/build.py')
            self.assertEqual(game,'tmnt');self.assertIn('-k',args);self.assertEqual(kind,'full')
            self.assertEqual(work,Path(temp)/'work/tmnt')

class RuntimeUpdatePlanning(unittest.TestCase):
    def test_installed_inputs_drive_plan_and_tampering_blocks_it(self):
        import update_runtime
        from tmnt_update import blocker
        from rewrite_modules import GAME, TOOL_SOURCES, sha256
        from tmnt_shaders import chain, translation_sources
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);app=root/'game.app';res=app/'Contents/Resources/game'
            meta=res/'runtime-update';meta.mkdir(parents=True)
            (res/'rewritten/nro').mkdir(parents=True)
            for module in ('main','subsdk0','subsdk1','subsdk2','nro/module'):
                for suffix in ('patched.bin','fixups.tsv','gates.tsv'):
                    (res/'rewritten'/f'{module}.{suffix}').write_text('verified bytes')
            for name in ('runtime_tables.h','romfs_config.h','shared_fonts_config.h','tmnt_nro_digest.h'):
                (meta/name).write_text('verified header')
            (meta/'rewrite-report.json').write_text(json.dumps({'tools':{n:sha256(GAME/'third_party/d2r'/n) for n in TOOL_SOURCES}}))
            hashes={str(p.relative_to(res)):sha256(p) for p in res.rglob('*') if p.is_file()}
            (meta/'inputs.json').write_text(json.dumps(hashes))
            seed=res/'build/shader-seed';seed.mkdir(parents=True)
            (seed/'seed-manifest.json').write_text(json.dumps({'chain':chain(),'translation_sources':translation_sources(),'bindings':sha256(GAME/'assets/shader_list.tsv')}))
            work=root/'recorded-work';work.mkdir()
            (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'org.macnativegames.tmnt.private','MNGWorkFolder':str(work)}))
            self.assertIsNone(blocker(app))
            plan=update_runtime.plan(app)
            self.assertEqual((plan['game'],plan['kind'],plan['work']),('tmnt','express',str(work)))
            (res/'rewritten/main.patched.bin').write_text('changed')
            self.assertEqual(update_runtime.plan(app)['kind'],'full')


if __name__ == '__main__': unittest.main()

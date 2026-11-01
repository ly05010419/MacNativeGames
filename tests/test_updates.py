"""Update import/packaging regressions. Synthetic bytes only, no game files or keys."""
import io
import json
import os
import shutil
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from test_game_containers import (HEADER, AREA, TITLEKEK, CONTENT, SCRIPTS, TITLE,
                                  encrypt, nca, partition, ticket, game_image, romfs)
from cryptography.hazmat.primitives.ciphers import modes
from extract_exefs import decrypt, common_tickets, container_entries
from extract_update_romfs import hactool_prod_keys, merge
import build_app

BOTW_TITLE = 0x01007EF00011E000


def update_nca(managed=True, title=TITLE):
    data = nca(managed, title=title)
    header = bytearray(b''.join(decrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')),
                                     data[i*512:(i+1)*512]) for i in range(6)))
    header[0x604] = 4  # BKTR marker in section 1; these tests extract only ExeFS.
    import hashlib
    header[0x2a0:0x2c0] = hashlib.sha256(header[0x600:0x800]).digest()
    return b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), header[i*512:(i+1)*512])
                    for i in range(6)) + data[0xc00:]


def fake_venv(tree):
    """A `.venv` whose python is this interpreter: build_app runs `<tree>/.venv/bin/python` for its scripts. Homebrew's
    sys.prefix has no bin/python, so linking the prefix (as the tests did) only works on python.org installs. A script that
    execs this interpreter by its own path, not a symlink: a venv's python finds its pyvenv.cfg (and so its packages,
    cryptography among them) from the path it is started by."""
    (tree/'.venv'/'bin').mkdir(parents=True, exist_ok=True)
    (tree/'.venv'/'bin'/'python').write_text(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
    (tree/'.venv'/'bin'/'python').chmod(0o755)
    (tree/'.venv'/'bin'/'nsz').write_text('#!/bin/sh\nexit 0\n')   # build_app.py runs .venv/bin/nsz for NSZ sources
    (tree/'.venv'/'bin'/'nsz').chmod(0o755)


def fake_data_bundle(tree, version, _logs, **_names):
    """build_app.data_bundle without the 16 GB image or codesign: an empty bundle folder."""
    bundle = tree / f'build/data-bundles/GameData-{version}.bundle'
    (bundle / 'Contents/Resources').mkdir(parents=True, exist_ok=True)
    (bundle / 'Contents/Resources/romfs.bin').write_bytes(b'image')
    return bundle


def game_data(tree, version):
    """The game data folder step_app copies into the app (build_app.place_game_data)."""
    for name in build_app.game_data_files(version):
        p=tree/f'build/game-data/{version}'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'test')


class Updates(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.keys = self.root/'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in
                            [('header_key', HEADER), ('key_area_key_application_00', AREA), ('titlekek_00', TITLEKEK)]))
        self.update = self.root/'update.nsp'
        self.update.write_bytes(partition([('program.nca', update_nca()), ('common.tik', ticket())]))

    def test_real_ticket_signature_byte_order(self):
        self.assertEqual(ticket()[:4], b'\x04\x00\x01\x00')
        raw = partition([('ticket.tik', ticket())])
        self.assertEqual(len(common_tickets(io.BytesIO(raw), container_entries(io.BytesIO(raw), len(raw))[1])), 1)
        bad = bytearray(ticket()); bad[:4] = b'\x00\x01\x00\x04'
        raw = partition([('ticket.tik', bad)])
        with self.assertRaisesRegex(ValueError, 'signature type'):
            common_tickets(io.BytesIO(raw), container_entries(io.BytesIO(raw), len(raw))[1])

    def test_update_exefs_and_base_rejection(self):
        out = self.root/'exefs'
        result = subprocess.run([sys.executable, SCRIPTS/'extract_exefs.py', self.update, '--update',
                                 '--keys', self.keys, '--output', out], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((out/'main').read_bytes(), b'synthetic main')
        base = self.root/'base.nsp'; base.write_bytes(game_image('nsp'))
        result = subprocess.run([sys.executable, SCRIPTS/'extract_exefs.py', base, '--update',
                                 '--keys', self.keys, '--output', self.root/'bad'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Expected a BKTR update', result.stderr)

    def test_wrong_version_stops_before_rewrite(self):
        tree = self.root/'tree'; tree.mkdir()
        (tree/'scripts').symlink_to(SCRIPTS, target_is_directory=True)
        fake_venv(tree)
        with patch.dict(build_app.UPDATED, {'exefs_sha256': {'main': build_app.UPDATED['exefs_sha256']['main']}}), self.assertRaisesRegex(build_app.BuildError, 'ExeFS main'):
            build_app.step_code(tree, self.update, self.keys, os.environ, self.root/'logs', version='v112')
        self.assertFalse((tree/'rewritten').exists())

    def test_botw190_update_is_extracted_by_the_botw_script_and_checked_against_its_pins(self):
        tree = self.root/'tree'; tree.mkdir()
        (tree/'scripts').symlink_to(SCRIPTS, target_is_directory=True)
        (self.root/'botw').mkdir(); (self.root/'botw/scripts').symlink_to(SCRIPTS.parents[1]/'botw/scripts', target_is_directory=True)
        fake_venv(tree)
        botw = self.root/'botw-update.nsp'
        botw.write_bytes(partition([('program.nca', update_nca(title=BOTW_TITLE)), ('common.tik', ticket())]))
        pins = {'exefs_sha256': {'main': build_app.BOTW190['exefs_sha256']['main']}, 'display_version': '1.9.0',
                'title_id': build_app.BOTW190['title_id']}
        with patch.dict(build_app.VERSIONS, {'botw190': pins}):
            # the synthetic ExeFS is extracted by games/botw/scripts/extract_exefs.py (#117) and then fails its pin
            with self.assertRaisesRegex(build_app.BuildError, 'ExeFS main'):
                build_app.step_code_botw(tree, self.root/'botw', botw, self.keys, os.environ, self.root/'logs', version='botw190')
            self.assertEqual((tree/'extracted/botw190/exefs/main').read_bytes(), b'synthetic main')
            # a Tears of the Kingdom update is another game for it
            shutil.rmtree(tree/'extracted')
            with self.assertRaisesRegex(build_app.BuildError, 'extract_exefs.py failed'):
                build_app.step_code_botw(tree, self.root/'botw', self.update, self.keys, os.environ, self.root/'logs', version='botw190')
        self.assertIn('Expected one update Program NCA of 01007EF00011E000', (self.root/'logs/extract_exefs-botw190.log').read_text())
        self.assertFalse((tree/'rewritten').exists())

    def test_nsz_conversion_is_cached_by_source_identity(self):
        tree = self.root/'tree'; tree.mkdir()
        source = self.root/'update.nsz'; source.write_bytes(b'NSZ test input')
        logs = self.root/'logs'
        calls = []
        def fake_run(cmd, log, cwd=None, env=None, quiet=True):
            calls.append(cmd)
            out = Path(cmd[cmd.index('-o') + 1]) / f'{source.stem}.nsp'
            out.write_bytes(b'verified NSP payload')
        with patch.object(build_app, 'run', side_effect=fake_run):
            first = build_app.prepare_update_container(tree, source, self.keys, logs)
            second = build_app.prepare_update_container(tree, source, self.keys, logs)
        self.assertEqual(first, second)
        self.assertEqual(first.read_bytes(), b'verified NSP payload')
        self.assertEqual(len(calls), 1)
        self.assertIn('--keys', calls[0])

    def test_v143_identified_by_content_and_cached(self):
        import hashlib
        tree = self.root/'tree'; tree.mkdir()
        (tree/'scripts').symlink_to(SCRIPTS, target_is_directory=True)
        fake_venv(tree)
        # Deliberately misleading filename: selection must use the module bytes.
        source = self.root/'1.1.2.nsp'; source.write_bytes(self.update.read_bytes())
        expected = {name: hashlib.sha256(data).hexdigest() for name, data in (('main', b'synthetic main'), ('main.npdm', b'synthetic metadata'))}
        with patch.dict(build_app.UPDATED143, {'exefs_sha256': expected}):
            version, normalized = build_app.identify_update(tree, source, self.keys, self.root/'logs')
            self.assertEqual(version, 'v143')
            self.assertEqual(normalized, source)
            self.assertEqual((tree/'extracted/v143/exefs/main').read_bytes(), b'synthetic main')
            with patch.object(build_app, 'run', side_effect=AssertionError('cache must avoid extraction')):
                self.assertEqual(build_app.identify_update(tree, source, self.keys, self.root/'logs'), (version, source))
        self.assertFalse((tree/'rewritten').exists())

    def test_lockpick_kek_source_nul_is_stripped_for_hactool(self):
        body = '11' * 16
        original = self.keys.read_text()
        if not original.endswith('\n'):
            original += '\n'
        extra = (f'master_kek_source_06 = {body}00\n'
                 f'mariko_master_kek_source_05 = {body.upper()}00 # kept\n'
                 '# a comment line\n'
                 f'titlekek_01 = {"33" * 16}   # hactool would read this note as part of the value\n')
        self.keys.write_text(original + extra)
        out = self.root/'hactool.keys'
        hactool_prod_keys(self.keys, out)
        self.assertEqual(os.stat(out).st_mode & 0o777, 0o600)
        lines = out.read_text().splitlines()
        self.assertIn(f'master_kek_source_06 = {body}', lines)
        self.assertIn(f'mariko_master_kek_source_05 = {body.upper()}', lines)
        self.assertIn(f'titlekek_01 = {"33" * 16}', lines)
        self.assertFalse([line for line in lines if '#' in line])
        self.assertEqual(len(lines), len([line for line in original.splitlines() if '=' in line]) + 3)
        self.assertEqual(self.keys.read_text(), original + extra)
        bad = self.root/'bad.keys'
        bad.write_text(f'master_kek_source_06 = {body}ff\n')
        with self.assertRaisesRegex(ValueError, 'master_kek_source_06 must be 16 bytes') as caught:
            hactool_prod_keys(bad, self.root/'rejected.keys')
        self.assertNotIn(body, str(caught.exception))
        self.assertFalse((self.root/'rejected.keys').exists())

    def test_merge_passes_stripped_kek_sources_only(self):
        space = patch('extract_update_romfs.shutil.disk_usage')
        space.start().return_value.free = 100 * 2**30
        self.addCleanup(space.stop)
        body = '22' * 16
        original = self.keys.read_text()
        if not original.endswith('\n'):
            original += '\n'
        self.keys.write_text(original + f'master_kek_source_0a = {body}00\n')
        before = self.keys.read_bytes()
        helper = self.root/'hactool'
        helper.write_text('#!'+sys.executable+'\n'+
            'import os,sys,pathlib,stat\n'
            f'body={body!r}\n'
            'path=pathlib.Path(sys.argv[sys.argv.index("-k")+1])\n'
            'assert stat.S_IMODE(path.stat().st_mode)==0o600\n'
            'found=[line.split("=",1)[1].strip() for line in path.read_text().splitlines() if line.startswith("master_kek_source_0a")]\n'
            'assert found==[body]\n'
            'out=pathlib.Path(next(s.split("=",1)[1] for s in sys.argv if s.startswith("--section")))\n'
            f'out.write_bytes({romfs()!r})\n'
            'for i in range(6): print(f"Level {i} (GOOD)")\n')
        helper.chmod(0o755)
        base = self.root/'base.nsp'; base.write_bytes(game_image('nsp', managed=True))
        merge(base, self.update, self.keys, helper, self.root/'merged')
        self.assertEqual(self.keys.read_bytes(), before)
        self.assertFalse(list(self.root.glob('update-merge-*')))

    def test_merge_ticket_privacy_and_failure_cleanup(self):
        # A mock hactool verifies plumbing, not BKTR cryptography (covered by the real local build).
        space = patch('extract_update_romfs.shutil.disk_usage')
        space.start().return_value.free = 100 * 2**30
        self.addCleanup(space.stop)
        helper = self.root/'hactool'
        helper.write_text('#!'+sys.executable+'\n'+
            'import os,sys,pathlib,stat\n'
            'key=pathlib.Path(os.environ["MNG_TITLE_KEYS"])\n'
            'assert stat.S_IMODE(key.stat().st_mode)==0o600\n'
            'assert key.read_text() and not any("--titlekey=" in s for s in sys.argv)\n'
            'assert len(list(key.parent.glob("*.nca")))==1\n'
            'base=pathlib.Path(next(s.split("=",1)[1] for s in sys.argv if s.startswith("--basenca=")))\n'
            'offset=int(os.environ["MNG_BASE_RANGE_OFFSET"]); size=int(os.environ["MNG_BASE_RANGE_SIZE"])\n'
            'assert base.suffix in (".xci",".nsp") and offset>=0 and size>0 and offset+size<=base.stat().st_size\n'
            'if os.environ.get("MNG_MOCK_FAIL"): print("Key ("+"ab"*17+") must be 32 hex digits!"); raise SystemExit(1)\n'
            'out=pathlib.Path(next(s.split("=",1)[1] for s in sys.argv if s.startswith("--section")))\n'
            f'out.write_bytes({romfs()!r})\n'
            'for i in range(6): print(f"Level {i} (GOOD)")\n')
        helper.chmod(0o755)
        for kind in ('xci', 'nsp'):
            base = self.root/f'base.{kind}';base.write_bytes(game_image(kind, managed=kind=='nsp'))
            out = self.root/f'merged-{kind}'
            merge(base, self.update, self.keys, helper, out)
            self.assertEqual((out/'romfs.bin').read_bytes(), romfs())
            self.assertFalse(list(self.root.glob('update-merge-*')))
        with patch.dict(os.environ, MNG_MOCK_FAIL='1'), self.assertRaisesRegex(ValueError, 'BKTR verification failed'):
            merge(base, self.update, self.keys, helper, self.root/'failed')
        self.assertFalse((self.root/'failed').exists())
        failure = (self.root/'failed-failure.log').read_text()
        self.assertIn('must be 32 hex digits', failure)
        self.assertNotIn('ab' * 17, failure)
        self.assertFalse(list(self.root.glob('update-merge-*')))

    def test_merge_accepts_hactool_signature_checks_it_cannot_perform(self):
        # Real hactool prints the NCA fixed-key and NPDM signature checks as (FAIL) on a successful merge:
        # it has no Nintendo public key. The merge must judge the data, not those lines.
        space = patch('extract_update_romfs.shutil.disk_usage')
        space.start().return_value.free = 100 * 2**30
        self.addCleanup(space.stop)
        helper = self.root/'hactool'
        helper.write_text('#!'+sys.executable+'\n'+
            'import sys,pathlib\n'+
            'out=pathlib.Path(next(s.split("=",1)[1] for s in sys.argv if s.startswith("--section")))\n'
            f'out.write_bytes({romfs()!r})\n'
            'print("Fixed-Key Signature (FAIL):")\n'
            'print("NPDM Signature (FAIL):")\n'
            'for i in range(6): print(f"Level {i} (GOOD)")\n')
        helper.chmod(0o755)
        base = self.root/'base.nsp'; base.write_bytes(game_image('nsp', managed=True))
        out = self.root/'merged'
        merge(base, self.update, self.keys, helper, out)
        self.assertEqual((out/'romfs.bin').read_bytes(), romfs())

    def test_bundle_contains_only_selected_game_version(self):
        def fake_lists(tree, res, game, logs):   # shader_list.py resolve, tested in test_shader_list
            for name in ('pso_manifest.tsv', 'pso_title.tsv'):
                (res/'build'/name).write_text('# resolved\n')
        for version, display in (('v112', '1.1.2'), ('v121', '1.2.1'), ('v143', '1.4.3')):
            with self.subTest(version=version):
                tree=self.root/version;out=self.root/f'{version}.app'
                files=[f'build/totk-native-{version}','build/pso_manifest.tsv','build/pso_title.tsv','build/spirv-cross/spirv-cross',
                       'prebuilt/macos-arm64/totk_xlate','build/app-launcher',f'extracted/{version}/romfs/romfs.bin',
                       f'extracted/{version}/romfs/runtime-index.bin',
                       f'build/shader-seed/{version}/seed-manifest.json',
                       f'build/shader-seed/{version}/pso_manifest.tsv']+[f'rewritten/{version}/{n}.patched.bin' for n in ('main','sdk','subsdk0')]
                files+=[f'build/game-data/{version}/{n}' for n in build_app.GAME_DATA]
                for rel in files:
                    p=tree/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'test')
                import hashlib
                (tree/f'build/shader-seed/{version}/seed-manifest.json').write_text(json.dumps(dict(
                    version=version, chain='totk-native-v8-unverified', shaders=8000,
                    files={'pso_manifest.tsv':hashlib.sha256(b'test').hexdigest()})))
                with patch.object(build_app, 'app_seal_valid', return_value=True), patch.object(build_app, 'sign_app') as signing, \
                     patch.object(build_app, 'resolve_lists', side_effect=fake_lists):
                    build_app.step_app(tree,out,self.root/'logs',version=version)
                signing.assert_called_once_with(out.with_name(out.name+'.partial'),self.root/'logs')
                info=plistlib.loads((out/'Contents/Info.plist').read_bytes())
                self.assertEqual(info['CFBundleShortVersionString'],display)
                self.assertNotIn('MNGSourceRevision', info)   # no update notice to compare it with GitHub
                self.assertIn('zh-Hans',info['CFBundleLocalizations']);self.assertIn('zh-Hant',info['CFBundleLocalizations'])
                self.assertEqual('pt-BR' in info['CFBundleLocalizations'],version=='v143')
                res=out/'Contents/Resources/totk'
                self.assertFalse((res/'build'/f'totk-native-{version}').exists())
                self.assertTrue((res/'build/totk-native').exists())
                self.assertTrue((res/f'rewritten/{version}/main.patched.bin').exists())
                self.assertTrue((res/'build/shader-seed/seed-manifest.json').exists())
                self.assertTrue((res/'build/pso_title.tsv').exists())   # the list the first frame waits for
                self.assertEqual([p.name for p in (res/'rewritten').iterdir()], [version])
                self.assertEqual(sorted(p.name for p in (res/'build/game-data').iterdir()), [version])   # runtime/game_data.h
                for name in build_app.GAME_DATA:
                    self.assertTrue((res/f'build/game-data/{version}'/name).is_file())

    def test_failed_bundle_seal_preserves_previous_app(self):
        tree=self.root/'tree';out=self.root/'existing.app';out.mkdir()
        (out/'keep.txt').write_text('previous working app')
        (tree/'build/shader-seed/v143').mkdir(parents=True)
        import hashlib
        game_data(tree,'v143')
        (tree/'build/shader-seed/v143/pso_manifest.tsv').write_bytes(b'test')
        (tree/'build/shader-seed/v143/seed-manifest.json').write_text(json.dumps(dict(
            version='v143',chain='totk-native-v8-unverified',shaders=8000,
            files={'pso_manifest.tsv':hashlib.sha256(b'test').hexdigest()})))
        def fake_clone(_source, target):
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(b'synthetic payload')
        with patch.object(build_app,'data_bundle',side_effect=fake_data_bundle),patch.object(build_app,'clone',side_effect=fake_clone), \
             patch.object(build_app,'sign_app',side_effect=build_app.BuildError('seal rejected')),patch.object(build_app,'resolve_lists'):
            with self.assertRaisesRegex(build_app.BuildError,'seal rejected'):
                build_app.step_app(tree,out,self.root/'logs',version='v143')
        self.assertEqual((out/'keep.txt').read_text(),'previous working app')

    def test_wrong_shader_seed_version_does_not_replace_existing_app(self):
        tree=self.root/'tree';out=self.root/'existing.app';out.mkdir()
        (out/'keep.txt').write_text('previous working app')
        seed=tree/'build/shader-seed/v143';seed.mkdir(parents=True)
        (seed/'seed-manifest.json').write_text(json.dumps(dict(version='v112',chain='totk-native-v8-unverified',shaders=1,files={'x':'0'*64})))
        def fake_clone(_source, target):
            target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'synthetic payload')
        with patch.object(build_app,'clone',side_effect=fake_clone),patch.object(build_app,'app_seal_valid',return_value=True),patch.object(build_app,'sign_app') as signing:
            with self.assertRaisesRegex(build_app.BuildError,'invalid prepared startup shader seed'):
                build_app.step_app(tree,out,self.root/'logs',version='v143')
            signing.assert_not_called()
        self.assertEqual((out/'keep.txt').read_text(),'previous working app')

    def test_running_app_is_preserved_until_its_process_exits(self):
        import hashlib
        from types import SimpleNamespace
        tree=self.root/'tree';out=self.root/'existing.app'
        old=out/'Contents/Resources/totk/build/totk-native';old.parent.mkdir(parents=True);old.write_bytes(b'old running app')
        seed=tree/'build/shader-seed/v143';seed.mkdir(parents=True)
        (seed/'pso_manifest.tsv').write_bytes(b'test')
        game_data(tree,'v143')
        (seed/'seed-manifest.json').write_text(json.dumps(dict(version='v143',chain='totk-native-v8-unverified',shaders=8000,
            files={'pso_manifest.tsv':hashlib.sha256(b'test').hexdigest()})))
        def fake_clone(_source, target):
            target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'new app')
        checks=[]
        def processes(args,*_args,**_kwargs):
            if args[0]=='git':   # build_app.source_commit: the commit the app records (MNGSourceCommit)
                return SimpleNamespace(returncode=0,stdout='abc1234\n')
            self.assertEqual(old.read_bytes(),b'old running app')
            checks.append(True)
            return SimpleNamespace(stdout=str(old.resolve())+'\n' if len(checks)==1 else '')
        with patch.object(build_app,'data_bundle',side_effect=fake_data_bundle),patch.object(build_app,'clone',side_effect=fake_clone),patch.object(build_app,'sign_app'),patch.object(build_app,'mac_pipelines'),patch.object(build_app,'app_seal_valid',return_value=True),patch.object(build_app,'resolve_lists'), \
             patch.object(build_app.subprocess,'check_output',return_value='a'*40), \
             patch.object(build_app.subprocess,'run',side_effect=processes),patch.object(build_app.time,'sleep') as pause:
            build_app.step_app(tree,out,self.root/'logs',version='v143')
        self.assertEqual(len(checks),2);pause.assert_called_once_with(2)
        self.assertEqual(old.read_bytes(),b'new app')

    def test_remove_tree_retries_when_a_file_reappears(self):
        path = self.root/'bundle.app'
        path.mkdir()
        (path/'keep').write_text('x')
        calls = {'n': 0}
        original = shutil.rmtree
        def flaky(target, *args, **kwargs):
            calls['n'] += 1
            if calls['n'] == 1:
                raise OSError(66, 'Directory not empty')
            original(target)
        with patch.object(build_app.shutil, 'rmtree', side_effect=flaky), \
             patch.object(build_app.time, 'sleep') as pause:
            build_app.remove_tree(path)
        self.assertFalse(path.exists())
        self.assertEqual(calls['n'], 2)
        pause.assert_called_once_with(0.2)



if __name__=='__main__': unittest.main()

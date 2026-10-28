"""Build safety and portable-toolchain checks for the Mario Kart 8 Deluxe target; synthetic inputs only, no game files or keys."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
import build_mk8d as build


def nsp(path, names):
    """A PFS0 header with the given file names and empty payloads: enough for the ticket-based title check."""
    strings = b''.join(n.encode() + b'\0' for n in names)
    entries, offset = b'', 0
    for n in names:
        entries += struct.pack('<QQI4x', 0, 0, offset)
        offset += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<II4x', len(names), len(strings)) + entries + strings)
    return path


def load_builder():
    spec = importlib.util.spec_from_file_location('nsp_builder_test', GAME / 'third_party/nxnative/build_ipa_from_nsp.py')
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(GAME / 'runtime/tools'))
    spec.loader.exec_module(module)
    return module


class BuildTests(unittest.TestCase):
    def test_tooling_checksums(self):
        build.verify_prebuilt()

    def test_tooling_rejects_unlisted_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copytree(GAME / 'prebuilt', root / 'prebuilt')
            (root / 'prebuilt/macos-arm64/dmsh_xlate/unexpected.dll').write_bytes(b'extra')
            with mock.patch.object(build, 'GAME', root):
                with self.assertRaisesRegex(ValueError, 'file set differs'):
                    build.verify_prebuilt()

    def test_source_staging_preserves_generated_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            feed = work / 'dependencies/nuget'
            source = build.stage(work, feed)
            table = source / f'dm/games/{build.TITLE}/c1_full.tsv'
            table.write_text('generated locally')
            build.stage(work, feed)
            self.assertEqual(table.read_text(), 'generated locally')
            self.assertTrue((source / 'dm/tools/nx_extract/Program.cs').is_file())
            self.assertFalse((source / 'dm/gfx/tools/spirv_cross_bin/spirv-cross').exists())   # built after staging (build_spirv_cross)
            for exe in ('dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate',):
                self.assertTrue((source / exe).stat().st_mode & 0o111, exe)
            self.assertIn(f'value="{feed}"', (source / 'NuGet.Config').read_text())
            self.assertIn('<clear />', (source / 'NuGet.Config').read_text())
            chain = subprocess.check_output([sys.executable, str(source / 'dm/gfx/tools/dmsh_chain.py')], text=True).strip()
            self.assertIn(f'"{chain}"', (source / 'dm/gfx/nvn/dmsh_chain.h').read_text())

    def test_spirv_cross_is_built_from_its_pinned_release(self):
        # no binary is carried (after PR #1226): the lock pins the --revision the build writes, from the release and commit time
        self.assertFalse((GAME / 'prebuilt/macos-arm64/spirv_cross/spirv-cross').exists())
        lock = dict(l.rstrip('\n').split('\t', 1) for l in (GAME / 'runtime/gfx/tools/dmsh_chain.lock').read_text().splitlines(True)
                    if '\t' in l and not l.startswith('#'))
        self.assertEqual(lock['spirv-cross'], build.SPIRV_CROSS_REVISION)
        header = build.spirv_cross_header('vulkan-sdk-1.4.363.0', 1789558039)   # f11ba9f's commit time
        self.assertIn(f'"{build.SPIRV_CROSS_REVISION}"', header)
        self.assertEqual(build.SPIRV_CROSS[1], 'vulkan-sdk-1.4.363.0')

    def test_chain_header_matches_runtime(self):
        chain = subprocess.check_output([sys.executable, str(GAME / 'runtime/gfx/tools/dmsh_chain.py')], text=True).strip()
        self.assertIn(f'"{chain}"', (GAME / 'runtime/gfx/nvn/dmsh_chain.h').read_text())

    def test_input_selection_skips_add_on_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()   # macOS: the temporary folder is under /var, a link to /private/var
            inputs, work = root / 'originals', root / 'build'
            inputs.mkdir(); work.mkdir()
            base = nsp(inputs / 'any name.nsp', [f'{build.TITLE}0000000000000000.tik', 'a.nca'])
            update = nsp(inputs / 'update [v1507328].nsp', [f'{build.UPDATE}0000000000000004.tik', 'b.nca'])
            nsp(inputs / 'add-on [0100152000023001].nsp', ['0100152000023001000000000000000a.tik'])
            result = build.link_inputs(inputs, work)
            links = sorted(p.name for p in result.iterdir())
            self.assertEqual(links, [f'Mario Kart 8 Deluxe [{build.TITLE}][v0].nsp', f'Mario Kart 8 Deluxe [{build.UPDATE}][v1507328].nsp'])
            self.assertEqual((result / links[0]).resolve(), base)
            self.assertEqual((result / links[1]).resolve(), update)

    def test_input_selection_rejects_other_updates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            inputs = root / 'originals'; inputs.mkdir()
            nsp(inputs / 'base.nsp', [f'{build.TITLE}0000000000000000.tik'])
            nsp(inputs / 'update [v1441792].nsp', [f'{build.UPDATE}0000000000000004.tik'])
            with self.assertRaisesRegex(ValueError, 'only the 4.0.1 update'):
                build.link_inputs(inputs, root / 'build')
            (inputs / 'update [v1441792].nsp').unlink()
            with self.assertRaisesRegex(ValueError, 'no Mario Kart 8 Deluxe 4.0.1 update'):
                build.link_inputs(inputs, root / 'build')

    def test_build_caches_stay_in_work_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            env = build.environment(work)
            for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
                self.assertTrue(Path(env[key]).is_relative_to(work))
                self.assertTrue(Path(env[key]).is_dir())

    def test_rejects_missing_or_modified_offline_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            package = work / 'local.nupkg'
            with self.assertRaisesRegex(ValueError, 'package is missing'):
                build.prepare_offline_dependencies(work, package)
            package.write_bytes(b'wrong package')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                build.prepare_offline_dependencies(work, package)
            self.assertFalse((work / 'dependencies').exists())

    def test_builder_rejects_other_titles_dlc_and_versions(self):
        module = load_builder()
        fam = {'base': Path('base.nsp'), 'updates': [(1507328, Path('u.nsp'))], 'dlc': []}
        module.target_inputs(0x0100152000022000, fam)
        with self.assertRaisesRegex(module.BuildError, 'supports only Mario Kart 8 Deluxe'):
            module.target_inputs(0x0100965017338000, fam)
        with self.assertRaisesRegex(module.BuildError, 'add-on content is not supported'):
            module.target_inputs(0x0100152000022000, dict(fam, dlc=[(0x0100152000023001, 65536, Path('aoc.nsp'))]))
        with self.assertRaisesRegex(module.BuildError, 'exactly the 4.0.1 update'):
            module.target_inputs(0x0100152000022000, dict(fam, updates=[(1441792, Path('u.nsp'))]))

    def test_builder_rejects_modified_originals_before_rewrite(self):
        module = load_builder()
        pins = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())['update']['exefs']
        with tempfile.TemporaryDirectory() as tmp:
            repo, exefs = Path(tmp), Path(tmp) / 'ExeFS'
            (repo / 'assets').mkdir(); exefs.mkdir()
            shutil.copy2(GAME / 'assets/expected-4.0.1.json', repo / 'assets')
            for name, pin in pins.items():
                (exefs / name).write_bytes(bytes(pin['size']))
            with self.assertRaisesRegex(module.BuildError, 'differs from assets/expected-4.0.1.json'):
                module.verify_originals(repo, 'v1507328', exefs)
            with self.assertRaisesRegex(module.BuildError, 'packs only 4.0.1'):
                module.verify_originals(repo, 'v1441792', exefs)
            (exefs / 'extra').write_bytes(b'x')
            with self.assertRaisesRegex(module.BuildError, 'not the pinned'):
                module.verify_originals(repo, 'v1507328', exefs)

    def test_pins_name_the_profile_builds(self):
        pins = json.loads((GAME / 'assets/expected-4.0.1.json').read_text())
        self.assertEqual((pins['title_id'], pins['package'], pins['version']), (build.TITLE, build.VERSION, '4.0.1'))
        rows = [line.split('\t') for line in (GAME / f'runtime/games/{build.TITLE}/game.tsv').read_text().splitlines() if line.startswith('build\t')]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][1], build.VERSION)
        for module_build in rows[0][3:]:
            name, build_id = module_build.split('=')
            self.assertEqual(pins['update']['exefs'][name]['build_id'], build_id)
        for pin in pins['update']['exefs'].values():
            self.assertRegex(pin['sha256'], r'^[0-9a-f]{64}$')

    def test_profile_carries_no_original_paths(self):
        profile = GAME / f'runtime/games/{build.TITLE}'
        for name in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv'):
            self.assertFalse((profile / name).exists(), name)
        for name in ('pso_manifest.tsv', 'bf_want.tsv', 'shader_map.tsv.conflicts.tsv'):
            data = [line for line in (profile / name).read_text().splitlines() if line and not line.startswith('#')]
            self.assertFalse(any('/' in line for line in data), name)
        rows = dict(line.split('\t', 1) for line in (profile / 'game.tsv').read_text().splitlines() if '\t' in line and not line.startswith('#'))
        self.assertEqual(rows['romfs_marker'], '.')
        self.assertEqual(rows['shader_groups'], '/=archive')

    def test_build_environment_disables_nuget_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(build.environment(Path(tmp))['NuGetAudit'], 'false')


if __name__ == '__main__':
    unittest.main()

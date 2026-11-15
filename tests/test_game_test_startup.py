"""Quick/test startup rejects mixed versions before starting a process."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

from tools.game_test_startup import StartupWatch, launch, preflight, startup_state

CHAIN = 'oot-native-v28-mesh-primitive-blocks'


class GameTestStartup(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary, self.native = self.root / 'runtime', self.root / 'native'
        self.seed, self.libs = self.root / 'seed', self.native / 'shader-libs'
        self.seed.mkdir(); self.libs.mkdir(parents=True)
        (self.native / 'shader-service').mkdir()
        self.binary.write_bytes(b'Mach-O fixture\0MNG_SHADER_CHAIN=' + CHAIN.encode() + b'\0')
        (self.native / 'shader-service/totk_xlate').write_bytes(b'current translator')
        (self.native / 'pso_title.tsv').write_text('R\ttitle-pipeline\n')
        source = b'verified startup MSL'
        source_hash = hashlib.sha256(source).hexdigest()
        (self.seed / 'shader_b.metal').write_bytes(source)
        library = source_hash + '.metallib'
        (self.libs / library).write_bytes(b'prepared Metal library')
        self.metadata = dict(chain=CHAIN, all_default=True, shaders=1, corpus_shaders=1, translated_default=1,
                             translator_sha256=hashlib.sha256(b'current translator').hexdigest(),
                             files={'shader_b.metal': source_hash})
        self.libraries = dict(complete=True, sources={'shader_b.metal': source_hash},
                              files={library: hashlib.sha256(b'prepared Metal library').hexdigest()})
        self.save()
        self.popen = Mock(return_value='process')

    def save(self):
        (self.seed / 'seed-manifest.json').write_text(json.dumps(self.metadata))
        (self.libs / 'libraries-manifest.json').write_text(json.dumps(self.libraries))

    def run_launch(self, **kw):
        return launch(self.binary, self.native, self.seed, self.libs, CHAIN, ['rewritten', 'boot'],
                      env={'TOTK_GPU_ALLOW_SUBMIT':'1'}, popen=self.popen, **kw)

    def rejected(self):
        with self.assertRaises((ValueError, FileNotFoundError, json.JSONDecodeError)):
            self.run_launch()
        self.popen.assert_not_called()

    def test_matched_quick_and_test_start_once_after_verification(self):
        for mode in ('quick', 'test'):
            process, facts = self.run_launch(mode=mode)
            self.assertEqual(process, 'process'); self.assertEqual(facts['chain'], CHAIN)
            self.assertEqual(self.popen.call_args.kwargs['env']['TOTK_WARM_BEFORE_TITLE'], '1')
            self.assertEqual(self.popen.call_args.kwargs['env']['TOTK_SHADER_SEED'], 'on')
        self.assertEqual(self.popen.call_count, 2)

    def test_v28_runtime_with_v27_seed_rejected_before_launch(self):
        self.metadata['chain'] = 'oot-native-v27-blend-zero'; self.save(); self.rejected()

    def test_source_label_cannot_hide_an_old_compiled_runtime(self):
        self.binary.write_bytes(b'MNG_SHADER_CHAIN=oot-native-v27-blend-zero\0'); self.rejected()

    def test_unmarked_runtime_rejected(self):
        self.binary.write_bytes(b'old runtime without compiled identity'); self.rejected()

    def test_missing_seed_rejected(self):
        (self.seed / 'seed-manifest.json').unlink(); self.rejected()

    def test_incomplete_seed_rejected(self):
        self.metadata['translated_default'] = 0; self.save(); self.rejected()

    def test_translator_replacement_rejected(self):
        (self.native / 'shader-service/totk_xlate').write_bytes(b'other translator'); self.rejected()

    def test_missing_or_damaged_library_rejected(self):
        library = self.libs / next(iter(self.libraries['files']))
        library.write_bytes(b'damaged'); self.rejected()
        library.unlink(); self.rejected()

    def test_missing_shader_coverage_rejected(self):
        self.libraries['sources'] = {}; self.save(); self.rejected()

    def test_incomplete_library_manifest_rejected(self):
        self.libraries['complete'] = False; self.save(); self.rejected()

    def test_damaged_seed_source_rejected(self):
        (self.seed / 'shader_b.metal').write_bytes(b'wrong source'); self.rejected()

    def test_different_library_location_rejected(self):
        other = self.root / 'other-libraries'; other.mkdir()
        for p in self.libs.iterdir():
            (other / p.name).write_bytes(p.read_bytes())
        with self.assertRaisesRegex(ValueError, 'not the libraries'):
            preflight(self.binary, self.native, self.seed, other, CHAIN)

    def test_no_title_pipeline_list_rejected(self):
        (self.native / 'pso_title.tsv').write_text('# empty\n'); self.rejected()

    def test_full_or_cold_fallback_modes_cannot_silently_start(self):
        with self.assertRaises(ValueError): self.run_launch(mode='full')
        with self.assertRaises(ValueError):
            launch(self.binary, self.native, self.seed, self.libs, CHAIN, [], env={'TOTK_SHADER_SEED':'off'}, popen=self.popen)
        self.popen.assert_not_called()

    def test_manifest_cannot_escape_prepared_folder(self):
        self.metadata['files'] = {'../outside': '0' * 64}; self.save(); self.rejected()

    def test_translator_environment_override_rejected(self):
        with self.assertRaisesRegex(ValueError, 'override'):
            launch(self.binary, self.native, self.seed, self.libs, CHAIN, [],
                   env={'TOTK_SHADER_XLATE':'old-tool'}, popen=self.popen)
        self.popen.assert_not_called()

    def test_runtime_seed_failure_is_immediate(self):
        with self.assertRaises(ValueError):
            startup_state('Startup shader seed identity is invalid；改为边玩边翻译', 33)

    def test_encoding_only_run_rejected_before_launch(self):
        for env in ({}, {'TOTK_GPU_ALLOW_SUBMIT':'0'},
                    {'TOTK_GPU_ALLOW_SUBMIT':'1','TOTK_GPU_NOCOMMIT':'1'}):
            with self.assertRaisesRegex(ValueError, 'GPU submission'):
                launch(self.binary,self.native,self.seed,self.libs,CHAIN,[],env=env,popen=self.popen)
        self.popen.assert_not_called()

    def test_runtime_encoding_only_warning_is_immediate_failure(self):
        with self.assertRaisesRegex(ValueError, 'disabled GPU submission'):
            startup_state('GPU submission disabled: work is encoded but never submitted',33)

    def test_zero_warm_pipelines_cannot_pass(self):
        with self.assertRaises(ValueError):
            startup_state('启动关键管线已就绪：等待 3 ms，可用着色器 0 / 渲染管线 0 / 计算管线 0', 33)

    def test_import_and_warmup_without_a_frame_are_not_success(self):
        text = '启动关键管线已就绪：等待 30 ms，可用着色器 33 / 渲染管线 59 / 计算管线 1'
        self.assertEqual(startup_state(text, 33), 'preparing')
        self.assertEqual(startup_state(text + '\nfirst presented frame', 33), 'ready')

    def test_unrelated_chatter_cannot_hide_thirty_second_stall(self):
        watch = StartupWatch(0)
        for second in range(31):
            state = watch.observe('audio output level ' + str(second), second, 33)
        self.assertEqual(state, 'diagnose')

    def test_real_warm_progress_resets_the_stall_deadline(self):
        watch = StartupWatch(0)
        self.assertEqual(watch.observe('[test-startup] shaders 1/33', 20, 33), 'preparing')
        self.assertEqual(watch.observe('[test-startup] shaders 2/33', 45, 33), 'preparing')
        self.assertEqual(watch.observe('[test-startup] shaders 2/33', 76, 33), 'diagnose')


if __name__ == '__main__':
    unittest.main()

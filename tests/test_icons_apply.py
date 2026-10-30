"""The settings panel's Apply button icons job (games/totk/runtime/icons_apply.m), with a fake Python standing in for
the builder's icons Python and make_button_icons.py: what it stages, and what it leaves when it fails or stops."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'

# Called as `python -I -c "import fontTools"` (the tools check) and `python <flags> <script> <mod> --app ... --plan <file>
# --replace --progress`. FAKE_* steer it; it logs its arguments and its pid.
FAKE = r'''#!/bin/sh
echo "$*" >> "$FAKE_DIR/log"
if [ "$2" = "-c" ]; then
  [ -n "$FAKE_SLOW_CHECK" ] && sleep "$FAKE_SLOW_CHECK"
  [ -n "$FAKE_NO_FONTTOOLS" ] && exit 1; exit 0
fi
mod=; take=
for a in "$@"; do [ -n "$take" ] && [ -z "$mod" ] && mod="$a"; case "$a" in *.py) take=1;; esac; done
echo $$ > "$FAKE_DIR/pid"
printf '%s\n' "$@" > "$FAKE_DIR/args"
while [ $# -gt 0 ]; do [ "$1" = "--plan" ] && cp "$2" "$FAKE_DIR/plan.json"; shift; done
echo "progress 1/6"
[ -n "$FAKE_SLEEP" ] && sleep 20
if [ -n "$FAKE_FAIL" ]; then echo "something" >&2; echo "boom" >&2; exit 1; fi
for k in 2 3 4 5 6; do echo "progress $k/6"; done
mkdir -p "$mod/Font" && echo icons > "$mod/Font/Font.bfarc.zs"
printf 'Font/Font.bfarc.zs\t6\tx\tx\n' >> "$mod/manifest.tsv"
exit 0
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class IconsApply(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.probe = Path(cls.build.name) / 'probe'
        made = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-DTOTK_GAME_V143', f'-I{RUNTIME}',
                               str(ROOT / 'tests/icons_apply_probe.m'), str(RUNTIME / 'icons_apply.m'),
                               '-framework', 'Foundation', '-o', str(cls.probe)], capture_output=True, text=True)
        if made.returncode:
            raise AssertionError(made.stderr[-3000:])

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.data, self.fake = root / 'data', root / 'fake'
        self.app = root / 'Tears of the Kingdom.app'
        (self.app / 'Contents/Resources/totk/scripts').mkdir(parents=True)
        (self.app / 'Contents/Resources/totk/scripts/make_button_icons.py').write_text('')
        self.fake.mkdir()
        python = self.fake / 'python'
        python.write_text(FAKE); python.chmod(0o755)
        self.mods = self.data / 'mods/v143'
        self.mods.mkdir(parents=True)
        (self.mods / 'manifest.tsv').write_text('Other/File.zs\t1\ta\tb\n')
        self.env = dict(os.environ, TOTK_ICONS_DATA=str(self.data), TOTK_ICONS_PYTHON=str(python),
                        TOTK_APP_BUNDLE=str(self.app), FAKE_DIR=str(self.fake))

    def tearDown(self):
        self.tmp.cleanup()

    def run_probe(self, mode='run', **env):
        out = subprocess.run([str(self.probe), mode], env=dict(self.env, **env), capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        return out.stdout.splitlines()

    def folder(self, suffix):
        return self.data / f'mods/v143{suffix}'

    def test_success_moves_partial_to_next_with_marker(self):
        lines = self.run_probe()
        self.assertIn('started 1', lines)
        updates = [l for l in lines if l.startswith('update ')]
        self.assertEqual(updates[0], 'update 1 1/8 ')
        self.assertEqual(updates[-1], 'update 2 8/8 ')
        self.assertEqual([int(l.split()[2].split('/')[0]) for l in updates], sorted(int(l.split()[2].split('/')[0]) for l in updates))
        staged = self.folder('-keyboard.next')
        self.assertTrue((staged / 'complete').is_file())
        self.assertTrue((staged / 'Font/Font.bfarc.zs').is_file())
        self.assertIn('Other/File.zs', (staged / 'manifest.tsv').read_text(), 'the game mods are kept')
        sha = hashlib.sha256((self.mods / 'manifest.tsv').read_bytes()).hexdigest()
        self.assertEqual((staged / 'made-from.txt').read_text().strip(), sha)
        self.assertFalse(self.folder('-keyboard.next.partial').exists())
        self.assertEqual(list(self.data.glob('mods/*.json')), [], 'the plan file is gone')

    def test_plan_and_arguments(self):
        self.run_probe()
        call = (self.fake / 'args').read_text().splitlines()
        script = call.index(str(self.app / 'Contents/Resources/totk/scripts/make_button_icons.py'))
        self.assertEqual(call[script + 1], str(self.folder('-keyboard.next.partial')))
        for flag, value in (('--app', str(self.app)), ('--version', 'v143'), ('--style', 'plan')):
            self.assertEqual(call[call.index(flag) + 1], value)
        self.assertIn('--replace', call); self.assertIn('--progress', call)
        self.assertIn('"E040":["single","E"]', (self.fake / 'plan.json').read_text())

    @unittest.skipUnless(importlib.util.find_spec('compression.zstd'), 'needs Python 3.14 (compression.zstd)')
    def test_the_real_script_runs_from_the_app_with_these_flags(self):
        # with -I the script's own folder was not searched: `import make_mod_manifest` failed on every run, and the
        # fake hid it. Run here: the real scripts as build_app places them, with the flags the job passes.
        self.run_probe()
        call = (self.fake / 'args').read_text().splitlines()
        flags = call[:call.index(str(self.app / 'Contents/Resources/totk/scripts/make_button_icons.py'))]
        scripts = Path(self.tmp.name) / 'scripts'
        scripts.mkdir()
        for name in ('make_button_icons.py', 'make_mod_manifest.py', 'make_romfs_patches.py'):
            shutil.copy(ROOT / 'games/totk/scripts' / name, scripts / name)
        env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}
        out = subprocess.run([sys.executable, *flags, str(scripts / 'make_button_icons.py'), '--help'], cwd=self.tmp.name,
                             env=env, capture_output=True, text=True, timeout=60)
        self.assertNotIn('ModuleNotFoundError', out.stderr)
        if out.returncode:   # without fontTools the script stops after its own imports, which is what this checks
            self.assertIn('needs fontTools', out.stderr)
        self.assertFalse((scripts / '__pycache__').exists(), 'bytecode written beside the scripts breaks the app\'s seal')

    def test_no_fonttools_reports_no_tools(self):
        lines = self.run_probe(FAKE_NO_FONTTOOLS='1')
        self.assertTrue(any(l.startswith('update 4 ') for l in lines), lines)
        self.assertEqual(sorted(p.name for p in (self.data / 'mods').iterdir()), ['v143'])

    def test_missing_python_reports_no_tools(self):
        lines = self.run_probe(TOTK_ICONS_PYTHON=str(self.fake / 'nothing'))
        self.assertTrue(any(l.startswith('update 4 ') for l in lines), lines)

    def test_script_failure_removes_partial(self):
        keyboard = self.folder('-keyboard'); keyboard.mkdir(); (keyboard / 'manifest.tsv').write_text('old\n')
        lines = self.run_probe(FAKE_FAIL='1')
        self.assertIn('update 3 3/8 boom', lines)
        self.assertFalse(self.folder('-keyboard.next.partial').exists())
        self.assertFalse(self.folder('-keyboard.next').exists())
        self.assertEqual((keyboard / 'manifest.tsv').read_text(), 'old\n')

    def test_app_without_scripts_fails(self):
        shutil.rmtree(self.app / 'Contents/Resources/totk/scripts')
        lines = self.run_probe()
        self.assertTrue(any(l.startswith('update 3 ') for l in lines), lines)
        self.assertFalse(self.folder('-keyboard.next.partial').exists())

    def test_linked_mods_folder_is_refused(self):
        # a clone of a link is a link: the drawing would write into the game's real mods while it runs
        real = Path(self.tmp.name) / 'elsewhere'
        self.mods.rename(real); self.mods.symlink_to(real)
        lines = self.run_probe()
        self.assertTrue(any(l.startswith('update 3 ') for l in lines), lines)
        self.assertFalse((self.fake / 'args').exists(), 'the drawing never starts')
        self.assertEqual(sorted(p.name for p in real.iterdir()), ['manifest.tsv'])
        self.assertFalse(self.folder('-keyboard.next.partial').exists())

    def test_link_inside_the_mods_folder_is_refused(self):
        real = Path(self.tmp.name) / 'Font.bfarc.zs'; real.write_text('mine')
        (self.mods / 'Font').mkdir(); (self.mods / 'Font/Font.bfarc.zs').symlink_to(real)
        lines = self.run_probe()
        self.assertTrue(any(l.startswith('update 3 ') for l in lines), lines)
        self.assertFalse((self.fake / 'args').exists(), 'the drawing never starts')
        self.assertEqual(real.read_text(), 'mine')
        self.assertFalse(self.folder('-keyboard.next.partial').exists())

    def test_the_state_and_a_cancel_do_not_wait_for_the_tools_check(self):
        # the panel reads the state on the main thread, and the window's close cancels there: neither may wait for the
        # check (or the copy) that runs on the job's queue
        lines = self.run_probe('early', FAKE_SLOW_CHECK='3')
        self.assertLess(int(next(l for l in lines if l.startswith('state ms')).split()[2]), 500, lines)
        self.assertLess(int(next(l for l in lines if l.startswith('cancel ms')).split()[2]), 500, lines)
        self.assertIn('after cancel 0', lines)
        self.assertFalse((self.fake / 'args').exists(), 'a cancelled job never starts the drawing')
        self.assertFalse(self.folder('-keyboard.next.partial').exists())
        self.assertFalse(any(l.startswith('update ') for l in lines), 'a cancelled job tells no one')

    def test_a_tools_check_that_hangs_is_given_up(self):
        lines = self.run_probe(FAKE_SLOW_CHECK='40')
        self.assertTrue(any(l.startswith('update 4 ') for l in lines), lines)

    def test_second_start_refused_while_running(self):
        self.assertIn('second 0', self.run_probe('second'))

    def test_cancel_kills_child_and_cleans(self):
        lines = self.run_probe('cancel', FAKE_SLEEP='1')
        self.assertIn('cancelled 0', lines)
        pid = int((self.fake / 'pid').read_text())
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        else:
            os.kill(pid, signal.SIGKILL)
            self.fail('the drawing kept running')
        self.assertFalse(self.folder('-keyboard.next.partial').exists())
        self.assertFalse(self.folder('-keyboard.next').exists())

    def test_missing_mods_folder_records_dash(self):
        shutil.rmtree(self.mods)
        self.run_probe()
        self.assertEqual((self.folder('-keyboard.next') / 'made-from.txt').read_text().strip(), '-')

    def test_stale_after_source_manifest_changes(self):
        keyboard = self.folder('-keyboard'); keyboard.mkdir()
        sha = hashlib.sha256((self.mods / 'manifest.tsv').read_bytes()).hexdigest()
        (keyboard / 'made-from.txt').write_text(sha + '\n')
        self.assertIn('stale 0', self.run_probe('stale'))
        (self.mods / 'manifest.tsv').write_text('changed\n')
        self.assertIn('stale 1', self.run_probe('stale'))

    def test_hand_made_keyboard_icons_are_not_stale(self):
        self.folder('-keyboard').mkdir()
        self.assertIn('stale 0', self.run_probe('stale'))


if __name__ == '__main__':
    unittest.main()

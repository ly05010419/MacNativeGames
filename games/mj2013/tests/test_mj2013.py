"""明星三缺一 2013 on the Zelda-line runtime (user 2026-11-22: "用测试保证 不靠记忆").

Run from games/mj2013: python3 -m unittest tests.test_mj2013 (or `make test`).

What is checked, without the game: the IPC answers this port adds, end to end through the shared IPC server
(tests/ipc_probe.c: firmware version, applet, SD card folder, RomFS, clock, hid shared memory, the screen from nvmap to the
window); the SD card path mapping (tests/sdmc_probe.c); the framebuffer conversion against libnx's own swizzler
(tests/fb_probe.c); the JIT's two views of code memory with the cache line sizes the guest is told (tests/jit_cache_probe.c);
hid's LIFOs read back with libnx's reader and the keyboard layout (tests/hid_probe.c); the rewriter on a
libnx-shaped module (an empty eh_frame_hdr, MOD0's LNY data in the text); the game patches' encodings; and that the port
is on the Zelda line (shared links, no nxnative). With the work folder (MJ2013_WORK, default
/Volumes/mac-win/totk-app/build/mj2013) the game patches are also checked against the player's main."""
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
HAVE_CLANG = sys.platform == 'darwin' and shutil.which('xcrun') is not None
WORK = Path(os.environ.get('MJ2013_WORK', '/Volumes/mac-win/totk-app/build/mj2013'))


def python_with(*modules):
    """A Python that can import the rewriter's modules: this one, MJ2013_PY, or the work folders' usual venv."""
    for candidate in (sys.executable, os.environ.get('MJ2013_PY'), '/Volumes/mac-win/totk-app/build/pla/venv/bin/python'):
        if candidate and Path(candidate).exists():
            probe = subprocess.run([candidate, '-c', ';'.join(f'import {m}' for m in modules)], capture_output=True)
            if probe.returncode == 0:
                return candidate
    return None


def build_and_run(test, name, sources, extra=(), env=None):
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / name
        subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        f'-I{HERE / "tests/probe_tables"}', f'-I{HERE / "runtime"}', *[str(HERE / s) for s in sources], *extra,
                        '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120,
                             env=dict(os.environ, PROBE_TMP=d, BY3_AUDIO='0', **(env or {})))
        test.assertEqual(run.returncode, 0, run.stdout + run.stderr[-3000:])
        return run.stdout


@unittest.skipUnless(HAVE_CLANG, 'clang')
class Probes(unittest.TestCase):
    def test_framebuffer_conversion(self):
        out = build_and_run(self, 'fb', ['tests/fb_probe.c', 'runtime/fb_convert.c', 'third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c',
                                         'third_party/d2r_gfx/gfx/nvn/nvn_tex.c'])
        self.assertIn('fb ok', out)

    def test_sd_card_paths(self):
        self.assertIn('sdmc ok', build_and_run(self, 'sdmc', ['tests/sdmc_probe.c']))

    def test_hid_shared_memory_and_keys(self):
        self.assertIn('hid ok', build_and_run(self, 'hid', ['tests/hid_probe.c', 'runtime/hid_publish.c']))

    def test_jit_code_runs_after_the_guests_cache_flush(self):
        ctr = re.search(r'#define HOST_CTR_EL0 (0x[0-9a-f]+)ULL', (HERE / 'runtime/loader.c').read_text()).group(1)
        self.assertIn('jit ok', build_and_run(self, 'jit', ['tests/jit_cache_probe.c'], [f'-DHOST_CTR_EL0={ctr}ULL']))

    def test_ipc_answers(self):
        sources = ['tests/ipc_probe.c', 'runtime/ipc.c', 'runtime/threads.c', 'runtime/hle_stack.c', 'runtime/hle_stack.S',
                   'runtime/thread_enter.S', 'runtime/services.c', 'runtime/hb_applet.c', 'runtime/hb_system.c', 'runtime/hb_fs.c',
                   'runtime/hb_hid.c', 'runtime/hb_display.c', 'runtime/hid_publish.c', 'runtime/svc_memory.c', 'runtime/fb_convert.c',
                   'runtime/audio_out.c', 'third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c', 'third_party/d2r_gfx/gfx/nvn/nvn_tex.c']
        frameworks = ['-framework', 'CoreFoundation', '-framework', 'CoreGraphics', '-framework', 'ImageIO', '-framework', 'AudioToolbox']
        out = build_and_run(self, 'ipc', sources, frameworks)
        self.assertIn('ipc ok', out)
        # shared/runtime/audio_out.c: a client spinning on GetReleasedAudioOutBuffers is held until a buffer is played
        # (this game's port asked ~50 000 times a second and kept a core at 94 %); TOTK_AUDOUT_POLL_WAIT_US=0 answers
        # every ask at once, as before.
        polls = int(re.search(r'audout polls=(\d+)', out).group(1))
        spun = build_and_run(self, 'ipc', sources, frameworks, env={'TOTK_AUDOUT_POLL_WAIT_US': '0'})
        spun_polls = int(re.search(r'audout polls=(\d+)', spun).group(1))
        self.assertLess(polls, 1000, out)
        self.assertGreater(spun_polls, 10 * polls, spun)


FAKE_RUNTIME = '''#!/bin/sh
echo "$0 $@" >> runs.txt
pwd > cwd.txt
env | sort > env.txt
mkdir -p "$TOTK_PROFILE_DIR/sdmc/switch/mj2013/save" && echo "run $$" > "$TOTK_PROFILE_DIR/sdmc/switch/mj2013/save/update.rom"
'''


@unittest.skipUnless(HAVE_CLANG, 'clang')
class App(unittest.TestCase):
    """app/launcher.c in a fake 明星三缺一.app (a shell script as the runtime), HOME in a temp folder: the player's data
    goes to ~/Library/Application Support/MacNativeGames/mj2013, the SD card is backed up before each start."""
    def test_the_launcher_runs_the_game_from_the_bundle_with_the_data_folder(self):
        with tempfile.TemporaryDirectory() as d:
            app = Path(d) / '明星三缺一.app'
            game = app / 'Contents/Resources/game'
            (app / 'Contents/MacOS').mkdir(parents=True)
            for f in ('rewritten/v0/main.patched.bin', 'romfs/romfs.bin'):
                (game / f).parent.mkdir(parents=True, exist_ok=True)
                (game / f).write_bytes(b'x')
            (game / 'build').mkdir()
            (game / 'build/mj2013-native').write_text(FAKE_RUNTIME)
            (game / 'build/mj2013-native').chmod(0o755)
            launcher = app / 'Contents/MacOS/mj2013'
            subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'app/launcher.c'), '-o', str(launcher)], check=True)
            home = Path(d) / 'home'
            home.mkdir()
            env = {k: v for k, v in os.environ.items() if k != 'MJ2013_DATA_DIR'}
            env.update(HOME=str(home), TOTK_HEADLESS='1')
            check = subprocess.run([str(launcher), '--check'], env=env, capture_output=True, text=True)
            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertIn('game was not launched', check.stdout)
            for _ in range(2):
                run = subprocess.run([str(launcher)], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stderr)
            data = home / 'Library/Application Support/MacNativeGames/mj2013'
            self.assertEqual(Path((data / 'cwd.txt').read_text().strip()).resolve(), data.resolve())
            seen = dict(line.split('=', 1) for line in (data / 'env.txt').read_text().splitlines() if '=' in line)
            self.assertEqual(Path(seen['TOTK_PROFILE_DIR']), data / 'profile')
            self.assertEqual(Path(seen['TOTK_ROMFS']).resolve(), (game / 'romfs').resolve())
            self.assertEqual(Path(seen['TOTK_INPUT_FILE']), data / 'commands.txt')
            self.assertNotIn('TOTK_HEADLESS', seen)
            runs = (data / 'runs.txt').read_text().splitlines()
            self.assertEqual(len(runs), 2)
            self.assertTrue(runs[0].endswith(f'{(game / "rewritten/v0").resolve()} boot'), runs[0])
            backups = list((data / 'backups').iterdir())
            self.assertEqual(len(backups), 1)   # the second start found the card the first one wrote
            self.assertTrue((backups[0] / 'switch/mj2013/save/update.rom').is_file())


def synthetic_homebrew():
    """A libnx-shaped NSO: crt0's `b` and "HOMEBREW", MOD0 with an empty eh_frame_hdr and LNY records (LNY1's magic decodes as
    `adds w12, w18, ...`), then code with an svc, an x18 use and a tpidrro_el0 read."""
    text = bytearray(0x1000)
    struct.pack_into('<I', text, 0, 0x14000020)
    struct.pack_into('<I', text, 4, 0xf0)
    text[8:16] = b'HOMEBREW'
    struct.pack_into('<4sIIIiii', text, 0xf0, b'MOD0', 0x2000 - 0xf0, 0x3000 - 0xf0, 0x3000 - 0xf0, 0x200 - 0xf0, 0x200 - 0xf0, 0)
    struct.pack_into('<4sII', text, 0x10c, b'LNY0', 0, 0)
    struct.pack_into('<4sII', text, 0x118, b'LNY1', 0x10bf2c00, 0)
    struct.pack_into('<4sII', text, 0x124, b'LNY2', 1, 0)
    for i, w in enumerate((0xd4000421, 0xaa1203e0, 0xd53bd060, 0xd65f03c0)):
        struct.pack_into('<I', text, 0x80 + 4 * i, w)
    ro, data = bytes(0x1000), bytes(0x1000)
    header = bytearray(0x100)
    header[0:4] = b'NSO0'
    offset = 0x100
    for i, (seg, mem) in enumerate(((text, 0), (ro, 0x1000), (data, 0x2000))):
        struct.pack_into('<III', header, 0x10 + i * 0x10, offset, mem, len(seg))
        struct.pack_into('<I', header, 0x60 + 4 * i, len(seg))
        offset += len(seg)
    struct.pack_into('<I', header, 0x3c, 0x1000)
    header[0x40:0x60] = bytes(range(32))
    return bytes(header) + bytes(text) + ro + data


class Rewriter(unittest.TestCase):
    def test_a_libnx_module_is_rewritten_and_its_data_kept(self):
        py = python_with('capstone', 'lz4')
        if not py:
            self.skipTest('no Python with capstone and lz4 (MJ2013_PY)')
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'main.nso'
            src.write_bytes(synthetic_homebrew())
            out = Path(d) / 'out'
            out.mkdir()
            run = subprocess.run([py, str(HERE / 'third_party/d2r/patch_nso.py'), str(src), str(out), 'main'], capture_output=True, text=True,
                                 cwd=str(HERE / 'third_party/d2r'), env=dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0'))
            self.assertEqual(run.returncode, 0, run.stderr[-2000:])
            sites = dict(line.split('\t')[:2] for line in (out / 'main.sites.tsv').read_text().splitlines())
            self.assertEqual(sites, {'0x80': 'svc', '0x84': 'x18', '0x88': 'tpidrro_r'})
            image = (out / 'main.patched.bin').read_bytes()
            self.assertEqual(image[0x10c:0x130], synthetic_homebrew()[0x100 + 0x10c:0x100 + 0x130], 'MOD0 LNY records untouched')
            self.assertEqual(image[8:16], b'HOMEBREW')


class GamePatches(unittest.TestCase):
    def patches(self):
        text = (HERE / 'runtime/game_patches.c').read_text()
        return [(int(o, 16), int(a, 16), int(b, 16)) for o, a, b in re.findall(r'\{0x([0-9a-f]+), 0x([0-9a-f]+)u, 0x([0-9a-f]+)u,', text)]

    def test_the_patches_say_what_they_mean(self):
        cs = importlib.util.find_spec('capstone')
        if not cs:
            self.skipTest('capstone')
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        dis = lambda w, a: next(md.disasm(struct.pack('<I', w), a))
        got = {o: (dis(a, o), dis(b, o)) for o, a, b in self.patches()}
        self.assertEqual(set(got), {0x95e5c, 0x95ea0, 0xa7d0})
        self.assertEqual((got[0x95e5c][0].op_str, got[0x95e5c][1].op_str), ('x20, x20, #0xc', 'x20, x20, #0xe'))
        self.assertEqual((got[0x95ea0][0].op_str, got[0x95ea0][1].op_str), ('x19, x0, x19, lsl #12', 'x19, x0, x19, lsl #14'))
        self.assertEqual((got[0xa7d0][0].mnemonic, got[0xa7d0][1].mnemonic), ('b.eq', 'nop'))

    def test_the_original_words_are_the_players_game(self):
        image = WORK / 'rewritten/v0/main.patched.bin'
        if not image.exists():
            self.skipTest(f'no prepared game in {WORK}')
        data = image.read_bytes()
        for offset, original, _ in self.patches():
            self.assertEqual(struct.unpack_from('<I', data, offset)[0], original, hex(offset))


class ZeldaLine(unittest.TestCase):
    LINKED = {'runtime/ipc.c': 'shared/runtime/ipc.c', 'runtime/ipc.h': 'shared/runtime/ipc.h', 'runtime/threads.c': 'shared/runtime/threads.c',
              'runtime/gate.S': 'shared/runtime/gate.S', 'runtime/thread_enter.S': 'shared/runtime/thread_enter.S',
              'runtime/hle_stack.c': 'shared/runtime/hle_stack.c', 'runtime/input_file.h': 'shared/runtime/input_file.h',
              'runtime/audio_out.c': 'shared/runtime/audio_out.c', 'runtime/thread_compat.h': 'shared/runtime/thread_compat.h',
              'third_party/d2r': 'games/totk/third_party/d2r', 'runtime/mng_active.h': 'games/totk/runtime/mng_active.h',
              'third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c': 'shared/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c',
              'third_party/d2r_gfx/gfx/nvn/nvn_tex.c': 'shared/third_party/d2r_gfx/gfx/nvn/nvn_tex.c',
              'app/app_launcher.h': 'shared/app/app_launcher.h'}

    def test_shared_code_is_linked_not_copied(self):
        for mine, shared in self.LINKED.items():
            with self.subTest(mine):
                path = HERE / mine
                self.assertTrue(path.is_symlink(), f'{mine} is a copy')
                self.assertEqual(path.resolve(), (ROOT / shared).resolve())

    def test_no_nxnative(self):
        for path in HERE.rglob('*'):
            if path.is_file() and not path.is_symlink() and path.suffix in ('.c', '.h', '.m', '.S', '.py', 'Makefile') and path.name != 'test_mj2013.py':
                self.assertNotIn('nxnative', path.read_text(errors='replace').lower(), str(path))
        self.assertFalse((HERE / 'third_party/nxnative').exists())

    def test_no_game_content(self):
        expected = json.loads((HERE / 'assets/expected.json').read_text())
        self.assertEqual(set(expected), {'title_id', 'name', 'exefs_sha256', 'romfs_size', 'romfs_sha256', 'rewritten_sha256'})
        for path in HERE.rglob('*'):
            if path.is_file() and not path.is_symlink():
                self.assertLess(path.stat().st_size, 200_000, f'{path}: too large for source (game data?)')


if __name__ == '__main__':
    unittest.main()

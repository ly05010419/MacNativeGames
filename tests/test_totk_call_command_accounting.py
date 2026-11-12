"""CPU regressions for Call/Copy accounting, using the real recorder helpers.

Only device, logging and diagnostic hooks are stubbed. No game files, Metal
device or running game are needed. Both host-copy paths run with UBSan.
"""
from pathlib import Path
import json
import os
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
GFX = GAME / 'third_party/d2r_gfx/gfx/nvn'
PROBE = GAME / 'tests/call_command_accounting/probe.c'


def function(source, marker, last=False):
    start = source.rindex(marker) if last else source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def implementation():
    source = (GFX / 'hle_nvn.c').read_text()
    objects = (GFX / 'nvn_obj.h').read_text()
    start = objects.index('typedef struct {', objects.index('// Command records live in host memory'))
    command = objects[start:objects.index('} cmd_t;', start) + len('} cmd_t;')]
    start = objects.index('enum {', objects.index('} op_hdr;'))
    operations = objects[start:objects.index('};', start) + 2]
    start = source.index('#define COPY_MAGIC ')
    copies = source[start:source.index('static uint8_t *flatten_recording_ops(', start)]
    parts = [command, 'typedef cmd_t NVNcommandBuffer;', '#define CB(c) ((cmd_t *)(c))',
             operations, 'typedef struct { uint32_t op, size; } op_hdr;',
             'static uint64_t g_submits;', copies, r'''
static struct { uint64_t bytes, recs; } g_cs;
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t t_rec_hist[OP_COUNT_];
static int cs_on(void) { return 0; }
static void cs_op(unsigned op, unsigned n) { (void)op; (void)n; }
static void (*runtime_frametrace_event)(int);
static void die(const char *fmt, ...) { (void)fmt; abort(); }
static int cmdlog_env(void) { return 0; }
static void life_call(NVNcommandBuffer *c, int n, const NVNcommandHandle *h, const char *s) {
    (void)c; (void)n; (void)h; (void)s;
}
static void copy_touch(NVNcommandHandle h, uint64_t n) {
    copy_prefix *c = copy_of(h); assert(c->magic == COPY_MAGIC); c->used_at = n;
}
static int call_stats_on(void) { return 0; }
static void call_stats_note(NVNcommandBuffer *c, NVNcommandHandle h, const uint8_t *p) {
    (void)c; (void)h; (void)p;
}
static int g_inline_bulk_negative;
static uint8_t *flatten_recording_ops(const uint8_t *p);
''']
    # Preserve the real weak declaration: missing-hook is an actual link case.
    start = source.index('extern int runtime_nvn_call_commands_reference_only(')
    parts.append(source[start:source.index(';', start) + 1])
    markers = [
        'static void put_hdr(', 'static void cmd_reserve(', 'static uint8_t *cmd_alloc(',
        'static void hw_fix(', 'NVN(nvnCommandBufferInitialize,',
        'NVN(nvnCommandBufferFinalize,', 'NVN(nvnCommandBufferBeginRecording,',
        'static int flatten_fast_mode(', 'static uint8_t *flatten_recording(',
        'static uint8_t *flatten_recording_ops(', 'static void *rec(',
    ]
    for marker in markers:
        parts.append(function(source, marker, last=marker == 'static uint8_t *flatten_recording_ops('))
    parts.append('#define REC(c,OP,T) ((T *)rec((c),(OP),sizeof(T)))')
    for marker in [
        'NVN(nvnCommandBufferBindUniformBuffer,', 'NVN(nvnCommandBufferDrawElements,',
        'static int inline_bulk_on(', 'static void inline_recording(',
        'static void inline_call_recording(', 'NVN(nvnCommandBufferCopyCommands,',
        'NVN(nvnCommandBufferCallCommands,', 'NVN(nvnCommandBufferGetCommandMemoryUsed,',
    ]:
        if marker == 'static int inline_bulk_on(':
            parts.append('static _Atomic int g_inline_bulk_on = -1;')
        parts.append(function(source, marker, last=marker == 'NVN(nvnCommandBufferGetCommandMemoryUsed,'))
    return '\n'.join(parts)


@unittest.skipUnless(platform.system() == 'Darwin' and shutil.which('xcrun'), 'macOS with clang')
class CallCommandAccounting(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        cls.directory = Path(cls.work.name)
        (cls.directory / 'implementation.inc').write_text(implementation())
        cls.executables = {}
        for hook in ('missing', 'false', 'true'):
            exe = cls.directory / hook
            args = []
            if hook == 'missing':
                # Darwin otherwise rejects any undefined symbol at link time;
                # allow this one weak import so dyld can exercise its null case.
                args.append('-Wl,-U,_runtime_nvn_call_commands_reference_only')
            else:
                hook_source = cls.directory / f'{hook}.c'
                hook_source.write_text('int runtime_nvn_call_commands_reference_only(void) { return ' +
                                       str(int(hook == 'true')) + '; }\n')
                args.append(str(hook_source))
            result = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-unused-function', '-Wno-missing-field-initializers',
                                     '-fsanitize=undefined', '-fno-sanitize-recover=all', *args,
                                     '-I', str(cls.directory), str(PROBE), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(result.stderr)
            cls.executables[hook] = exe

    def run_probe(self, hook, mode, bulk):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_', 'DYLD_'))}
        env['TOTK_INLINE_BULK'] = bulk
        result = subprocess.run([str(self.executables[hook]), mode], env=env, check=True,
                                capture_output=True, text=True, timeout=15)
        return json.loads(result.stdout)

    def test_accounting_and_replay(self):
        expected = {'call': 20, 'copy': 68, 'multiple': 20, 'reuse': 20, 'mixed': 68,
                    'zero': 20, 'empty': 20, 'nested_call': 20, 'nested_copy': 68,
                    'retired_source': 20, 'growth': 20, 'next_op': 40, 'bare_fix': 12,
                    'threshold': 1720}
        for bulk in ('0', '1'):
            for mode, used in expected.items():
                with self.subTest(bulk=bulk, mode=mode):
                    missing = self.run_probe('missing', mode, bulk)
                    false = self.run_probe('false', mode, bulk)
                    corrected = self.run_probe('true', mode, bulk)
                    self.assertEqual(missing, false)
                    for key in ('host_used', 'host_hash', 'draws', 'last_draw_count',
                                'ctl_before_end', 'ctl_after_end'):
                        self.assertEqual(corrected[key], false[key], key)
                    self.assertEqual(corrected['hw'], used)
                    self.assertEqual(corrected['logical_used'], used)
                    if mode not in ('copy', 'mixed', 'nested_copy'):
                        self.assertEqual(corrected['after'], corrected['before'])
                        self.assertEqual(corrected['after_last'], corrected['before_last'])
                    if mode == 'copy':
                        self.assertEqual(corrected, false)
                    if mode == 'growth':
                        self.assertEqual(corrected['draws'], 3000)
                        self.assertEqual(false['hw'], 144020)
                    if mode == 'threshold':
                        self.assertEqual(false['hw'], 2104)
                        self.assertEqual((2048 + corrected['hw'] + 4095) & ~4095, 4096)
                        self.assertEqual((2048 + false['hw'] + 4095) & ~4095, 8192)

    def test_runtime_profile_capability(self):
        source = (GAME / 'runtime/gfx_bridge.c').read_text()
        capability = function(source, 'int runtime_nvn_call_commands_reference_only(')
        unit = self.directory / 'profile.c'
        unit.write_text('#include "game_version.h"\n' + capability +
                        '\nint main(void) { return runtime_nvn_call_commands_reference_only() != EXPECTED; }\n')
        profiles = [([], 1), (['TOTK_GAME_V121'], 1), (['TOTK_GAME_V143'], 0),
                    (['GAME_BOTW', 'GAME_BOTW160'], 0), (['GAME_BOTW', 'GAME_BOTW190'], 0),
                    (['GAME_NS2', 'GAME_DKB'], 0), (['GAME_NS2', 'GAME_POKOPIA'], 0)]
        for defines, expected in profiles:
            with self.subTest(defines=defines):
                exe = self.directory / 'profile'
                subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                '-I', str(GAME / 'runtime'), f'-DEXPECTED={expected}',
                                *[f'-D{d}=1' for d in defines], str(unit), '-o', str(exe)],
                               check=True, capture_output=True, text=True, timeout=30)
                subprocess.run([str(exe)], check=True, capture_output=True, timeout=5)


if __name__ == '__main__':
    unittest.main()

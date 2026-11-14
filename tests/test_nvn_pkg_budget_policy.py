#!/usr/bin/env python3
"""Compile actual cache-policy fragments without requiring Metal or an Apple SDK."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'
GIB = 1 << 30
MIB = 1 << 20

# variant, physical bytes, MB environment (None=absent), enable environment, expected mode, expected MiB
CASES = [
    ('oot_mac', 8 * GIB, None, None, 2, 1536),
    ('oot_mac', 16 * GIB, None, None, 2, 1536),
    ('oot_mac', 16 * GIB + 1, None, None, 0, 0),
    ('oot_mac', 32 * GIB, None, None, 0, 0),
    ('oot_mac', 16 * GIB, '', None, 0, 0),
    ('oot_mac', 16 * GIB, '0', None, 0, 0),
    ('oot_mac', 16 * GIB, '000', None, 0, 0),
    ('oot_mac', 16 * GIB, '1', None, 2, 1),
    ('oot_mac', 16 * GIB, '2048', None, 2, 2048),
    ('oot_mac', 32 * GIB, '1536', None, 2, 1536),
    ('oot_mac', 16 * GIB, '2048junk', None, 2, 2048),  # preserve existing strtoull semantics
    ('oot_mac', 16 * GIB, None, '0', 0, 1536),
    ('oot_mac', 16 * GIB, None, '', 0, 1536),
    ('oot_mac', 16 * GIB, None, '1', 2, 1536),
    ('oot_mac', 16 * GIB, '2048', '0', 0, 2048),
    ('other_mac', 16 * GIB, None, None, 0, 0),
    ('other_mac', 16 * GIB, '', None, 0, 0),
    ('other_mac', 16 * GIB, '0', None, 0, 0),
    ('other_mac', 16 * GIB, '1536', None, 2, 1536),
    ('oot_ios', 16 * GIB, None, None, 1, 0),
    ('oot_ios', 16 * GIB, '', None, 1, 0),
    ('oot_ios', 16 * GIB, '0', None, 1, 0),
    ('oot_ios', 16 * GIB, '1536', None, 2, 1536),
    ('oot_ios', 16 * GIB, None, '0', 0, 0),
    ('other_ios', 16 * GIB, None, None, 1, 0),
    ('oot_other_platform', 16 * GIB, None, None, 0, 0),
]


def actual_initialization():
    source = SOURCE.read_text()
    env_mb = next(line for line in source.splitlines() if line.startswith('static uint64_t env_mb('))
    start = source.index('static void pkg_evict_sweep(void) {')
    start = source.index('        const char *e = getenv("TOTK_TEX_EVICT");', start)
    stop = source.index('        if (mode == 1) MLOG(', start)
    return env_mb, source[start:stop]


class BudgetDefaultPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='oot-budget-policy-')
        folder = Path(cls.temporary.name)
        env_mb, initialization = actual_initialization()
        # A C structure supplies the physical-memory property value. Policy/parse/mode code is extracted unchanged.
        harness = '''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { struct { uint64_t physicalMemory; } processInfo; } NSProcessInfo;
''' + env_mb + '''
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    NSProcessInfo.processInfo.physicalMemory = strtoull(argv[1], NULL, 10);
    unsetenv("TOTK_TEX_EVICT_MB"); unsetenv("TOTK_TEX_EVICT");
    unsetenv("TOTK_TEX_EVICT_LOW_MB"); unsetenv("TOTK_TEX_EVICT_HIGH_MB"); unsetenv("TOTK_TEX_EVICT_CRIT_MB");
    if (strcmp(argv[2], "ABSENT")) setenv("TOTK_TEX_EVICT_MB", argv[2], 1);
    if (strcmp(argv[3], "ABSENT")) setenv("TOTK_TEX_EVICT", argv[3], 1);
    int mode = -1; uint64_t budget, low, high, crit;
''' + initialization + '''
    printf("%d %llu %llu %llu %llu\\n", mode, (unsigned long long)budget,
        (unsigned long long)low, (unsigned long long)high, (unsigned long long)crit);
    return 0;
}
'''
        source = folder / 'policy.c'
        source.write_text(harness)
        cls.executables = {}
        for variant, oot, mac, ios in (
            ('oot_mac', True, 1, 0), ('other_mac', False, 1, 0),
            ('oot_ios', True, 0, 1), ('other_ios', False, 0, 1), ('oot_other_platform', True, 0, 0),
        ):
            executable = folder / variant
            command = ['/usr/bin/cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-UGAME_OOT', '-UTARGET_OS_OSX', '-UTARGET_OS_IPHONE',
                       f'-DTARGET_OS_OSX={mac}', f'-DTARGET_OS_IPHONE={ios}']
            if oot:
                command.append('-DGAME_OOT=1')
            subprocess.run(command + [str(source), '-o', str(executable)], check=True)
            cls.executables[variant] = executable

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_existing_working_set_pressure(self):
        source = SOURCE.read_text()
        start = source.index('    uint64_t avail = 0, want = 0;', source.index('static void pkg_evict_sweep(void) {'))
        stop = source.index('    registry_lock();', start)
        # Preserve the public renderer's pressure-on-working-set behavior, even
        # when host available memory is unknown or already above the low mark.
        fragment = source[start:stop]
        harness = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t available, overage;
static uint64_t evict_available(void) { return available; }
static uint64_t res_over_working_set(void) { return overage; }
static void pressure(int mode, uint64_t low, uint64_t high) {
''' + fragment + r'''
    printf("%llu", (unsigned long long)want);
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    available = strtoull(argv[1], NULL, 10);
    overage = strtoull(argv[2], NULL, 10);
    pressure(1, 250, 350);
    return 0;
}
'''
        folder = Path(self.temporary.name)
        code, executable = folder / 'pressure.c', folder / 'pressure'
        code.write_text(harness)
        subprocess.run(['/usr/bin/cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(code), '-o', str(executable)], check=True)
        for available, overage, expected in (
            (0, 0, ''), (250, 0, ''), (500, 0, ''),
            (500, 64, '64'), (200, 0, '150'), (200, 200, '200'), (0, 64, '64'),
        ):
            with self.subTest(available=available, overage=overage):
                result = subprocess.run([str(executable), str(available), str(overage)],
                                        text=True, capture_output=True, check=True, timeout=5)
                self.assertEqual(result.stdout, expected)

    def test_policy_matrix(self):
        for variant, physical, budget, enable, mode, megabytes in CASES:
            with self.subTest(variant=variant, physical=physical, budget=budget, enable=enable):
                result = subprocess.run([str(self.executables[variant]), str(physical),
                                         'ABSENT' if budget is None else budget,
                                         'ABSENT' if enable is None else enable],
                                        text=True, capture_output=True, check=True, timeout=5)
                self.assertEqual(tuple(map(int, result.stdout.split())),
                                 (mode, megabytes * MIB, 250 * MIB, 350 * MIB, 150 * MIB))


if __name__ == '__main__':
    if '--cases' in __import__('sys').argv:
        print(json.dumps(CASES, indent=2))
    else:
        unittest.main()

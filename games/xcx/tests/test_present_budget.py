"""Presentation diagnostics must respect the original cap of delayed frames."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'third_party/d2r_gfx/gfx/nvn/hle_nvn.c'


class PresentBudgetTest(unittest.TestCase):
    def test_delayed_feedback_across_frame_rate_changes(self):
        source = SOURCE.read_text()
        start = source.index('#define PRESENT_BUDGET_N 16')
        end = source.index('__attribute__((weak)) double dm_brightness_now', start)
        harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#define LOG(...) ((void)0)
%s
int main(void) {
    assert(present_diagnostic_period(33333333) == 33333333);
    /* Normal 30/60/120 FPS are not late; one missed period is late. */
    const uint64_t periods[] = {33333333, 16666666, 8333333};
    for (unsigned i = 0; i < 3; i++) {
        present_budget_store(100 + i, periods[i]);
        double budget = present_budget_ms(100 + i);
        assert(!present_is_late(budget, budget));
        assert(!present_is_late(budget * 1.5, budget));
        assert(present_is_late(budget * 2, budget));
        assert(!present_is_late(0, budget)); /* no callback yet */
        assert(present_is_late(-1, budget)); /* system discarded */
    }
    /* Eight-frame feedback must retain the old cap, in both directions. */
    for (uint64_t f = 200; f < 216; f++) {
        uint64_t period = f < 208 ? 33333333 : 16666666;
        present_budget_store(f, period);
        if (f >= 208) {
            assert(!present_is_late(33.334, present_budget_ms(f - 8)));
            assert(present_is_late(33.334, present_budget_ms(f)));
        }
    }
    for (uint64_t f = 300; f < 316; f++) {
        present_budget_store(f, f < 308 ? 16666666 : 33333333);
        if (f >= 308) {
            assert(present_is_late(33.334, present_budget_ms(f - 8)));
            assert(!present_is_late(33.334, present_budget_ms(f)));
        }
    }
    /* Missing/uncapped history keeps the previous 60-Hz diagnostic fallback. */
    assert(present_is_late(30, present_budget_ms(999)));
    present_budget_store(1000, 0);
    assert(!present_is_late(16.667, present_budget_ms(1000)));
    assert(present_is_late(30, present_budget_ms(1000)));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            code, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            code.write_text(harness % source[start:end])
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(code), '-o', str(binary)], check=True)
            clean_env = {k: v for k, v in os.environ.items() if k != 'DM_NVN_DIAG_FPS'}
            subprocess.run([str(binary)], check=True, env=clean_env)
            for value, expected in [('30', 33333333), ('60', 16666666),
                                    ('120', 8333333), ('0', 33333333),
                                    ('-1', 33333333), ('30x', 33333333),
                                    ('1001', 33333333), ('', 33333333)]:
                check = harness % source[start:end]
                check = check.replace(
                    'assert(present_diagnostic_period(33333333) == 33333333);',
                    f'assert(present_diagnostic_period(33333333) == {expected});')
                code.write_text(check)
                subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                str(code), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True,
                               env={**clean_env, 'DM_NVN_DIAG_FPS': value})

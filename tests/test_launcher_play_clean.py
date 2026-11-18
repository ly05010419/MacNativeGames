"""MNG Launcher's Play is the players' start (user 2026-11-13: "正式启动 按钮 里面 不要加 任何 拖慢游戏的 log或者 debug 代码 那是给用户用的"):
the environment it passes holds the player's own choices only, never a log, a per-frame record, a trace or another
diagnostic switch. Those belong to the diagnostics and test starts."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.builder import start_options  # noqa: E402

DIAGNOSTIC = re.compile(r'LOG|DEBUG|TSV|TRACE|DUMP|STATS|PROF|LABEL|SURVEY|PASSDUMP|WATCH|PROBE|CAPTURE|^DM_NVN|LONGFRAME|UPTIME|CHECK')


class PlayIsClean(unittest.TestCase):
    def test_play_passes_no_diagnostic_switch(self):
        names = {k for env in (*start_options.ENV.values(), *start_options.HOLD_ENV.values()) for k in env}
        self.assertTrue(names, 'Play passes no variables at all? then this test checks nothing')
        self.assertEqual(sorted(n for n in names if DIAGNOSTIC.search(n)), [])

    def test_play_opens_the_app_with_those_variables_only(self):
        text = (ROOT / 'tools/builder/start_options.py').read_text()
        body = text[text.index('def play(runner'):]
        end = body.find('\ndef ', 1)
        body = body[:end] if end > 0 else body
        self.assertIn('env = env_for(state, gpu)', body)
        self.assertNotRegex(body, r'TOTK_DEBUG_LOG|DM_NVN|_LOG\b')


if __name__ == '__main__':
    unittest.main()

"""The Launcher's test controls show and act in test / development builds only, never in the release.

User 2026-11-20, with a screenshot of 3D, 两层景深, 景深强度 and 慢速模式: "记住 这些 只能测试版 显示，正式版 没有"
(11-19: "龟速模式 按钮 只能我 开发版 看到"). The release is the downloaded Launcher (`Paths.isRelease`: its
Info.plist has MNGSourceArchiveSHA256).

Hiding the button is not enough: a value a hidden control sets can still act (an @AppStorage value kept from a
development build, or a default that is not the release's). So for every variable a control binds inside an
`if !Paths.isRelease {` block of HomePage.swift:
  - no control outside those blocks binds it, and
  - every other use of it outside those blocks is on a line that asks `Paths.isRelease` (the release's own value).
"""
import re
import unittest
from pathlib import Path

HOME = Path(__file__).resolve().parents[1] / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift'
OPEN = 'if !Paths.isRelease {'
# the controls the user named; the check finds any others in the blocks too
NAMED = {'depthLayers', 'depthReach', 'slowMotion'}


def code(line):
    """A line without its comment."""
    return line.split('//', 1)[0]


def release_blocks(text):
    """The spans (start, end) of each `if !Paths.isRelease { ... }` block, braces matched."""
    spans = []
    for m in re.finditer(re.escape(OPEN), text):
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans.append((m.start(), i + 1))
    return spans


def problems(text):
    """What lets a test control show or act in the release, as readable lines."""
    spans = release_blocks(text)
    inside = ''.join(text[a:b] for a, b in spans)
    names = set(re.findall(r'\$(\w+)', '\n'.join(code(l) for l in inside.splitlines())))
    found = []
    missing = NAMED - names
    if missing:
        found.append(f'not inside an `{OPEN}` block: {sorted(missing)}')
    outside, last = [], 0
    for a, b in spans:
        outside.append(text[last:a])
        last = b
    outside.append(text[last:])
    for line in ''.join(outside).splitlines():
        line = code(line)
        for name in names:
            if not re.search(rf'(?<![\w.]){re.escape(name)}\b|\${re.escape(name)}\b', line):
                continue
            if re.search(rf'\bvar {re.escape(name)}\b', line):
                continue   # its declaration
            if f'${name}' in line:
                found.append(f'{name} bound by a control outside the test block: {line.strip()}')
            elif 'Paths.isRelease' not in line:
                found.append(f'{name} used without asking Paths.isRelease: {line.strip()}')
    return found


class TestControlsTestBuildOnly(unittest.TestCase):
    def test_controls_and_values_test_build_only(self):
        self.assertEqual(problems(HOME.read_text()), [])

    def test_check_catches_an_ungated_value(self):
        # the opposite case: the same page with one value no longer gated must fail
        text = HOME.read_text()
        gated = '.environment(\\.depthLayers, !Paths.isRelease && depthLayers)'
        self.assertIn(gated, text)
        broken = text.replace(gated, '.environment(\\.depthLayers, depthLayers)')
        self.assertTrue(any('depthLayers used without asking' in p for p in problems(broken)))

    def test_check_catches_a_control_shown_in_the_release(self):
        text = HOME.read_text()
        spans = release_blocks(text)
        self.assertTrue(spans)
        a, b = spans[0]
        block = text[a:b]
        # the block's body moved out of the `if`: its controls would show in the release
        broken = text[:a] + block[len(OPEN):-1] + text[b:]
        self.assertTrue(problems(broken))


if __name__ == '__main__':
    unittest.main()

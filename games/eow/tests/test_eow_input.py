"""The keyboard, mouse and controller layouts (runtime/keyboard_map.h, runtime/controller_map.h): tests/input_probe.c checks
the C side; the default keyboard layout is the fixed one this game had before it had key bindings (runtime/input.c button(),
which scripted input keeps), and the README's controls table restates it."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
RUNTIME = GAME / "runtime"

# The README's Switch column -> keyboard_map.h's actions (a stick is its four directions).
SWITCH = {"A": ["a"], "B": ["b"], "X": ["x"], "Y": ["y"], "ZL": ["zl"], "ZR": ["zr"], "L": ["l"], "R": ["r"],
          "L3": ["lstick"], "R3": ["rstick"], "+": ["plus"], "−": ["minus"],
          "left stick": ["move_up", "move_left", "move_down", "move_right"],
          "right stick": ["camera_up", "camera_left", "camera_down", "camera_right"],
          "d-pad up": ["up"], "d-pad down": ["down"], "d-pad left": ["left"], "d-pad right": ["right"],
          "capture": ["screenshot"]}   # not a Switch input: the capture button's picture (keyboard_map.h KEYMAP_SCREENSHOT)
# How the README writes an input -> keyboard_map.h's label.
WRITTEN = {"W A S D": ["W", "A", "S", "D"], "arrow keys": ["↑", "←", "↓", "→"]}


def compile_probe(test, tmp):
    probe = Path(tmp) / "input_probe"
    build = subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{RUNTIME}",
                            str(GAME / "tests/input_probe.c"), "-o", str(probe)], capture_output=True, text=True)
    test.assertEqual(build.returncode, 0, build.stderr[-2000:])
    return probe


def readme_table():
    lines = (GAME / "README.md").read_text().splitlines()
    start = lines.index("| Switch | Keys | Controller |")
    rows = []
    for line in lines[start + 2:]:
        if not line.startswith("|"):
            break
        rows.append([cell.strip() for cell in line.strip("|").split("|")])
    return rows


def fixed_layout():
    """runtime/input.c's fixed layout: key code -> FullKey bits (button()), and the stick keys pad_states reads."""
    source = (RUNTIME / "input.c").read_text()
    body = re.search(r"static uint64_t button\(unsigned code\)\{switch\(code\)\{(.*?)default:return 0;\}\}", source).group(1)
    buttons = {}
    for cases, value in re.findall(r"((?:case \d+:)+)return (\d+);", body):
        for code in re.findall(r"case (\d+):", cases):
            buttons[int(code)] = int(value)
    sticks = re.search(r"state\.lx=32767\*\(\(int\)keys\[(\d+)\]-\(int\)keys\[(\d+)\]\);state\.ly=32767\*\(\(int\)keys\[(\d+)\]-\(int\)keys\[(\d+)\]\);"
                       r"state\.rx=32767\*\(\(int\)keys\[(\d+)\]-\(int\)keys\[(\d+)\]\);state\.ry=32767\*\(\(int\)keys\[(\d+)\]-\(int\)keys\[(\d+)\]\);",
                       source).groups()
    axes = {}
    for i, axis in enumerate(("lx", "ly", "rx", "ry")):
        axes[int(sticks[2 * i])] = (axis, 32767)
        axes[int(sticks[2 * i + 1])] = (axis, -32767)
    return buttons, axes


@unittest.skipUnless(shutil.which("xcrun"), "needs the Xcode command line tools")
class InputLayoutTests(unittest.TestCase):
    def test_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = subprocess.run([str(compile_probe(self, tmp))], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS", run.stdout)

    def test_default_is_the_old_fixed_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = subprocess.run([str(compile_probe(self, tmp)), "keys"], capture_output=True, text=True, timeout=30, check=True).stdout
        buttons, axes = fixed_layout()
        self.assertEqual(len(buttons), 18)
        for line in out.splitlines():
            code, bits, lx, ly, rx, ry = map(int, line.split("\t"))
            with self.subTest(code=code):
                self.assertEqual(bits, buttons.get(code, 0))
                want = dict(lx=0, ly=0, rx=0, ry=0)
                if code in axes:
                    want[axes[code][0]] = axes[code][1]
                self.assertEqual(dict(lx=lx, ly=ly, rx=rx, ry=ry), want)

    def test_readme_table_shows_the_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = subprocess.run([str(compile_probe(self, tmp)), "dump"], capture_output=True, text=True, timeout=30, check=True).stdout
        layout = {fields[0]: fields[1:] for fields in (line.split("\t") for line in out.splitlines())}
        covered = set()
        for switch, keys, _controller in readme_table():
            with self.subTest(row=switch):
                names = SWITCH[switch]
                covered.update(names)
                written = []
                for part in [p for p in re.split(r",\s*", keys) if p]:
                    written.extend(WRITTEN.get(part, [part]))
                if len(names) == 4:   # a stick: one input per direction, in the table's order
                    self.assertEqual([layout[n] for n in names], [[i] for i in written], f"{switch}: {keys}")
                else:
                    self.assertEqual(sorted(layout[names[0]]), sorted(written), f"{switch}: {keys}")
        self.assertEqual(covered, set(layout), "every action has a row")


class InputSourceTests(unittest.TestCase):
    def test_scripted_input_keeps_the_fixed_layout(self):
        source = (RUNTIME / "input.c").read_text()
        synthetic = source[source.index("void runtime_input_key_synthetic"):source.index("void runtime_input_key(unsigned")]
        self.assertIn("button(code)", synthetic)
        self.assertNotIn("keymap_apply", synthetic)
        self.assertIn("runtime_settings_toggle()", synthetic)   # key 1 opens the panel from a script too
        for gone in ("runtime_output_mode_cycle", "runtime_aspect_toggle", "mfx_interp", "runtime_shortcut_help_toggle"):
            self.assertNotIn(gone, source)   # keys 2-4 are no shortcuts any more

    def test_no_other_game_includes(self):
        # games/eow is self-contained: its build reads shared/ through its own links, never another game's folder.
        for path in [RUNTIME / "settings_game.h", RUNTIME / "bindings_diagram.h", RUNTIME / "keyboard_map.h", RUNTIME / "frontend.m",
                     RUNTIME / "input.c", RUNTIME / "gamepad.m"]:
            for line in path.read_text().splitlines():
                if line.startswith("#include") or line.startswith("#import"):
                    target = re.match(r'#(?:include|import)\s*["<]([^">]+)', line).group(1)   # the path, not the comment
                    self.assertNotIn("games/", target, f"{path.name}: {line}")
                    self.assertNotIn("../../", target, f"{path.name}: {line}")


if __name__ == "__main__":
    unittest.main()

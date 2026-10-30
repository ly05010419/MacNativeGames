"""The keyboard, mouse and controller layouts (runtime/keyboard_map.h, runtime/controller_map.h) and the settings panel's
choices: tests/input_probe.c checks the C side; the README's controls table, the panel's language list and its rows are
checked against the sources they restate."""
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
          "d-pad up": ["up"], "d-pad down": ["down"], "d-pad left": ["left"], "d-pad right": ["right"]}
# How the README writes an input -> keyboard_map.h's label.
WRITTEN = {"W A S D": ["W", "A", "S", "D"], "arrow keys": ["↑", "←", "↓", "→"], "left button": ["Left click"],
           "right button": ["Right click"], "wheel click": ["Wheel click"], "side button 4": ["Mouse 4"],
           "side button 5": ["Mouse 5"]}


def compile_probe(test, tmp):
    probe = Path(tmp) / "input_probe"
    build = subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{RUNTIME}",
                            str(GAME / "tests/input_probe.c"), "-o", str(probe)], capture_output=True, text=True)
    test.assertEqual(build.returncode, 0, build.stderr[-2000:])
    return probe


def readme_table():
    lines = (GAME / "README.md").read_text().splitlines()
    start = lines.index("| Action | Switch | Keys | Mouse |")
    rows = []
    for line in lines[start + 2:]:
        if not line.startswith("|"):
            break
        rows.append([cell.strip() for cell in line.strip("|").split("|")])
    return rows


@unittest.skipUnless(shutil.which("xcrun"), "needs the Xcode command line tools")
class InputLayoutTests(unittest.TestCase):
    def test_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = subprocess.run([str(compile_probe(self, tmp))], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS", run.stdout)

    def test_readme_table_shows_the_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = subprocess.run([str(compile_probe(self, tmp)), "dump"], capture_output=True, text=True, timeout=30, check=True).stdout
        layout = {fields[0]: fields[1:] for fields in (line.split("\t") for line in out.splitlines())}
        covered = set()
        for action, switch, keys, mouse in readme_table():
            with self.subTest(row=action):
                names = SWITCH[switch]
                covered.update(names)
                written = []
                for part in [p for p in re.split(r",\s*", keys) + [mouse] if p]:
                    written.extend(WRITTEN.get(part, [part]))
                if len(names) == 4:   # a stick: one input per direction, in the table's order
                    self.assertEqual([layout[n] for n in names], [[i] for i in written], f"{action}: {keys}")
                else:
                    self.assertEqual(sorted(layout[names[0]]), sorted(written), f"{action}: {keys} / {mouse}")
        self.assertEqual(covered, set(layout), "every action has a row")


class SettingsPanelSourceTests(unittest.TestCase):
    panel = (RUNTIME / "settings_panel.h").read_text()

    def test_languages_are_codes_the_runtime_accepts(self):
        table = self.panel[self.panel.index("settings_languages[][2]="):self.panel.index("enum { SETTINGS_LANGUAGES")]
        offered = re.findall(r'\{"([a-zA-Z-]+)","', table)
        services = (RUNTIME / "services.c").read_text()
        known = re.search(r"known\[\]=\{([^}]*)\}", services).group(1)
        accepted = re.findall(r'"([a-zA-Z-]+)"', known)
        system = re.findall(r'"([a-zA-Z-]+)"', re.search(r"language_codes\[\] = \{([^}]*)\}",
                                                          (RUNTIME / "system_services.c").read_text()).group(1))
        self.assertEqual(sorted(offered), sorted(accepted), "the panel offers exactly what services.c reads back")
        self.assertFalse(set(accepted) - set(system), "every language is one the game's settings service lists")

    def test_no_contribution_or_star(self):
        # ACNH has no shader-list contribution flow; its panel carries neither that action nor the project-star link.
        for word in ("contribute:", "star:", "TOTK_REPO_URL", "contrib_submit", "totk_shader_contrib"):
            self.assertNotIn(word, self.panel)

    def test_no_other_game_includes(self):
        # games/acnh is self-contained: its build does not read another game's implementation.
        for path in [RUNTIME / "settings_panel.h", RUNTIME / "bindings_diagram.h", RUNTIME / "keyboard_map.h", RUNTIME / "frontend.m"]:
            for line in path.read_text().splitlines():
                if line.startswith("#include") or line.startswith("#import"):
                    self.assertNotIn("games/", line, f"{path.name}: {line}")
                    self.assertNotIn("../../", line, f"{path.name}: {line}")

    def test_scripted_input_keeps_the_fixed_layout(self):
        source = (RUNTIME / "input.c").read_text()
        synthetic = source[source.index("void runtime_input_key_synthetic"):source.index("void runtime_input_key(unsigned")]
        self.assertIn("button(code)", synthetic)
        self.assertNotIn("keymap_apply", synthetic)


if __name__ == "__main__":
    unittest.main()

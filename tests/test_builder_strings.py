"""tools/builder/strings.py (S2.1, T1-T2): text() and the drift test of the Controls page's texts. The ids are read
from the sources with ast, not listed by hand: every literal id MNG Launcher's Controls page (tools/builder_jobs/
tab_controls.py) uses has its English text, every id the page builds from a model's word (a save's refusal, an icons
reason or status, a controller type or answer, a custom button's mode) has one for each word the model can give, and
every English text has the page's Chinese beside it (so no text is left behind for a page that no longer exists)."""
import ast
import inspect
from pathlib import Path
import unittest

from tools.builder import controller_controls, controller_icons, controls_file as cf
from tools.builder.strings import EN, text
from tools.builder_jobs import tab_controls

REPO = Path(__file__).resolve().parents[1]
PAGE = REPO / 'tools/builder_jobs/tab_controls.py'
PREFIX = 'gui.controls.'


def literal_ids(source):
    """Every string constant of source that is a whole text id (gui.controls.<...>, no format field)."""
    return {node.value for node in ast.walk(ast.parse(source))
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.startswith(PREFIX)
            and '{' not in node.value and not node.value.endswith('.')}


def reasons_of(function, result):
    """The literal reason words a model function gives: the second argument of result(False, '<reason>', ...), and the
    words of a `why = 'a' if ... else 'b' if ... else ''` chain."""
    found = set()
    for node in ast.walk(ast.parse(inspect.getsource(function).lstrip())):
        if isinstance(node, ast.Call) and getattr(node.func, 'id', None) == result and len(node.args) > 1 \
                and isinstance(node.args[1], ast.Constant):
            found.add(node.args[1].value)
        elif isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', None) == 'why':
            chain = node.value
            while isinstance(chain, ast.IfExp):
                found.add(chain.body.value)
                chain = chain.orelse
            found.add(chain.value)
    return found - {''}


class Drift(unittest.TestCase):
    def test_every_literal_id_of_the_page_has_a_text(self):
        ids = literal_ids(PAGE.read_text(encoding='utf-8'))
        self.assertGreater(len(ids), 40)
        self.assertEqual(ids - set(EN), set())

    def test_every_word_a_model_gives_has_a_text(self):
        saves = reasons_of(controller_controls.save, 'SaveResult') | reasons_of(controller_controls.restore, 'SaveResult')
        self.assertEqual(saves, {'other_game', 'running', 'changed', 'refused'})
        icons = reasons_of(controller_icons.icons_info, 'IconsInfo')
        self.assertEqual(icons, {'not_offered', 'no_app', 'not_latest', 'broken_file', 'ask', 'nintendo'})
        wanted = {f'not_saved.{r}' for r in saves} | {f'icons.not.{r}' for r in icons}
        wanted |= {f'icons.{s}' for s in ('none', 'current', 'outdated')}           # icons_mod.status
        wanted |= {f'icons.download.{e}' for e in ('missing', 'broken')}          # icons_env.status but 'ready'
        wanted |= {f'mode.{t}' for t in cf.TYPES if t != 'auto'} | {f'mode.auto.{a}' for a in cf.ANSWERS}
        wanted |= {f'answer.{a}' for a in cf.ANSWERS}
        wanted |= {f'custom.{m}' for m in ('tap', 'toggle', 'aim', 'none', 'shared')}   # controls_file.layout's
        self.assertEqual({PREFIX + w for w in wanted} - set(EN), set())

    def test_every_text_has_the_page_s_chinese(self):
        self.assertEqual(set(EN) ^ set(tab_controls.ZH), set())

    def test_a_missing_id_is_reported(self):
        source = "say('gui.controls.nope')\nsay(f'gui.controls.{k}')\nx = 'gui.controls.title'\n"
        self.assertEqual(literal_ids(source) - set(EN), {'gui.controls.nope'})


class Text(unittest.TestCase):
    def test_with_and_without_values(self):
        self.assertEqual(text('gui.controls.title', name='Breath of the Wild'), 'Breath of the Wild · controls')
        self.assertEqual(text('gui.controls.save'), 'Save controls')

    def test_unknown_key(self):
        with self.assertRaises(KeyError):
            text('gui.controls.no_such_id')


if __name__ == '__main__':
    unittest.main()

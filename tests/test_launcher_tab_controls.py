"""MNG Launcher's Controls page (tools/builder_jobs/tab_controls.py): the old window's Controls tab over the same models
(controller_controls, controls_file, controller_icons, keyboard_layout), one JSON answer per call, the pending layout
carried by the window with the token of the files it was read from, and writes only under the home given."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_launcher_tabs import fake_app  # noqa: E402
from tools.builder import controller_icons, controls_file as cf, keyboard_layout as kb  # noqa: E402
from tools.builder.strings import EN  # noqa: E402
from tools.builder_jobs import tab_controls, tabs  # noqa: E402


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.app = fake_app(self.tmp)
        self.data = self.home / 'Library/Application Support/MacNativeGames/TOTK'
        os.environ.pop('MNG_LANG', None)

    def call(self, action, **args):
        answer = tabs.call('controls', action, self.app, args, home=self.home)
        self.assertTrue(answer['ok'], answer)
        return answer['result']

    def answered(self):
        return self.call('answer', controller='xbox')


class Layout(Base):
    def test_it_asks_which_controller_first_and_writes_the_answer_under_the_home(self):
        first = self.call('state')
        self.assertTrue(first['which'] and first['ask'])
        self.assertEqual(first['rows'], [])
        view = self.answered()
        self.assertFalse(view['which'])
        self.assertIn('auto, read for an Xbox', view['mode'])
        self.assertEqual([r['input'] for r in view['rows']], list(tab_controls.ORDER))
        self.assertEqual(len(view['buttons']), 16)
        self.assertEqual([p.relative_to(self.home).as_posix() for p in self.home.rglob('*') if p.is_file()],
                         ['Library/Application Support/MacNativeGames/Builder/controls-tab.json'])

    def test_swap_save_read_restore(self):
        view = self.answered()
        self.assertEqual(view['desired']['south'], 'b')
        view = self.call('swap', token=view['token'], desired=view['desired'], converted=view['converted'],
                         input='x', button='south')
        self.assertTrue(view['pending'])
        self.assertEqual((view['desired']['south'], view['desired']['north']), ('x', 'b'))
        self.assertIn('→ A', {r['input']: r['shown'] for r in view['rows']}['x'])
        saved = self.call('save', token=view['token'], desired=view['desired'], converted=view['converted'])
        self.assertTrue(saved['ok'] and not saved['pending'] and saved['on_disk'])
        self.assertIn('The game reads it at its next start', saved['message'])
        text = (self.data / 'controls.txt').read_bytes()
        self.assertEqual(cf.layout(cf.parse(text), False)[0], view['desired'])
        again = self.call('state')
        self.assertEqual(again['desired'], view['desired'])
        restored = self.call('restore', token=again['token'])
        self.assertTrue(restored['ok'])
        self.assertFalse((self.data / 'controls.txt').exists())
        self.assertEqual(len(list(self.data.glob('controls.txt.before-*'))), 1)
        self.assertEqual(restored['desired']['south'], 'b')

    def test_a_token_of_other_files_drops_the_pending_layout_and_save_is_refused(self):
        view = self.answered()
        pending = self.call('swap', token=view['token'], desired=view['desired'], converted=[], input='x',
                            button='south')
        self.data.mkdir(parents=True, exist_ok=True)
        (self.data / 'controls.txt').write_text('view = minus\n')
        refused = self.call('save', token=pending['token'], desired=pending['desired'], converted=[])
        self.assertFalse(refused['ok'])
        self.assertIn('changed meanwhile', refused['message'])
        self.assertFalse(refused['pending'])
        self.assertEqual((self.data / 'controls.txt').read_text(), 'view = minus\n')

    def test_a_running_game_refuses_save_and_keeps_the_pending_layout(self):
        view = self.answered()
        pending = self.call('swap', token=view['token'], desired=view['desired'], converted=[], input='x',
                            button='south')
        with patch('tools.builder.game_state.game_running', return_value=True):
            refused = self.call('save', token=pending['token'], desired=pending['desired'], converted=[])
        self.assertFalse(refused['ok'])
        self.assertIn('the game is running', refused['message'])
        self.assertEqual(refused['desired'], pending['desired'])
        self.assertFalse((self.data / 'controls.txt').exists())

    def test_custom_buttons_are_converted_only_once_confirmed(self):
        self.data.mkdir(parents=True)
        (self.data / 'controls.txt').write_text('l1 = l tap\n')
        view = self.answered()
        self.assertEqual({b['id']: b['custom'] for b in view['buttons']}['l1'], 'tap')
        ask = self.call('swap', token=view['token'], desired=view['desired'], converted=[], input='x', button='l1')
        self.assertIn('plain buttons', ask['confirm'])
        self.assertEqual((ask['convert'], ask['desired']), (['l1'], view['desired']))
        done = self.call('swap', token=view['token'], desired=view['desired'], converted=[], input='x', button='l1',
                         confirmed=True)
        self.assertEqual((done['converted'], done['desired']['l1'], done['confirm']), (['l1'], 'x', ''))
        saved = self.call('save', token=done['token'], desired=done['desired'], converted=done['converted'])
        self.assertTrue(saved['ok'], saved['message'])
        self.assertNotIn('tap', cf.HEADER.decode() + (self.data / 'controls.txt').read_text().split(cf.HEADER.decode())[1])

    def test_xbox_letters_and_nothing_more_to_do(self):
        view = self.answered()
        letters = self.call('xbox_letters', token=view['token'], desired=view['desired'], converted=[])
        self.assertEqual([letters['desired'][d] for d in ('south', 'east', 'west', 'north')], ['a', 'b', 'x', 'y'])
        self.assertIn('not saved', letters['message'])
        same = self.call('xbox_letters', token=view['token'], desired=letters['desired'], converted=[])
        self.assertIn('already', same['message'])

    def test_a_file_the_game_refuses_is_shown_and_can_be_restored(self):
        self.data.mkdir(parents=True)
        (self.data / 'controls.txt').write_text('south = nothing\n')
        view = self.answered()
        self.assertIn('line 1', view['broken'])
        self.assertEqual((view['rows'], view['on_disk']), ([], True))
        self.assertTrue(self.call('restore', token=view['token'])['ok'])

    def test_bad_layouts_from_the_window_are_refused_in_words(self):
        view = self.answered()
        answer = tabs.call('controls', 'save', self.app, {'token': view['token'], 'desired': {'south': 'a'}},
                           home=self.home)
        self.assertFalse(answer['ok'])
        self.assertIn('desired', answer['error'])
        self.assertFalse(tabs.call('controls', 'answer', self.app, {'controller': 'sega'}, home=self.home)['ok'])


class Texts(unittest.TestCase):
    def test_every_chinese_text_is_an_old_english_one_with_the_same_fields(self):
        fields = lambda s: sorted(re.findall(r'\{(\w+)\}', s))  # noqa: E731
        for key, zh in tab_controls.ZH.items():
            self.assertIn(key, EN)
            self.assertEqual(fields(zh), fields(EN[key]), key)
        old = {k for k in EN if k.startswith('gui.controls.')}
        self.assertEqual(old - set(tab_controls.ZH), set())

    def test_chinese_when_asked(self):
        with patch.dict(os.environ, {'MNG_LANG': 'zh'}):
            self.assertEqual(tab_controls.say('gui.controls.save'), '保存按键')
            self.assertEqual(tab_controls.word('Jump'), '跳跃')
        self.assertEqual(tab_controls.say('gui.controls.save'), 'Save controls')


class Keyboard(Base):
    def test_the_key_codes_are_the_runtime_s_and_name_every_key_input(self):
        codes = tab_controls.key_codes(REPO)
        self.assertEqual(set(codes.values()), {n for n in kb.INPUTS if not n.startswith('mouse')})
        self.assertEqual((codes[49], codes[0], codes[56]), ('space', 'a', 'shift'))

    def test_defaults_then_the_game_s_keyboard_txt(self):
        view = self.call('keyboard')
        self.assertIn('no keyboard.txt', view['note'])
        self.assertEqual(view['keys']['49']['actions'], ['X (jump)'])
        self.assertEqual(view['keys']['60']['actions'], ['B (cancel)'])      # right shift: its left twin's
        self.data.mkdir(parents=True)
        (self.data / 'keyboard.txt').write_text('x = j\n')
        view = self.call('keyboard')
        self.assertEqual((view['note'], view['source']), ('', str(self.data / 'keyboard.txt')))
        self.assertEqual(view['keys']['38']['actions'], ['X (jump)'])
        self.assertEqual(view['keys']['49']['actions'], [])
        self.assertEqual({a['id']: a['keys'] for a in view['actions']}['x'], ['J'])
        self.assertEqual(view['formats']['actions'], '{key} -> {actions}')

    def test_a_bad_keyboard_txt_says_so(self):
        self.data.mkdir(parents=True)
        (self.data / 'keyboard.txt').write_text('jump = space\n')
        self.assertIn('unknown action', self.call('keyboard')['note'])


class Icons(Base):
    def test_the_icons_line_asks_for_the_controller_first(self):
        info = self.call('icons_info')
        self.assertEqual((info['offered'], info['reason']), (False, 'ask'))
        self.assertIn('say which controller', info['line'])
        made = self.call('icons')
        self.assertFalse(made['ok'])

    def test_icons_are_refused_when_the_mods_folder_is_outside_the_home(self):
        ready = controller_icons.IconsInfo(True, '', 'ready', 'none')
        with patch.object(controller_icons, 'icons_info', return_value=ready), \
                patch.object(controller_icons, 'make_icons') as make, \
                patch('tools.builder_jobs.tab_controls.cm._library') as library:
            library.return_value.load = Path('/elsewhere/mods/v143')
            answer = tabs.call('controls', 'icons', self.app, {}, home=self.home)
        self.assertFalse(answer['ok'])
        self.assertIn('not under', answer['error'])
        make.assert_not_called()

    def test_icons_need_the_download_and_the_font_mods_agreed(self):
        missing = controller_icons.IconsInfo(True, '', 'missing', 'none', (('xbox-icons', 'Xbox icons'),))
        with patch.object(controller_icons, 'icons_info', return_value=missing), \
                patch.object(controller_icons, 'make_icons') as make:
            self.assertIn('fontTools 4.67.0', self.call('icons')['message'])
            self.assertIn('still replace', self.call('icons', install_env=True)['message'])
        make.assert_not_called()


class CommandLine(Base):
    def test_one_json_object_and_chinese(self):
        env = dict(os.environ, MNG_HOME=str(self.home), HOME=str(self.home), PYTHONPATH=str(REPO), MNG_LANG='zh')
        done = subprocess.run([sys.executable, '-m', 'tools.builder_jobs', 'tab', 'controls', 'answer', '--app',
                               str(self.app), '--args', json.dumps({'controller': 'nintendo'})],
                              cwd=REPO, env=env, capture_output=True, text=True)
        answer = json.loads(done.stdout)
        self.assertTrue(answer['ok'], answer)
        self.assertIn('任天堂', answer['result']['mode'])
        self.assertEqual(answer['result']['rows'][0]['name'], '动作')


if __name__ == '__main__':
    unittest.main()

"""The player's contribution file (totk_shader_contrib.h): written by the runtime, merged by scripts/shader_list.py."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
sys.path.insert(0, str(TOTK / 'scripts'))
from shader_list import ShaderList, canonical  # noqa: E402
from prepare_shader_seed import answers_for, suffix  # noqa: E402


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class ContributionFileTests(unittest.TestCase):
    def test_probe_writes_rows_the_shared_list_merges(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); probe = tmp / 'probe'; contrib = tmp / 'shader-list-contribution.txt'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    f'-I{TOTK}/third_party/d2r_gfx/gfx/nvn', str(TOTK / 'tests/shader_contrib/probe.m'),
                                    '-framework', 'Foundation', '-o', str(probe)], capture_output=True, text=True)
            if build.returncode:
                self.skipTest(f'clang could not build the probe: {build.stderr[-300:]}')
            run = subprocess.run([str(probe), str(contrib)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(run.stdout)['status'], 'pass')
            rows = [line.split('\t') for line in contrib.read_text().splitlines() if line.startswith('V\t')]
            self.assertTrue(rows and all(canonical(json.loads(deps)) == deps for _, _, deps in rows), 'deps as canonical() writes them')
            shared = ShaderList()
            shared.add_rows(contrib, 'm')
            self.assertEqual(len(shared.variants), 2 * 100 + 60)   # TOTK_CONTRIB_STEP 100
            self.assertEqual([row[0] for row in shared.warm], ['R', 'B'])   # render pipelines merge with the shaders
            deps = {'tex': {'9': '*', '10': '*_depth', '3': 'cube'}, 'attr': {'6': 'uint'}, 'fout': {'0': 'float'}}
            self.assertEqual(json.loads(run.stdout)['listed'], f'{7:064x}-' + suffix(answers_for(deps)),
                             'a pipeline names its shaders as prepare_shader_seed.py names list rows')
            noseed = subprocess.run([str(probe), str(contrib), 'noseed'], capture_output=True, text=True)
            self.assertEqual(noseed.returncode, 0, noseed.stderr)
            self.assertEqual(json.loads(noseed.stdout)['status'], 'pass')
            self.assertFalse(contrib.exists(), 'a run without the seed writes no contribution file')


if __name__ == '__main__':
    unittest.main()


import collect_shader_issues  # noqa: E402
from collect_shader_issues import (attachment_urls, contribution_pipelines, contribution_rows, issue_urls, key_source,  # noqa: E402
                                   reply_text, route)


class IssueCollectionTests(unittest.TestCase):
    KEY_A, KEY_B = 'a' * 64, 'b' * 64

    def test_attachment_links_in_order_without_repeats(self):
        body = ('### shader-list-contribution.txt\n\n[shader-list-contribution.txt](https://github.com/user-attachments/files/123/'
                'shader-list-contribution.txt)\nagain: https://github.com/user-attachments/files/123/shader-list-contribution.txt '
                'old style: https://github.com/someone/MacNativeGames/files/456/contrib.txt).\nnot one: https://example.com/files/1/x.txt')
        self.assertEqual(attachment_urls(body), ['https://github.com/user-attachments/files/123/shader-list-contribution.txt',
                                                 'https://github.com/someone/MacNativeGames/files/456/contrib.txt'])
        self.assertEqual(attachment_urls(None), [])

    def test_an_automatic_submission_is_read_like_a_form_issue(self):
        link = watch.gist_raw('https://gist.github.com/someone/0123abcd\n')
        self.assertEqual(link, 'https://gist.githubusercontent.com/someone/0123abcd/raw/shader-list-contribution.txt')
        self.assertIsNone(watch.gist_raw('error: HTTP 401'))
        body = watch.issue_body('iPad', link)
        self.assertEqual(attachment_urls(body), [link])
        self.assertIn('### Device / 设备\n\niPad\n', body)
        self.assertNotIn('udid', body.lower())

    def test_only_shader_rows_are_taken(self):
        text = '\n'.join([
            '# header', '',
            f'V\t{self.KEY_A}\t{{"tex":{{"9":"2d","10":"3d"}}}}',     # canonicalised
            f'V\t{self.KEY_A}\t{{"tex":{{"10":"3d","9":"2d"}}}}',     # the same row
            f'V\t{self.KEY_B}\t{{}}',
            f'S\t{self.KEY_A}-0123456789ab_b\t0\tm',                  # shader libraries are not taken from contributions
            f'L\t{self.KEY_A}\tShader/x.bnsh\t0\t880\t100',            # nor locators
            'R\tabcd', 'R\tabcd', 'B\tabcd\t-\t3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;',   # render pipelines are (a row once)
            'T\tabcd\t5:3;7:1;', 'T\tabcd\t5:9;',                   # and tile reads (an attachment past 7 is none)
            'R\tnot hex', 'B\tabcd\t-\t-',
            'V\tnot-a-key\t{}', f'V\t{self.KEY_B}\t{{"local":[1]}}', f'V\t{self.KEY_B}\tnot json'])
        rows, dropped = contribution_rows(text)
        self.assertEqual(rows, {(self.KEY_A, '{"tex":{"10":"3d","9":"2d"}}'), (self.KEY_B, '{}')})
        self.assertEqual(dropped, 8)
        self.assertEqual(contribution_pipelines(text), [('R', 'abcd'), ('B', 'abcd', '-', '3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;'),
                                                        ('T', 'abcd', '5:3;7:1;')])

    @staticmethod
    def pipeline(vs, fs):   # a pipeline key leads with two NUL-padded 80-byte names "<variant>_b"
        name = lambda k: (f'{k}-0123456789ab_b'.encode() if k else b'').ljust(80, b'\0')
        return ('R', (name(vs) + name(fs) + bytes(16)).hex())

    def test_rows_go_to_the_game_their_shader_key_belongs_to(self):
        # one contribution file serves both games, so an issue titled for one can carry the other's rows (2026-10-25)
        key_c = 'c' * 64
        keys = {'totk': {self.KEY_A}, 'botw': {self.KEY_B}}
        rows = {(self.KEY_A, '{}'), (self.KEY_B, '{}'), (key_c, '{}')}
        own, mixed, foreign, compute = (self.pipeline(self.KEY_A, self.KEY_A), self.pipeline(self.KEY_A, self.KEY_B),
                                        self.pipeline(key_c, key_c), self.pipeline(self.KEY_B, None))
        games, unknown = route(rows, [own, mixed, foreign, compute], keys)
        self.assertEqual(games['totk'], ({(self.KEY_A, '{}')}, [own]))
        self.assertEqual(games['botw'], ({(self.KEY_B, '{}')}, [compute]))
        self.assertEqual(unknown, ({(key_c, '{}')}, [mixed, foreign]))

    def test_shader_keys_come_from_a_seed_or_a_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            seed = Path(tmp) / 'botw160'; seed.mkdir()
            (seed / 'shader-keys.json').write_text(json.dumps({'version': 'botw160', 'keys': [self.KEY_B]}))
            self.assertEqual(key_source(str(seed)), ('botw', {self.KEY_B}))
            listed = Path(tmp) / 'v121-keys.json'; listed.write_text(json.dumps([self.KEY_A]))
            self.assertEqual(key_source(f'totk={listed}'), ('totk', {self.KEY_A}))
            old = Path(tmp) / 'v0'; old.mkdir(); (old / f'{self.KEY_A}.variants').write_text('')
            with self.assertRaises(ValueError):   # an old seed names only its startup shaders: not every key of the game
                key_source(str(old))
            with self.assertRaises(ValueError):
                key_source(f'odyssey={listed}')

    def test_attachments_in_comments_are_taken(self):
        issue = {'body': 'upload failed', 'comments': [{'body': 'here: https://github.com/user-attachments/files/9/a.txt'}]}
        self.assertEqual(issue_urls(issue), ['https://github.com/user-attachments/files/9/a.txt'])

    def test_a_large_contribution_is_read(self):
        # contributions carry render pipelines too: a 1.2.1 file had 10,188 rows, over the old 8 MiB (#435)
        self.assertGreaterEqual(collect_shader_issues.MAX_BYTES, 64 << 20)

    def test_the_reply_names_each_game_it_merged_into(self):
        self.assertIn('15 of them new', reply_text({'totk': (100, 15)}, 'totk'))
        mixed = reply_text({'totk': (54, 0), 'botw': (436, 2)}, 'botw')
        self.assertIn("Tears of the Kingdom: 54 rows, 0 new; Breath of the Wild: 436 rows, 2 new", mixed)
        self.assertIn('王国之泪 54 条，其中新增 0 条；旷野之息 436 条，其中新增 2 条', mixed)
        self.assertIn('None of the shaders', reply_text({}, 'totk'))   # --drop-unknown on another game's file


class OtherGamesTests(unittest.TestCase):
    """Every other game's rows go to its own list (user 2026-10-28: "每个游戏支持自己的着色器清单 收到自己游戏里 不要混了")."""
    KEY_A, KEY_W = 'a' * 64, 'e' * 64

    @staticmethod
    def issue(title, body='', number=1):
        return {'number': number, 'title': title, 'body': body, 'labels': [], 'comments': [], 'author': {'login': 'p'}}

    def test_the_game_comes_from_the_title_the_form_or_the_data_folder(self):
        titled = collect_shader_issues.titled_game
        self.assertEqual(titled(self.issue('Shader list: 120 shaders from Mac (TOTK 1.4.3)')), 'totk')
        self.assertEqual(titled(self.issue('Shader list (Breath of the Wild): 3 shaders')), 'botw')
        self.assertEqual(titled(self.issue('Shader list (Super Mario Bros. Wonder): 40 shaders from Mac (Wonder 1.2.1)')), 'wonder')
        self.assertEqual(titled(self.issue('Shader list (other games): ', '### Game / 游戏\n\nMario vs. Donkey Kong\n')), 'mvdk')
        self.assertEqual(titled(self.issue('Shader list (other games): ', '### Game / 游戏\n\n超级马力欧 奥德赛\n')), 'odyssey')
        # sent before the games had their own titles: the launcher's "(<data folder> <version>)"
        self.assertEqual(titled(self.issue('Shader list: 1365 shaders from Mac (Wonder 1.2.1)')), 'wonder')
        self.assertEqual(titled(self.issue('Shader list: 915 shaders from Mac (SMO 1.3.0)')), 'odyssey')
        self.assertEqual(titled(self.issue('Shader list: 13 shaders from Mac (SSBU 13.0.2)')), 'ssbu')
        self.assertEqual(titled(self.issue('Shader list: Wonder')), 'wonder')   # #1001: the game named in a typed title
        self.assertEqual(titled(self.issue('Shader list: my iPhone, 2 hours')), 'totk')

    def test_rows_pasted_into_the_issue_are_read(self):
        body = f'notes\n```tsv\nV\t{self.KEY_A}\t{{}}\nR\tabcd\nnot a row\n```'
        self.assertEqual(collect_shader_issues.inline_text(self.issue('Shader list:', body)), f'V\t{self.KEY_A}\t{{}}\nR\tabcd')
        self.assertEqual(collect_shader_issues.inline_text(self.issue('Shader list:', 'Failed to upload')), '')

    def merge(self, issue, keys):
        lists = {g: ShaderList() for g in collect_shader_issues.GAMES}
        line, per_game = collect_shader_issues.merge_issue(issue, lists, keys, {}, '', False)
        return lists, line, per_game

    def test_another_game_takes_every_row_no_seeded_game_has(self):
        keys = {g: set() for g in collect_shader_issues.GAMES}; keys['totk'] = {self.KEY_A}
        body = f'```\nV\t{self.KEY_A}\t{{}}\nV\t{self.KEY_W}\t{{}}\n```'   # a file shared with Tears of the Kingdom
        lists, line, per_game = self.merge(self.issue('Shader list (Super Mario Bros. Wonder): 2 shaders', body), keys)
        self.assertEqual(lists['wonder'].variants, {(self.KEY_W, '{}')})
        self.assertEqual(lists['totk'].variants, {(self.KEY_A, '{}')})
        self.assertEqual(per_game, {'totk': (1, 1), 'wonder': (1, 1)})
        self.assertIn(self.KEY_W, keys['wonder'], 'what it merged teaches later issues the game\'s keys')
        # a plain "Shader list:" issue then finds those keys in Wonder's list, and what no game has is Wonder's too: a file
        # comes from one game, and the seeds name every Tears of the Kingdom and Breath of the Wild shader
        later = self.issue('Shader list:', f'V\t{self.KEY_W}\t{{"tex":{{"1":"2d"}}}}\nV\t{"f" * 64}\t{{}}')
        lists, line, per_game = self.merge(later, keys)
        self.assertEqual(lists['wonder'].variants, {(self.KEY_W, '{"tex":{"1":"2d"}}'), ('f' * 64, '{}')})
        self.assertEqual(per_game, {'wonder': (2, 2)})
        # nothing names another game: an unknown key keeps the issue open
        lists, line, per_game = self.merge(self.issue('Shader list:', f'V\t{"9" * 64}\t{{}}'), keys)
        self.assertIsNone(per_game)
        self.assertIn('neither game has', line)

    def test_another_game_s_reply_names_its_own_list(self):
        reply = reply_text({'wonder': (40, 12)}, 'wonder')
        self.assertIn("12 of them new to Super Mario Bros. Wonder's own shader list (`games/wonder/assets/shader_list.tsv`)", reply)
        self.assertIn('超级马力欧兄弟 惊奇自己的着色器清单', reply)
        self.assertNotIn('next build', reply)   # its build may not read the list yet

    def test_another_game_s_list_keeps_its_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'odyssey/assets/shader_list.tsv'
            saved = collect_shader_issues.GAMES['odyssey']
            collect_shader_issues.GAMES['odyssey'] = (path,) + saved[1:]
            try:
                shared = ShaderList(); shared.variants.add((self.KEY_W, '{}'))
                collect_shader_issues.save_list('odyssey', shared)   # a new list: a header naming the game
                self.assertTrue(path.read_text().startswith('# Super Mario Odyssey shader list v1'))
                path.write_text('# Odyssey list, chain v4\n' + path.read_text().split('\n', 2)[2])
                shared.variants.add((self.KEY_A, '{}'))
                collect_shader_issues.save_list('odyssey', shared)
                text = path.read_text()
                self.assertTrue(text.startswith('# Odyssey list, chain v4\nV\t'), text[:80])
                self.assertEqual(len(ShaderList.load(path).variants), 2)
            finally:
                collect_shader_issues.GAMES['odyssey'] = saved


sys.path.insert(0, str(TOTK / 'ios/tools'))
import shader_contrib_watch as watch  # noqa: E402


class MacReminderForDevicesTests(unittest.TestCase):
    def test_only_paired_reachable_ios_devices(self):
        listing = {'result': {'devices': [
            {'identifier': 'A', 'deviceProperties': {'name': 'iPhone'}, 'hardwareProperties': {'platform': 'iOS', 'udid': 'u-a'},
             'connectionProperties': {'pairingState': 'paired', 'transportType': 'localNetwork', 'tunnelState': 'disconnected'}},
            {'identifier': 'B', 'deviceProperties': {'name': 'iPad'}, 'hardwareProperties': {'platform': 'iOS', 'udid': 'u-b'},
             'connectionProperties': {'pairingState': 'paired'}},                                   # not reachable now
            {'identifier': 'E', 'deviceProperties': {'name': 'Tablet'}, 'hardwareProperties': {'platform': 'iOS', 'udid': 'u-e', 'deviceType': 'iPad'},
             'connectionProperties': {'pairingState': 'paired', 'transportType': 'wired'}},
            {'identifier': 'C', 'hardwareProperties': {'platform': 'watchOS'}, 'connectionProperties': {'pairingState': 'paired', 'transportType': 'wired'}},
            {'identifier': 'D', 'hardwareProperties': {'platform': 'iOS'}, 'connectionProperties': {'pairingState': 'unpaired', 'transportType': 'wired'}}]}}
        self.assertEqual(watch.reachable_devices(listing), [('A', 'u-a', 'iPhone', 'iPhone'), ('E', 'u-e', 'Tablet', 'iPad')])
        self.assertEqual(watch.reachable_devices({}), [])

    def test_the_game_is_found_by_name_or_bundle(self):
        self.assertEqual(watch.game_bundle({'result': {'apps': [{'name': 'Other', 'bundleIdentifier': 'x.y'},
                                                                 {'name': 'Tears of the Kingdom', 'bundleIdentifier': 'com.someone.totk'}]}}), 'com.someone.totk')
        self.assertEqual(watch.game_bundle({'result': {'apps': [{'name': 'TOTK', 'bundleIdentifier': 'org.macnativegames.totk'}]}}), 'org.macnativegames.totk')
        self.assertIsNone(watch.game_bundle({'result': {'apps': [{'name': 'Other', 'bundleIdentifier': 'x.y'}]}}))

    def test_rows_and_every_100(self):
        text = '# header\n' + ''.join(f'V\t{i:064x}\t{{}}\n' for i in range(3)) + f'V\t{0:064x}\t{{}}\n' + 'R\tab\n' + 'R\tab\n'
        self.assertEqual(watch.contribution_rows(text), 4)   # shaders and pipelines together
        self.assertFalse(watch.due(99, 0)); self.assertTrue(watch.due(100, 0))
        self.assertFalse(watch.due(199, 100)); self.assertTrue(watch.due(200, 100))

    def test_a_device_s_rows_are_merged_into_the_one_file(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); phone, mine = d / 'phone.txt', d / 'shader-list-contribution.txt'
            mine.write_text('# Mac\nR\tab\nV\t' + 'a' * 64 + '\t{}\n')
            phone.write_text('# phone\nR\tab\nR\tcd\nR\tcd\nB\tcd\t-\t-\n')
            self.assertEqual(watch.merge_rows(phone, mine), 4)      # ab and the V row, then cd and its B row once
            self.assertEqual(watch.merge_rows(phone, mine), 4)      # nothing new the second time
            fresh = d / 'new.txt'
            self.assertEqual(watch.merge_rows(phone, fresh), 3)
            self.assertTrue(fresh.read_text().startswith('# '))

    def test_agent_runs_check_every_ten_minutes_with_the_xcode(self):
        plist = watch.agent_plist('/usr/bin/python3', Path('/x/shader_contrib_watch.py'), '/X/Xcode.app/Contents/Developer')
        self.assertEqual(plist['ProgramArguments'], ['/usr/bin/python3', '/x/shader_contrib_watch.py', 'check'])
        self.assertEqual(plist['StartInterval'], 600)
        stored = watch.agent_plist('/usr/bin/python3', Path('/x/w.py'), '/X', Path('/Users/me/MacNativeGames/shader-list-contribution'))
        self.assertEqual(stored['ProgramArguments'][2:], ['check', '--store', '/Users/me/MacNativeGames/shader-list-contribution'])   # the clone
        self.assertEqual(plist['EnvironmentVariables'], {'DEVELOPER_DIR': '/X/Xcode.app/Contents/Developer'})
        self.assertFalse(plist['RunAtLoad'])

    def test_never_stops_the_reminders(self):
        self.assertEqual(watch.reminded_at(''), 0)
        self.assertEqual(watch.reminded_at('500\n'), 500)
        self.assertIsNone(watch.reminded_at('never\n'))
        self.assertEqual(watch.message(500, 'iPad')[2:], ('Remind me next time', "Don't remind me again", 'Send')
                         if not watch.chinese() else ('下次提醒', '不要提醒了', '发送'))

    def test_gh_replaces_remind_me_next_time_with_submit(self):
        self.assertEqual(watch.message(500, 'iPad', auto=True)[2:], ('Submit automatically', "Don't remind me again", 'Send')
                         if not watch.chinese() else ('自动提交', '不要提醒了', '发送'))
        with tempfile.TemporaryDirectory() as tmp:   # no gh on PATH or in the usual places: no automatic button
            old = (watch.GH_CANDIDATES, watch.shutil.which)
            watch.GH_CANDIDATES, watch.shutil.which = (str(Path(tmp) / 'gh'),), lambda name: None
            try:
                self.assertIsNone(watch.gh_path())
            finally:
                watch.GH_CANDIDATES, watch.shutil.which = old

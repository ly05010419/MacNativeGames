"""The player's contribution file (totk_shader_contrib.h): written by the runtime, merged by scripts/shader_list.py."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.request
from unittest import mock
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
            game = subprocess.run([str(probe), str(contrib), 'game'], capture_output=True, text=True)   # the iPhone / iPad app's tag
            self.assertEqual(game.returncode, 0, game.stderr)
            self.assertEqual(json.loads(game.stdout)['status'], 'pass')
            self.assertIn('\n# Game: TOTK 1.4.3\n', contrib.read_text())
            prune = subprocess.run([str(probe), str(contrib), 'prune'], capture_output=True, text=True)   # served and sent rows leave
            self.assertEqual(prune.returncode, 0, prune.stderr)
            self.assertEqual(json.loads(prune.stdout)['status'], 'pass')


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
        # How the gh submissions of the Mac's device reminder laid out their issues (the reminder went on 2026-11-10: the
        # devices contribute over HTTPS now); issues sent that way are still merged.
        link = 'https://gist.githubusercontent.com/someone/0123abcd/raw/shader-list-contribution.txt'
        body = ('### Game version / 游戏版本\n\n1.4.3\n\n### Device / 设备\n\niPhone\n\n'
                f'### shader-list-contribution.txt\n\n{link}\n\n### Where you played (optional) / 玩了哪些地方（可选）\n\n_No response_\n')
        self.assertEqual(attachment_urls(body), [link])

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

    def seeded_keys(self, game):
        keys = {g: set() for g in collect_shader_issues.GAMES}
        keys[game] = {self.KEY_A}
        return keys

    def test_a_seeded_game_named_in_the_title_takes_the_rows_no_seed_names(self):
        body = f'```\nV\t{self.KEY_W}\t{{}}\n```'   # a shader outside the seed: new to the list, so it is collected
        for title, game in (('Shader list (Breath of the Wild): 1 shaders', 'botw'),
                            ('Shader list: 5227 shaders from Mac (TOTK 1.0.0)', 'totk')):
            with self.subTest(title=title):
                lists, line, per_game = self.merge(self.issue(title, body), self.seeded_keys(game))
                self.assertEqual(lists[game].variants, {(self.KEY_W, '{}')})
                self.assertEqual(per_game, {game: (1, 1)})
                self.assertNotIn('neither game has', line)

    def test_a_title_that_names_no_game_leaves_unknown_rows_open(self):
        body = f'```\nV\t{self.KEY_W}\t{{}}\n```'
        lists, line, per_game = self.merge(self.issue('Shader list: my iPhone, 2 hours', body), self.seeded_keys('totk'))
        self.assertEqual(lists['totk'].variants, set())
        self.assertIsNone(per_game)
        self.assertIn('neither game has', line)

    def test_known_rows_of_the_other_seeded_game_stay_with_it(self):
        keys = self.seeded_keys('totk')
        body = f'```\nV\t{self.KEY_A}\t{{}}\nV\t{self.KEY_W}\t{{}}\n```'
        lists, _, per_game = self.merge(self.issue('Shader list (Breath of the Wild): 2 shaders', body), keys)
        self.assertEqual(lists['totk'].variants, {(self.KEY_A, '{}')})
        self.assertEqual(lists['botw'].variants, {(self.KEY_W, '{}')})
        self.assertEqual(per_game, {'totk': (1, 1), 'botw': (1, 1)})

    def test_a_plain_title_whose_known_rows_are_one_games_gives_it_the_rest(self):
        body = f'```\nV\t{self.KEY_A}\t{{}}\nV\t{self.KEY_W}\t{{}}\n```'   # one install's file: one game's shaders
        lists, line, per_game = self.merge(self.issue('Shader list:', body), self.seeded_keys('totk'))
        self.assertEqual(lists['totk'].variants, {(self.KEY_A, '{}'), (self.KEY_W, '{}')})
        self.assertEqual(per_game, {'totk': (2, 2)})
        self.assertNotIn('neither game has', line)

    def test_a_file_with_two_games_rows_leaves_the_rest_unknown(self):
        key_b = 'b' * 64
        keys = self.seeded_keys('totk'); keys['botw'] = {key_b}
        body = f'```\nV\t{self.KEY_A}\t{{}}\nV\t{key_b}\t{{}}\nV\t{self.KEY_W}\t{{}}\n```'
        lists, line, per_game = self.merge(self.issue('Shader list:', body), keys)
        self.assertIsNone(per_game)
        self.assertIn('1 rows of shaders neither game has', line)

    def test_pokopia_issues_are_its_own_game(self):
        titled = collect_shader_issues.titled_game
        self.assertEqual(titled(self.issue('Shader list: 167 shaders from Mac (Pokopia 2.0.1)')), 'pokopia')

    def test_the_report_is_what_the_merge_found(self):
        issue = self.issue('Shader list (Breath of the Wild): 1 shaders', number=7)
        report = collect_shader_issues.merged_report([(issue, {'botw': (3, 1)})])
        self.assertEqual(report, [{'number': 7, 'titled': 'botw', 'per_game': {'botw': [3, 1]}}])

    def test_bananza_issues_are_its_own_game(self):
        titled = collect_shader_issues.titled_game
        self.assertEqual(titled(self.issue('Shader list: 1120 shaders from Mac (Bananza v458752)')), 'dkb')
        self.assertEqual(titled(self.issue('Shader list (Donkey Kong Bananza): Mac v458752, en')), 'dkb')
        body = f'```\nV\t{self.KEY_W}\t{{}}\n```'
        keys = {g: set() for g in collect_shader_issues.GAMES}
        lists, _, _ = self.merge(self.issue('Shader list: 1 shaders from Mac (Bananza v458752)', body), keys)
        self.assertEqual(lists['dkb'].variants, {(self.KEY_W, '{}')})

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

    def test_a_pipeline_naming_a_bind_state_the_file_does_not_give_is_not_merged(self):
        keys = {g: set() for g in collect_shader_issues.GAMES}; keys['totk'] = {self.KEY_A}
        deps = '{"tex":{"0":"cube"}}'
        named = f'{self.KEY_A}-{suffix(answers_for(json.loads(deps)))}_b'
        def key(vs, fs):
            return (vs.encode().ljust(80, b'\0') + fs.encode().ljust(80, b'\0') + b'\x07' * 40).hex()
        body = (f'```\nV\t{self.KEY_A}\t{deps}\nR\t{key(named, named)}\n'
                f'R\t{key(self.KEY_A + "-" + "9" * 12 + "_b", named)}\n```')   # an older game wrote no V row for this one
        lists, line, per_game = self.merge(self.issue('Shader list: 3 rows', body), keys)
        self.assertEqual(len(lists['totk'].warm), 1)
        self.assertIn('1 naming bind states the file does not give', line)
        self.assertIn(named[65:77], lists['totk'].answers)

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



class OcarinaOfTimeTests(unittest.TestCase):
    """Ocarina of Time's issues go through its own shader_list.py (collect_shader_issues.OWN_SCRIPT): its list keeps mesh
    pipelines (M rows) TOTK's script does not know, and the bind state of every pipeline (2026-11-09, #1980-#1991)."""
    LIST = TOTK.parent / 'oot/assets/shader_list.tsv'

    @staticmethod
    def issue(title, body='', number=1):
        return {'number': number, 'title': title, 'body': body, 'labels': [], 'comments': [], 'author': {'login': 'p'}}

    def test_the_launcher_titles_name_it(self):
        titled = collect_shader_issues.titled_game
        self.assertEqual(titled(self.issue('Shader list: 2557 shaders from Mac (OcarinaOfTime 1.0.0)')), 'oot')
        self.assertEqual(titled(self.issue('Shader list: 151 shaders from Mac (OcarinaOfTime 2.0.0)')), 'oot')

    @unittest.skipUnless(LIST.is_file(), 'needs the Ocarina of Time list')
    def test_merged_by_its_own_script_keeping_mesh_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'games/oot/assets/shader_list.tsv'; path.parent.mkdir(parents=True)
            shutil.copy2(self.LIST, path)
            meshes = sum(1 for line in path.read_text().splitlines() if line.startswith('M\t'))
            self.assertGreater(meshes, 0)
            shared = collect_shader_issues.load_list('oot', path)
            key = 'f' * 64
            line, per_game = collect_shader_issues.merge_own(self.issue('Shader list: 1 shaders from Mac (OcarinaOfTime 2.0.0)',
                                                                         f'```\nV\t{key}\t{{}}\n```'), 'oot', shared, token='')
            self.assertEqual(per_game, {'oot': (1, 1)}, line)
            saved = collect_shader_issues.GAMES['oot']
            collect_shader_issues.GAMES['oot'] = (path, *saved[1:])
            try:
                collect_shader_issues.save_list('oot', shared)
            finally:
                collect_shader_issues.GAMES['oot'] = saved
            text = path.read_text()
            self.assertTrue(text.startswith('# Ocarina of Time'), text[:60])
            self.assertEqual(sum(1 for line in text.splitlines() if line.startswith('M\t')), meshes)
            self.assertIn(f'V\t{key}\t{{}}', text)


class ShaderInboxTests(unittest.TestCase):
    """Rows the apps sent over HTTPS (the shader inbox) merge as an issue titled for the game they name."""
    KEY_A, KEY_W = 'a' * 64, 'c' * 64

    @staticmethod
    def pending(rows):
        return [{'id': str(i + 1), 'kind': r[0], 'body': r, 'players': 1, 'versions': ['1.0.0'], 'platforms': ['mac']}
                for i, r in enumerate(rows)]

    def merge(self, by_game, keys=None):
        keys = keys or {g: set() for g in collect_shader_issues.GAMES}
        lists = {g: ShaderList() for g in collect_shader_issues.GAMES}
        with mock.patch.object(collect_shader_issues, 'inbox_pending', lambda base, token, game: by_game.get(game, [])):
            done = collect_shader_issues.merge_inbox('https://inbox.test', 't', lists, keys, {}, False)
        return lists, keys, {game: (line, ids) for line, game, ids in done}

    def test_rows_go_to_the_game_the_app_named(self):
        lists, keys, done = self.merge({'wonder': self.pending([f'V\t{self.KEY_W}\t{{}}'])})
        self.assertEqual(lists['wonder'].variants, {(self.KEY_W, '{}')})
        self.assertEqual(done['wonder'][1], ['1'])
        self.assertIn(self.KEY_W, keys['wonder'], 'teaches plain-titled issues the game\'s keys')

    def test_a_seeded_game_takes_rows_no_seed_names(self):
        lists, _, done = self.merge({'botw': self.pending([f'V\t{self.KEY_W}\t{{}}', f'V\t{self.KEY_A}\t{{}}'])})
        self.assertEqual(lists['botw'].variants, {(self.KEY_W, '{}'), (self.KEY_A, '{}')})
        self.assertEqual(lists['totk'].variants, set())
        self.assertEqual(done['botw'][1], ['1', '2'])

    def test_a_failed_export_takes_nothing(self):
        def broken(base, token, game):
            raise ValueError('/v1/export: HTTP 401 unauthorized')
        lists = {g: ShaderList() for g in collect_shader_issues.GAMES}
        with mock.patch.object(collect_shader_issues, 'inbox_pending', broken):
            done = collect_shader_issues.merge_inbox('https://inbox.test', 't', lists, {g: set() for g in lists}, {}, False)
        self.assertTrue(done and all(ids == [] and 'HTTP 401' in line for line, _, ids in done))

    def test_pending_follows_the_pages(self):
        pages = {0: {'rows': [{'id': '1'}, {'id': '2'}], 'next': '2'}, 2: {'rows': [{'id': '3'}], 'next': None}}
        asked = []
        def call(base, token, path, payload=None):
            asked.append(path)
            return pages[int(path.split('after=')[1].split('&')[0])]
        with mock.patch.object(collect_shader_issues, 'inbox_call', call):
            rows = collect_shader_issues.inbox_pending('https://inbox.test', 't', 'totk')
        self.assertEqual([r['id'] for r in rows], ['1', '2', '3'])
        self.assertEqual(len(asked), 2)
        self.assertTrue(all(p.startswith('/v1/export?game=totk&') for p in asked))


# The inbox is a project of its own (not in this repository): $SHADER_INBOX_DIR, else MacNativeGamesWeb/shader-inbox beside it.
INBOX = Path(os.environ.get('SHADER_INBOX_DIR') or TOTK.parents[2] / 'MacNativeGamesWeb/shader-inbox')


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('node') and (INBOX / 'node_modules/@electric-sql/pglite').is_dir(),
                     'needs the Xcode command line tools, node and the shader inbox (SHADER_INBOX_DIR) with npm install')
class ShaderInboxUploadTests(unittest.TestCase):
    """The app's upload (totk_contrib_upload.h) against the inbox's own handlers on 127.0.0.1 (test/local-server.js)."""

    def build(self, tmp, url):
        probe = tmp / f'probe-{len(list(tmp.iterdir()))}'
        build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', f'-DTOTK_CONTRIB_INBOX="{url}"',
                                f'-I{TOTK}/third_party/d2r_gfx/gfx/nvn', str(TOTK / 'tests/shader_contrib/inbox_probe.m'),
                                '-framework', 'Foundation', '-o', str(probe)], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr[-600:])
        return probe

    def test_rows_go_to_the_inbox_once_and_leave_the_file(self):
        server = subprocess.Popen(['node', str(INBOX / 'test/local-server.js')], cwd=INBOX, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().split()[1])
            with tempfile.TemporaryDirectory() as tmp:
                tmp = Path(tmp)
                probe = self.build(tmp, f'http://127.0.0.1:{port}/v1/contrib')
                run = subprocess.run([str(probe), str(tmp / 'shader-list-contribution.txt'), 'up'], capture_output=True, text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stderr)
                result = json.loads(run.stdout)
                self.assertEqual(result['status'], 'pass')
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/admin/rows?game=totk', timeout=30) as reply:
                    rows = json.loads(reply.read())['data']['rows']
                self.assertEqual(sorted(r['kind'] for r in rows), ['B', 'R', 'V'])
                self.assertTrue(all(r['players'] == 1 and r['versions'] == ['1.4.3'] and r['platforms'] == ['mac'] for r in rows))
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/admin/uploads?game=totk', timeout=30) as reply:
                    uploads = json.loads(reply.read())['data']
                model = subprocess.run(['sysctl', '-n', 'hw.model'], capture_output=True, text=True).stdout.strip()
                self.assertEqual([(u['install'], u['platform'], u['device'], u['game_version'], u['shaders_sent'], u['pipelines_sent'])
                                  for u in uploads], [(result['install'], 'mac', model, '1.4.3', 1, 2)],
                                 'one upload: this Mac\'s model, the rows by kind')
                self.assertRegex(uploads[0]['os'], r'^macOS \d+\.\d+\.\d+$')
        finally:
            server.stdin.close(); server.wait(timeout=30); server.stdout.close()

    def test_without_an_answer_the_rows_stay(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            probe = self.build(tmp, 'http://127.0.0.1:9/v1/contrib')   # the discard port: nothing listens
            run = subprocess.run([str(probe), str(tmp / 'shader-list-contribution.txt'), 'down'], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(run.stdout)['status'], 'pass')

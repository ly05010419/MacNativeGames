"""The player's contribution file (totk_shader_contrib.h): written by the runtime, merged by scripts/shader_list.py."""
import json
import re
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


import collect_shaders  # noqa: E402
from collect_shaders import contribution_items, key_source, owner  # noqa: E402
from shader_list import DEFAULT_SUFFIX  # noqa: E402


def name(key):
    """A variant name of the default bind state (every list knows it)."""
    return f'{key}-{DEFAULT_SUFFIX}_b'


def pipeline(vs, fs, kind='R'):
    """A render pipeline row: its key leads with two NUL-padded 80-byte variant names."""
    pad = lambda k: (name(k).encode() if k else b'').ljust(80, b'\0')
    return (kind, (pad(vs) + pad(fs) + bytes(16)).hex())


def lists_for(games=None):
    """Empty lists of every game, each of the kind its merge uses (Ocarina of Time's own shader_list.py)."""
    with tempfile.TemporaryDirectory() as tmp:
        return {g: collect_shaders.load_list(g, Path(tmp) / f'{g}.tsv') for g in (games or collect_shaders.GAMES)}


class ContributionRowsTests(unittest.TestCase):
    KEY_A, KEY_B = 'a' * 64, 'b' * 64

    def test_only_shader_and_pipeline_rows_are_taken_once(self):
        text = '\n'.join([
            '# header', '',
            f'V\t{self.KEY_A}\t{{"tex":{{"9":"2d","10":"3d"}}}}',     # canonicalised
            f'V\t{self.KEY_A}\t{{"tex":{{"10":"3d","9":"2d"}}}}',     # the same row
            f'V\t{self.KEY_B}\t{{}}',
            f'S\t{self.KEY_A}-0123456789ab_b\t0\tm',                  # shader libraries are not taken from contributions
            f'L\t{self.KEY_A}\tShader/x.bnsh\t0\t880\t100',            # nor locators
            'R\tabcd', 'R\tabcd', 'B\tabcd\t-\t3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;',   # render pipelines (a row once)
            'T\tabcd\t5:3;7:1;', 'T\tabcd\t5:9;',                   # and tile reads (an attachment past 7 is none)
            'R\tnot hex', 'B\tabcd\t-\t-',
            'V\tnot-a-key\t{}', f'V\t{self.KEY_B}\t{{"local":[1]}}', f'V\t{self.KEY_B}\tnot json'])
        items, dropped = contribution_items(text)
        self.assertEqual([item for _, item in items], [
            ('V', (self.KEY_A, '{"tex":{"10":"3d","9":"2d"}}')), ('V', (self.KEY_B, '{}')), ('P', ('R', 'abcd')),
            ('P', ('B', 'abcd', '-', '3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;')), ('P', ('T', 'abcd', '5:3;7:1;'))])
        self.assertEqual(items[0][0], f'V\t{self.KEY_A}\t{{"tex":{{"9":"2d","10":"3d"}}}}', 'each item keeps the line it came from')
        self.assertEqual(dropped, 8)

    def test_shader_keys_come_from_a_seed_or_a_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            for folder, version, game in (('botw160', 'botw160', 'botw'), ('oot', 'oot', 'oot'), ('v143', 'v143', 'totk')):
                seed = Path(tmp) / folder; seed.mkdir()
                (seed / 'shader-keys.json').write_text(json.dumps({'version': version, 'keys': [self.KEY_B]}))
                self.assertEqual(key_source(str(seed)), (game, {self.KEY_B}, True))
            listed = Path(tmp) / 'v121-keys.json'; listed.write_text(json.dumps([self.KEY_A]))
            self.assertEqual(key_source(f'totk={listed}'), ('totk', {self.KEY_A}, True))
            old = Path(tmp) / 'v0'; old.mkdir(); (old / f'{self.KEY_A}.variants').write_text('')
            with self.assertRaises(ValueError):   # an old seed names only its startup shaders: not every key of the game
                key_source(str(old))
            with self.assertRaises(ValueError):
                key_source(f'odyssey={listed}')


class SwitchTwoSeedTests(unittest.TestCase):
    """A Switch 2 game's seed (prepare_ns2_seed.py) names in shader-keys.json only the shaders it translated, in
    game-shader-keys.json every shader the game ships: only the second makes the game COMPLETE (user 2026-11-12: "做a")."""
    TRANSLATED, OTHER = 'a' * 64, 'b' * 64
    ROMFS = 'c' * 64

    def seed(self, tmp, version='pkp', identity=True, romfs=None, rule=collect_shaders.KEY_RULE):
        folder = Path(tmp) / version; folder.mkdir()
        (folder / 'seed-manifest.json').write_text(json.dumps({'shader_key_rule': rule} if rule else {}))
        (folder / 'shader-keys.json').write_text(json.dumps({'version': version, 'source_romfs_sha256': self.ROMFS,
                                                             'keys': [self.TRANSLATED]}))
        if identity:
            (folder / 'game-shader-keys.json').write_text(json.dumps({'version': version, 'source_romfs_sha256': romfs or self.ROMFS,
                                                                      'keys': [self.TRANSLATED, self.OTHER]}))
        return folder

    def test_every_shader_of_the_game_from_its_own_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(key_source(str(self.seed(tmp))), ('pokopia', {self.TRANSLATED, self.OTHER}, True))
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(key_source(str(self.seed(tmp, 'dkb'))), ('dkb', {self.TRANSLATED, self.OTHER}, True))

    def test_a_seed_without_it_is_not_every_shader(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(key_source(str(self.seed(tmp, identity=False))), ('pokopia', {self.TRANSLATED}, False))

    def test_it_must_name_the_shaders_by_the_lists_key_rule(self):
        # 2026-11-12: the Switch 2 key rule changed; a seed of the old one names no shader the lists name, and a full
        # seed's check would then take every row out of the game's list
        for rule in (None, 'nvn2-an-older-rule'):
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(ValueError, 'shader key rule'):
                    key_source(str(self.seed(tmp, 'dkb', rule=rule)))

    def test_it_must_be_of_the_same_game_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'other game files'):
                key_source(str(self.seed(tmp, romfs='d' * 64)))

    def test_the_merge_will_not_run_with_a_seed_that_names_only_what_it_translated(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = subprocess.run([sys.executable, str(TOTK / 'scripts/collect_shaders.py'), '--inbox', 'https://inbox.test',
                                  '--seed', str(self.seed(tmp, 'dkb', identity=False))],
                                 capture_output=True, text=True, env={**os.environ, 'SHADER_INBOX_TOKEN': 't'})
            self.assertEqual(run.returncode, 2)
            self.assertIn('package dkb again', run.stderr)

    def test_bananza_is_complete_and_pokopia_not_yet(self):
        # Bananza: all 213 shader keys its installs sent were among its corpus's 22,854. Pokopia: 24 of 333 among its
        # 131,090 (2026-11-12): its corpus misses shaders the game uses, so its rows are not refused by it
        self.assertIn('dkb', collect_shaders.COMPLETE)
        self.assertNotIn('pokopia', collect_shaders.COMPLETE)


class OwnerTests(unittest.TestCase):
    """Which list a row goes to: what its shaders are, the app's game only as the default (user 2026-11-12: "根治")."""
    TOTK, BOTW, OOT, NEW = 'a' * 64, 'b' * 64, 'c' * 64, 'f' * 64

    def keys(self):
        keys = {g: set() for g in collect_shaders.GAMES}
        keys['totk'] = {self.TOTK, self.BOTW}; keys['botw'] = {self.BOTW}; keys['oot'] = {self.OOT}
        return keys

    def test_the_named_game_when_it_holds_the_shaders(self):
        self.assertEqual(owner([self.OOT], 'oot', self.keys()), ('oot', 'named'))
        self.assertEqual(owner([self.BOTW], 'totk', self.keys()), ('totk', 'named'), 'a shader both games have, sent by one')

    def test_another_game_when_only_it_holds_them(self):
        self.assertEqual(owner([self.TOTK, self.TOTK], 'oot', self.keys()), ('totk', 'other'))
        self.assertEqual(owner([self.TOTK], 'wonder', self.keys()), ('totk', 'other'))

    def test_left_out_when_several_other_games_hold_them(self):
        self.assertEqual(owner([self.BOTW], 'oot', self.keys()), (None, 'shared'))

    def test_a_new_shader_is_the_named_games_unless_its_seed_names_all_its_shaders(self):
        self.assertEqual(owner([self.NEW], 'totk', self.keys()), ('totk', 'named'))
        self.assertEqual(owner([self.NEW], 'wonder', self.keys()), ('wonder', 'named'))
        self.assertEqual(owner([self.NEW], 'oot', self.keys()), (None, 'outside'))
        self.assertEqual(owner([self.OOT, self.NEW], 'oot', self.keys()), (None, 'outside'))
        self.assertEqual(owner([], 'oot', self.keys()), (None, 'outside'))


class ShaderInboxTests(unittest.TestCase):
    """The rows the apps sent over HTTPS (the shader inbox), the only way contributions come in."""
    TOTK, BOTW, OOT, NEW = 'a' * 64, 'b' * 64, 'c' * 64, 'f' * 64

    @staticmethod
    def pending(rows):
        return [{'id': str(i + 1), 'kind': r[0], 'body': r, 'players': 1, 'versions': ['1.0.0'], 'platforms': ['mac']}
                for i, r in enumerate(rows)]

    def keys(self):
        keys = {g: set() for g in collect_shaders.GAMES}
        keys['totk'] = {self.TOTK, self.BOTW}; keys['botw'] = {self.BOTW}; keys['oot'] = {self.OOT}
        return keys

    def merge(self, by_game, keys=None):
        keys = keys or self.keys()
        lists = lists_for()
        with mock.patch.object(collect_shaders, 'inbox_pending', lambda base, token, game: by_game.get(game, [])):
            done = collect_shaders.merge_inbox('https://inbox.test', 't', lists, keys, {})
        return lists, keys, {game: (line, ids) for line, game, ids in done}

    def test_another_games_pipelines_an_app_sent_go_to_that_game(self):
        # 2026-11-11: an Ocarina of Time install sent 418 Tears of the Kingdom pipelines and they went into Ocarina of Time's list
        theirs = pipeline(self.TOTK, self.TOTK)
        body = ['\t'.join(theirs), f'V\t{self.OOT}\t{{}}', '\t'.join(pipeline(self.OOT, self.OOT))]
        lists, _, done = self.merge({'oot': self.pending(body)})
        self.assertIn(theirs, lists['totk'].warm)
        self.assertNotIn(theirs, lists['oot'].warm)
        self.assertEqual(lists['oot'].variants, {(self.OOT, '{}')})
        self.assertIn(pipeline(self.OOT, self.OOT), lists['oot'].warm)
        self.assertIn("1 rows of another game's shaders went to it", done['oot'][0])
        self.assertEqual(done['oot'][1], ['1', '2', '3'])

    def test_rows_outside_a_full_seed_no_game_has_stay_in_the_inbox(self):
        lists, _, done = self.merge({'oot': self.pending([f'V\t{self.NEW}\t{{}}', f'V\t{self.OOT}\t{{}}'])})
        self.assertEqual(lists['oot'].variants, {(self.OOT, '{}')})
        self.assertTrue(all(not s.variants for g, s in lists.items() if g != 'oot'))
        self.assertEqual(done['oot'][1], ['2'], 'the row nobody holds is not marked done')
        self.assertIn("1 rows of shaders no game here has and outside oot's full seed", done['oot'][0])

    def test_rows_several_other_games_hold_are_left_out(self):
        lists, _, done = self.merge({'oot': self.pending([f'V\t{self.BOTW}\t{{}}'])})
        self.assertTrue(all(not s.variants for s in lists.values()))
        self.assertIn('1 rows several games share (left out)', done['oot'][0])
        self.assertEqual(done['oot'][1], ['1'])

    def test_a_game_without_a_full_seed_takes_new_shaders_and_shared_ones(self):
        lists, keys, done = self.merge({'botw': self.pending([f'V\t{self.NEW}\t{{}}', f'V\t{self.BOTW}\t{{}}']),
                                        'wonder': self.pending([f'V\t{"e" * 64}\t{{}}'])})
        self.assertEqual(lists['botw'].variants, {(self.NEW, '{}'), (self.BOTW, '{}')})
        self.assertEqual(lists['totk'].variants, set())
        self.assertEqual(lists['wonder'].variants, {('e' * 64, '{}')})
        self.assertIn('e' * 64, keys['wonder'], 'what it merged names its shaders for later rows')
        self.assertEqual(done['botw'][1], ['1', '2'])

    def test_switch_2_rows_only_from_apps_of_the_current_key_rule(self):
        # an app made before the key rule (nvn_ctl.h NVN2_KEY_RULE) sends keys no list or seed names: never merged, marked
        # done; one this repository cannot place (a fork, a build outside git, an export without builds) stays in the inbox
        rows = self.pending([f'V\t{"1" * 64}\t{{}}', f'V\t{"2" * 64}\t{{}}', f'V\t{"3" * 64}\t{{}}', f'V\t{"4" * 64}\t{{}}'])
        for row, builds in zip(rows, (['old1', 'new1'], ['old1', 'old2'], ['fork1'], None)):
            if builds:
                row['builds'] = builds
        rule = {'new1': 'yes', 'old1': 'no', 'old2': 'no', 'fork1': 'unknown'}
        with mock.patch.object(collect_shaders, 'key_rule_commit', lambda: 'k' * 40), \
                mock.patch.object(collect_shaders, 'build_rule', lambda build, since: rule[build]):
            lists, _, done = self.merge({'pokopia': rows})
        self.assertEqual(lists['pokopia'].variants, {('1' * 64, '{}')})
        self.assertEqual(done['pokopia'][1], ['1', '2'])
        self.assertIn('1 only from apps of an older shader key rule (marked done, not merged)', done['pokopia'][0])
        self.assertIn('2 from app builds not in this repository (left in the inbox)', done['pokopia'][0])

    def test_without_the_rules_commit_no_switch_2_row_is_placed(self):
        rows = self.pending([f'V\t{"1" * 64}\t{{}}'])
        rows[0]['builds'] = ['0123abc']
        with mock.patch.object(collect_shaders, 'key_rule_commit', lambda: None):
            lists, _, done = self.merge({'pokopia': rows})
        self.assertEqual(lists['pokopia'].variants, set())
        self.assertEqual(done['pokopia'][1], [])

    def test_a_build_is_placed_by_git(self):
        head = subprocess.run(['git', '-C', str(TOTK), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        first = subprocess.run(['git', '-C', str(TOTK), 'rev-list', '--max-parents=0', 'HEAD'], capture_output=True,
                               text=True).stdout.split()[0]
        self.assertEqual(collect_shaders.build_rule(head[:7], first), 'yes')
        self.assertEqual(collect_shaders.build_rule(first[:7], head), 'no' if first != head else 'yes')
        self.assertEqual(collect_shaders.build_rule('dev', head), 'unknown')
        self.assertEqual(collect_shaders.build_rule('0' * 12, head), 'unknown')
        self.assertEqual(collect_shaders.build_rule(head[:7], None), 'unknown')

    def test_a_failed_export_takes_nothing(self):
        def broken(base, token, game):
            raise ValueError('/v1/export: HTTP 401 unauthorized')
        lists = lists_for()
        with mock.patch.object(collect_shaders, 'inbox_pending', broken):
            done = collect_shaders.merge_inbox('https://inbox.test', 't', lists, self.keys(), {})
        self.assertTrue(done and all(ids == [] and 'HTTP 401' in line for line, _, ids in done))

    def test_pending_follows_the_pages(self):
        pages = {0: {'rows': [{'id': '1'}, {'id': '2'}], 'next': '2'}, 2: {'rows': [{'id': '3'}], 'next': None}}
        asked = []
        def call(base, token, path, payload=None):
            asked.append(path)
            return pages[int(path.split('after=')[1].split('&')[0])]
        with mock.patch.object(collect_shaders, 'inbox_call', call):
            rows = collect_shaders.inbox_pending('https://inbox.test', 't', 'totk')
        self.assertEqual([r['id'] for r in rows], ['1', '2', '3'])
        self.assertEqual(len(asked), 2)
        self.assertTrue(all(p.startswith('/v1/export?game=totk&') for p in asked))

    def test_the_token_never_reaches_the_command_line(self):
        seen = {}
        def run(cmd, **kwargs):
            seen['cmd'] = cmd
            seen['header'] = Path(cmd[cmd.index('-H') + 1][1:]).read_text()
            seen['mode'] = Path(cmd[cmd.index('-H') + 1][1:]).stat().st_mode & 0o777
            return subprocess.CompletedProcess(cmd, 0, '{"success":true,"data":{"marked":2}}\n200', '')
        with mock.patch.object(collect_shaders.subprocess, 'run', run):
            data = collect_shaders.inbox_call('https://inbox.test', 'secret-token', '/v1/mark', {'ids': ['1']})
        self.assertEqual(data, {'marked': 2})
        self.assertEqual(seen['cmd'][0], '/usr/bin/curl')
        self.assertFalse(any('secret-token' in arg for arg in seen['cmd']))
        self.assertIn('Authorization: Bearer secret-token', seen['header'])
        self.assertEqual(seen['mode'], 0o600)

    def test_a_refused_answer_is_an_error(self):
        reply = subprocess.CompletedProcess([], 0, '{"success":false,"error":"unauthorized"}\n401', '')
        with mock.patch.object(collect_shaders.subprocess, 'run', lambda cmd, **kw: reply):
            with self.assertRaisesRegex(ValueError, 'HTTP 401 unauthorized'):
                collect_shaders.inbox_call('https://inbox.test', 't', '/v1/export?game=totk')

    def test_the_script_will_not_run_without_the_full_seed(self):
        run = subprocess.run([sys.executable, str(TOTK / 'scripts/collect_shaders.py'), '--inbox', 'https://inbox.test'],
                             capture_output=True, text=True, env={**os.environ, 'SHADER_INBOX_TOKEN': 't'})
        self.assertEqual(run.returncode, 2)
        self.assertIn('no seed of oot', run.stderr)


class OcarinaOfTimeListTests(unittest.TestCase):
    """Ocarina of Time's seed names every shader of its files (collect_shaders.COMPLETE): its list never keeps another's."""
    LIST = TOTK.parent / 'oot/assets/shader_list.tsv'
    TOTK_KEY, OOT_KEY = 'a' * 64, 'c' * 64

    def test_rows_outside_its_seed_leave_its_list_for_the_game_that_holds_them(self):
        lists = lists_for(('oot', 'totk', 'botw'))
        ours, theirs, tiles = pipeline(self.OOT_KEY, self.OOT_KEY), pipeline(self.TOTK_KEY, self.TOTK_KEY), \
            ('T', pipeline(self.TOTK_KEY, self.TOTK_KEY)[1], '5:3;')
        lists['oot'].add_variant(self.OOT_KEY, '{}')
        for row in (ours, theirs, tiles):
            lists['oot'].add_warm(row, 'm')
        keys = {'oot': {self.OOT_KEY}, 'totk': {self.TOTK_KEY}, 'botw': set()}
        line = collect_shaders.drop_foreign('oot', lists, keys, {self.OOT_KEY})
        self.assertEqual(list(lists['oot'].warm), [ours])
        self.assertEqual(set(lists['totk'].warm), {theirs, tiles})
        self.assertIn('0 variants and 2 pipeline rows', line)
        self.assertIn('2 new to the game that holds them', line)
        self.assertEqual(collect_shaders.foreign_rows('oot', lists['oot'], {self.OOT_KEY}), (set(), []))
        self.assertEqual(collect_shaders.drop_foreign('oot', lists, keys, {self.OOT_KEY}), '', 'clean: nothing to say')

    @unittest.skipUnless(LIST.is_file(), 'needs the Ocarina of Time list')
    def test_merged_by_its_own_script_keeping_mesh_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'games/oot/assets/shader_list.tsv'; path.parent.mkdir(parents=True)
            shutil.copy2(self.LIST, path)
            meshes = sum(1 for line in path.read_text().splitlines() if line.startswith('M\t'))
            self.assertGreater(meshes, 0)
            shared = collect_shaders.load_list('oot', path)
            key = 'f' * 64
            items, _ = contribution_items(f'V\t{key}\t{{}}\n')
            part = collect_shaders.merge_items('oot', items, {'oot': shared}, {})
            self.assertEqual(part, 'oot 1 rows (1 new)')
            saved = collect_shaders.GAMES['oot']
            collect_shaders.GAMES['oot'] = path
            try:
                collect_shaders.save_list('oot', shared)
            finally:
                collect_shaders.GAMES['oot'] = saved
            text = path.read_text()
            self.assertTrue(text.startswith('# Ocarina of Time'), text[:60])
            self.assertEqual(sum(1 for line in text.splitlines() if line.startswith('M\t')), meshes)
            self.assertIn(f'V\t{key}\t{{}}', text)


class ListFileTests(unittest.TestCase):
    KEY_A, KEY_W = 'a' * 64, 'e' * 64

    def test_another_game_s_list_keeps_its_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'odyssey/assets/shader_list.tsv'
            saved = collect_shaders.GAMES['odyssey']
            collect_shaders.GAMES['odyssey'] = path
            try:
                shared = ShaderList(); shared.variants.add((self.KEY_W, '{}'))
                collect_shaders.save_list('odyssey', shared)   # a new list: a header naming the game
                self.assertTrue(path.read_text().startswith('# Super Mario Odyssey shader list v1'))
                path.write_text('# Odyssey list, chain v4\n' + path.read_text().split('\n', 2)[2])
                shared.variants.add((self.KEY_A, '{}'))
                collect_shaders.save_list('odyssey', shared)
                text = path.read_text()
                self.assertTrue(text.startswith('# Odyssey list, chain v4\nV\t'), text[:80])
                self.assertEqual(len(ShaderList.load(path).variants), 2)
            finally:
                collect_shaders.GAMES['odyssey'] = saved

    def test_a_pipeline_naming_a_bind_state_no_row_gives_is_not_merged(self):
        # 2026-11-11, #2020 / PR #2092: two such rows reached Pokopia's list, and packaging stopped in resolve
        deps = '{"tex":{"0":"cube"}}'
        named = f'{self.KEY_W}-{suffix(answers_for(json.loads(deps)))}_b'
        def key(vs, fs):
            return (vs.encode().ljust(80, b'\0') + fs.encode().ljust(80, b'\0') + b'\x07' * 40).hex()
        text = (f'V\t{self.KEY_W}\t{deps}\nR\t{key(named, named)}\n'
                f'R\t{key(self.KEY_W + "-" + "9" * 12 + "_b", named)}\n')   # an older game wrote no V row for this one
        for game in ('totk', 'wonder'):
            with self.subTest(game=game):
                lists = lists_for((game,))
                items, _ = contribution_items(text)
                part = collect_shaders.merge_items(game, items, lists, {})
                self.assertEqual(len(lists[game].warm), 1)
                self.assertIn('1 naming bind states the file does not give', part)


class NoIssueRouteTests(unittest.TestCase):
    """User 2026-11-12: contributions come only through the apps' upload; nothing reads or asks for GitHub issues."""

    def test_the_collector_reads_no_issues(self):
        text = (TOTK / 'scripts/collect_shaders.py').read_text()
        for word in ('gh issue', "'gh'", 'issues/new', 'gist', '--close', 'reply'):
            self.assertNotIn(word, text)
        self.assertFalse((TOTK / 'scripts/collect_shader_issues.py').exists())


class ShaderInboxScheduleTests(unittest.TestCase):
    """Every game's copy of the upload (user 2026-11-10): the question has no row threshold, one upload at each start,
    then one an hour."""

    def test_every_copy_asks_without_rows_and_uploads_at_start_then_on_a_timer(self):
        # The three Zelda games (games/totk: TotK and BotW; games/oot) send every half hour (user 2026-11-11: "开机和半小时间隔"),
        # the other games every hour.
        copies = sorted(TOTK.parent.glob('*/third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h'))
        self.assertGreaterEqual(len(copies), 7)
        for copy in copies:
            text = copy.read_text()
            ask = text[text.index('static inline NSUInteger totk_inbox_should_ask'):]
            ask = ask[:ask.index('\n}\n')]
            self.assertNotIn('totk_contrib_due', ask, f'{copy}: the question waits for rows')
            zelda = copy.relative_to(TOTK.parent).parts[0] in ('totk', 'oot')
            every = '(30 * 60)' if zelda else '(60 * 60)'
            self.assertIn(f'#define TOTK_INBOX_EVERY {every}', text, f'{copy}: not every {"half hour" if zelda else "hour"}')
            self.assertIn('#define TOTK_INBOX_FIRST 20', text, f'{copy}: no upload at the start')


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


class OneContributionFilePerGameTests(unittest.TestCase):
    """Each game writes its own contribution file: an app sends its file naming its own game, so a file two games share
    sends one game's rows as the other's (2026-11-12: games/totk's launcher gave Breath of the Wild, Bananza and Pokopia one
    file; the inbox had 464 Pokopia rows sent as Breath of the Wild 1.6.0's)."""

    def test_the_totk_launcher_names_each_games_file(self):
        text = (TOTK / 'app/launcher.m').read_text()
        body = text[text.index('static NSString *contrib_file'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('!strcmp(GAME_FOLDER, "TOTK") ? @"shader-list-contribution.txt"', body)
        self.assertIn('!strcmp(GAME_FOLDER, "BotW") ? @"shader-list-contribution-botw.txt"', body)
        self.assertIn('@"shader-list-contribution-%s.txt", TOTK_GAME_DIR', body)

    def test_every_place_that_starts_the_upload_claims_the_file_first(self):
        # the game gets its id from the claim and writes only to a file of it (ContributionOwnerTests); a launcher or app that
        # set the file without claiming it would leave the game writing without the check
        starts = [p for p in sorted(TOTK.parent.glob('*/**/*.[mhc]'))
                  if 'third_party' not in p.parts and 'tests' not in p.parts and 'setenv("TOTK_SHADER_CONTRIB"' in p.read_text(errors='replace')
                  and 'totk_inbox_start' in p.read_text(errors='replace')]
        self.assertGreaterEqual(len(starts), 8)
        for path in starts:
            self.assertIn('totk_inbox_claim(', path.read_text(), f'{path}: sets the contribution file without claiming it')
        odyssey = (TOTK.parent / 'odyssey/runtime/settings_panel.h').read_text()   # its launcher is C: the panel claims it
        self.assertIn('totk_inbox_start(totk_inbox_claim(totk_contrib_path())', odyssey)

    def test_no_launcher_gives_every_other_game_one_file(self):
        shared = re.compile(r'strcmp\(GAME_FOLDER, "\w+"\) \? @"shader-list-contribution[^"]*"\s*: @"shader-list-contribution')
        for launcher in sorted(TOTK.parent.glob('*/app/launcher.m')):
            self.assertIsNone(shared.search(launcher.read_text()), f'{launcher}: one contribution file for several games')


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class ContributionOwnerTests(unittest.TestCase):
    """Every game writes and sends only a contribution file of its own (user 2026-11-12: "每个游戏的着色器都独立 … 上传独立"):
    the launcher claims the file (totk_inbox_claim), the game writes only to a file of its id, the upload sends only such a
    file. Each copy of the upload is built and run (games/<game>/tests/... has none of its own: one probe, every copy)."""
    PROBE = TOTK / 'tests/shader_contrib/owner_probe.m'
    ROW = f'V\t{7:064x}\t{{}}'

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.probes = {}
        for copy in sorted(TOTK.parent.glob('*/third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h')):
            game = copy.relative_to(TOTK.parent).parts[0]
            out = Path(cls.tmp.name) / f'probe-{game}'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-DTOTK_CONTRIB_INBOX="http://127.0.0.1:9/v1/contrib"', '-DTOTK_GAME_VERSION="1.0.0"',
                                    f'-I{copy.parent}', str(cls.PROBE),
                                    '-framework', 'Foundation', '-o', str(out)], capture_output=True, text=True)
            cls.probes[game] = out if build.returncode == 0 else build.stderr[-800:]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_probe(self, game, *args, env=None):
        probe = self.probes[game]
        self.assertIsInstance(probe, Path, f'{game}: the probe did not build:\n{probe}')
        return subprocess.run([str(probe), *args], capture_output=True, text=True, timeout=120,
                              env={**{k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}, **(env or {})})

    def test_every_copy_with_an_upload_is_checked(self):
        self.assertGreaterEqual(len(self.probes), 7)
        for game in self.probes:
            with self.subTest(game=game):
                self.assertEqual(self.run_probe(game, 'id').stdout.strip(), game)   # each copy's GAME_INBOX_ID is its folder's

    def test_a_new_file_names_its_game(self):
        for game in self.probes:
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'shader-list-contribution.txt'
                run = self.run_probe(game, 'claim-write', str(path))
                self.assertEqual(run.returncode, 0, run.stderr)
                lines = path.read_text().splitlines()
                self.assertIn(f'# Inbox game: {self.run_probe(game, "id").stdout.strip()}', lines)
                self.assertIn(self.ROW, lines)

    def test_another_games_file_and_an_unnamed_one_move_aside_unsent(self):
        for game in self.probes:
            for owner in ('someothergame', None):
                with self.subTest(game=game, owner=owner), tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / 'shader-list-contribution.txt'
                    path.write_text('# header\n' + (f'# Inbox game: {owner}\n' if owner else '') + f'V\t{"a" * 64}\t{{}}\n')
                    done = Path(tmp) / '.shader-list-contribution.txt.done'; done.write_text(f'V\t{"b" * 64}\t{{}}\n')
                    run = self.run_probe(game, 'claim-write', str(path))
                    self.assertEqual(run.returncode, 0, run.stderr)
                    aside = [p for p in Path(tmp).iterdir() if p.name.startswith('shader-list-contribution.txt.')]
                    self.assertEqual(len(aside), 1, sorted(p.name for p in Path(tmp).iterdir()))
                    self.assertIn(f'of-{owner}-' if owner else 'unclaimed-', aside[0].name)
                    self.assertIn('a' * 64, aside[0].read_text())
                    self.assertTrue((Path(tmp) / f'.{aside[0].name}.done').is_file(), 'its done file goes with it')
                    self.assertNotIn('a' * 64, path.read_text(), 'the new file holds only this game\'s rows')
                    self.assertFalse(done.exists())

    def test_the_game_writes_nothing_to_another_games_file(self):
        for game in self.probes:
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'shader-list-contribution.txt'
                text = '# Inbox game: someothergame\n'
                path.write_text(text)
                own = self.run_probe(game, 'id').stdout.strip()
                run = self.run_probe(game, 'write', str(path), env={'TOTK_CONTRIB_GAME_ID': own})
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(path.read_text(), text)
                self.assertIn('nothing is written', run.stderr)

    def test_the_upload_sends_no_other_games_file(self):
        for game in self.probes:
            for owner in ('someothergame', None):
                with self.subTest(game=game, owner=owner), tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / 'shader-list-contribution.txt'
                    text = (f'# Inbox game: {owner}\n' if owner else '') + f'V\t{"a" * 64}\t{{}}\n'
                    path.write_text(text)
                    run = self.run_probe(game, 'send', str(path))
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertEqual(run.stdout.strip(), '0')
                    self.assertIn('not sent', run.stderr)
                    self.assertEqual(path.read_text(), text)

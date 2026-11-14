#!/usr/bin/env python3
"""dm/tools/triage_report.py: the rules of the triage report, offline.
triage_report_api.json is the GraphQL answer for a few real open PRs and issues of seedds/nxnative (bodies shortened). A temporary Git repository plays main and the
PR heads, so no network and no token are needed."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import triage_report as tr

API = Path(__file__).resolve().parent / 'triage_report_api.json'
REPO = 'seedds/nxnative'
NOW = datetime(2026, 10, 31, 12, 0, tzinfo=timezone.utc)
A_C = ''.join('line %d\n' % i for i in range(1, 13))
VIOLET = 'dm/games/01008F6008C5E000/game.tsv'
ALLSTARS = 'dm/games/010049900F546000/game.tsv'
# the head of each fixture PR, as files it writes on top of the base commit
HEADS = {
    29: {'a.c': A_C.replace('line 1\n', 'pr 29\n')},
    167: {'b.c': 'pr 167\n'},
    193: {ALLSTARS: 'menu\n', 'dm/games/010049900F546800/game.tsv': 'update\n'},
    269: {VIOLET: 'pr 269\n'},
    408: {'.github/workflows/pr-areas.yml': 'on: pull_request_target\n'},
    415: {VIOLET: 'pr 415\n'},
    573: {'dm/games/010003F003A34000/game.tsv': 'pikachu\n'},
    734: {ALLSTARS: 'mario 64\n'},
    756: {'x.c': 'fixed\n'},
}


def load():
    return json.loads(API.read_text(encoding='utf-8'))


class Server:
    """A repository with main and refs/pull/<N>/head, as GitHub has them."""

    def __init__(self, root):
        self.root = root

    def git(self, *args, date='2026-10-01T00:00:00Z'):
        env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
        return subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'init.defaultBranch=main', *args],
                              cwd=self.root, env=env, check=True, capture_output=True, text=True).stdout.strip()

    def commit(self, files, message, date):
        for path, text in files.items():
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text(text)
        self.git('add', '-A')
        self.git('commit', '-qm', message, date=date)
        return self.git('rev-parse', 'HEAD')


def build(root):
    s = Server(root)
    s.git('init', '-q')
    base = s.commit({'a.c': A_C, 'b.c': 'b\n', 'x.c': 'x\n', 'README.md': 'nxnative\n', 'dm/games/0100AE00096EA000/game.tsv': 'hyrule\n'}, 'Base', '2026-10-01T00:00:00Z')
    for n, files in HEADS.items():
        s.git('checkout', '-q', '--detach', base)
        s.git('update-ref', 'refs/pull/%d/head' % n, s.commit(files, 'PR %d' % n, '2026-10-15T00:00:00Z'))
    s.git('checkout', '-q', 'main')
    sha = {
        'touch': s.commit({'a.c': A_C.replace('line 12\n', 'main 12\n')}, 'Touch a.c far from line 1', '2026-10-20T00:00:00Z'),
        'clash': s.commit({'a.c': A_C.replace('line 1\n', 'main 1\n').replace('line 12\n', 'main 12\n')}, 'Rewrite line 1 of a.c', '2026-10-22T00:00:00Z'),
        'violet': s.commit({VIOLET: 'main\n'}, 'Add Pokemon Violet\n\nPR #408 is opened later, so this does not count.', '2026-10-26T08:00:00Z'),
        'landed': s.commit({'b.c': 'reworked\n'}, 'Textures: upload CPU-written textures\n\nThis replaces PR #167.', '2026-10-29T00:00:00Z'),
        'lint': s.commit({'x.c': 'fixed\n'}, 'Fix the lint findings (PR #756)', '2026-10-31T00:00:00Z'),
        'areas': s.commit({'c.c': 'areas\n'}, 'Area labels from the paths (#408)\n\nNot (#29): only a subject names a PR this way.', '2026-10-31T01:00:00Z'),
    }
    return s, sha


class Rules(unittest.TestCase):
    def test_linked_issues(self):
        pr = {'title': 'Fix the menu', 'closingIssuesReferences': {'nodes': [{'number': 1}]}, 'body': (
            'Closes: #2. Part of #3. Refs https://github.com/seedds/nxnative/issues/4. Related to #5, see #6 and issue #7.\n'
            '<!-- Fixes #8 is the template\'s example -->\nStacked on #9. Fixes #10, which is an open PR. Fixes https://github.com/other/repo/issues/11.')}
        self.assertEqual(tr.linked_issues(pr, REPO, {10}), [1, 2, 3, 4, 5, 6, 7])

    def test_fixture_links(self):
        data = load()
        numbers = {p['number'] for p in data['prs']}
        links = {p['number']: tr.linked_issues(p, REPO, numbers) for p in data['prs']}
        self.assertEqual(links[408], [313])   # "Part of #313"
        self.assertEqual(links[573], [224])   # "Related to #224"
        self.assertEqual([n for n, ls in links.items() if ls], [408, 573])

    def test_replied(self):
        issues = {i['number']: i for i in load()['issues']}
        self.assertFalse(tr.replied(issues[10]))    # no comment
        self.assertFalse(tr.replied(issues[542]))   # only its author
        self.assertTrue(tr.replied(issues[313]))
        bot = copy.deepcopy(issues[10])
        bot['comments'] = {'totalCount': 1, 'nodes': [{'author': {'__typename': 'Bot', 'login': 'github-actions'}}]}
        self.assertFalse(tr.replied(bot))
        bot['comments']['nodes'].append({'author': None})   # a deleted account counts as someone else
        bot['comments']['totalCount'] = 2
        self.assertTrue(tr.replied(bot))
        many = copy.deepcopy(issues[542])
        many['comments']['totalCount'] = 31   # more comments than the query reads
        self.assertTrue(tr.replied(many))

    def test_app_id(self):
        self.assertEqual(tr.app_id('010049900F546800'), '010049900F546000')   # update
        self.assertEqual(tr.app_id('010049900F547003'), '010049900F546000')   # add-on content
        self.assertNotEqual(tr.app_id('010049900F548000'), '010049900F546000')

    def test_cell(self):
        self.assertEqual(tr.cell('a | b @someone'), 'a \\| b @\u200bsomeone')
        self.assertEqual(len(tr.cell('x' * 100)), 70)


class Report(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        root = Path(tmp.name).resolve()
        (root / 'server').mkdir()
        _, cls.sha = build(root / 'server')
        subprocess.run(['git', 'clone', '-q', str(root / 'server'), str(root / 'clone')], check=True, capture_output=True)
        cls.clone = str(root / 'clone')
        cls.data = load()
        tr.fetch_heads(cls.clone, 'origin', [p['number'] for p in cls.data['prs']])
        cls.git = tr.analyse_git(cls.data, cls.clone, 'main')
        cls.t = tr.triage(cls.data, cls.git)

    def numbers(self, rows):
        return [p['number'] for p in rows]

    def test_states(self):
        state = {n: a['state'] for n, a in self.git['prs'].items()}
        self.assertEqual(state, {29: 'conflict', 167: 'conflict', 193: 'clean', 269: 'conflict', 408: 'clean', 415: 'conflict', 573: 'clean', 734: 'clean', 756: 'no-op'})
        self.assertEqual(self.git['prs'][29]['conflicts'], ['a.c'])

    def test_no_op_and_named(self):
        self.assertEqual(self.numbers(self.t['no_op']), [756])
        self.assertEqual(self.numbers(self.t['named']), [167, 408])   # 756 is named too, but merges as a no-op
        self.assertEqual(self.git['prs'][167]['named'], [(self.sha['landed'], '2026-10-29T00:00:00Z', 'body')])
        self.assertEqual(self.git['prs'][756]['named'], [(self.sha['lint'], '2026-10-31T00:00:00Z', 'subject')])
        self.assertEqual(self.git['prs'][408]['named'], [(self.sha['areas'], '2026-10-31T01:00:00Z', 'subject')])   # not 'violet', older than the PR
        self.assertEqual(self.git['prs'][29]['named'], [])   # "(#N)" in a body does not count

    def test_conflicts_since(self):
        self.assertEqual(self.numbers(self.t['conflicts']), [29, 269, 415, 167])   # 167 is named by a commit on main and still conflicts
        self.assertEqual(self.git['prs'][29]['since'][0], self.sha['clash'])   # not 'touch', which edits a.c too but merges cleanly
        self.assertEqual(self.git['prs'][269]['since'][0], self.sha['violet'])

    def test_duplicates(self):
        groups = [(g['kind'], g['key'], g['in_main'], self.numbers(g['prs'])) for g in self.t['duplicates']]
        self.assertEqual(groups, [('profile', '010049900F546000', False, [193, 734]), ('profile', '01008F6008C5E000', True, [269, 415])])

    def test_similar_titles_and_files(self):
        prs = [{'number': 1, 'title': 'iOS: swipe camera and touch controls', 'createdAt': '2026-10-01T00:00:00Z'},
               {'number': 2, 'title': 'Touch controls editor for iOS', 'createdAt': '2026-10-02T00:00:00Z'},
               {'number': 3, 'title': 'Touch controls editor for iOS', 'createdAt': '2026-10-03T00:00:00Z'}]
        analysis = {1: {'files': {'dm/ios/a.m': 'M', 'dm/ios/b.m': 'M'}}, 2: {'files': {'dm/ios/a.m': 'M', 'dm/ios/c.m': 'M'}}, 3: {'files': {'docs/x.md': 'M'}}}
        groups = tr.duplicates(prs, analysis, [])
        self.assertEqual([(g['kind'], g['key'], [p['number'] for p in g['prs']]) for g in groups], [('files', ['dm/ios/a.m'], [1, 2])])

    def test_unlinked_and_untriaged(self):
        self.assertEqual(self.numbers(self.t['unlinked']), [29, 167, 193, 269, 415, 734])   # 756 merges as a no-op
        self.assertEqual(self.numbers(self.t['untriaged']), [10, 224, 542])
        self.assertEqual(self.t['naming'], {313: [408], 224: [573]})
        self.assertEqual((self.t['no_reply'], self.t['no_label']), (3, 4))
        self.assertEqual(self.t['skipped'], [])

    def test_markdown(self):
        md = tr.report(self.data, self.git, self.t, NOW, limit=2)
        self.assertIn('| Pull requests that merge as a no-op | 1 |', md)
        self.assertIn('| Pull requests named by a commit on `main` | 2 |', md)
        self.assertIn('| Likely duplicates | 2 groups, 4 pull requests |', md)
        self.assertIn('| Pull requests that conflict with `main` | 4 |', md)
        self.assertIn('body of [`%s`](https://github.com/seedds/nxnative/commit/%s) (2026-10-29) | conflict |' % (self.sha['landed'][:8], self.sha['landed']), md)
        self.assertIn('| subject of [`%s`]' % self.sha['lint'][:8], md)
        self.assertIn('2026-10-22 [`%s`]' % self.sha['clash'][:8], md)
        self.assertIn('profile `01008F6008C5E000`, already in `main`', md)
        self.assertIn('4 more pull requests not shown.', md)
        self.assertIn('| [#224](https://github.com/seedds/nxnative/issues/224) |', md)
        self.assertNotRegex(md.replace('@\u200b', ''), r'@\w')   # nobody is mentioned

    def test_main_load(self):
        with tempfile.TemporaryDirectory() as d:
            saved = Path(d) / 'api.json'
            saved.write_text(json.dumps(self.data))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tr.main(['--load', str(saved), '--git', self.clone, '--limit', '0', '--repo', 'ignored/by-load'])
        md = out.getvalue()
        self.assertTrue(md.startswith('# Triage report\n'))
        self.assertIn('[#734](https://github.com/seedds/nxnative/pull/734)', md)
        self.assertNotIn('not shown', md)


if __name__ == '__main__':
    unittest.main()

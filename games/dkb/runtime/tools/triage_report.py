#!/usr/bin/env python3
"""A triage report on the open pull requests and issues of a GitHub repository, as Markdown (.github/workflows/triage-report.yml puts it in the job summary).
Fixed rules over the GitHub API and git, no judgement: it lists, and never closes, labels or comments. docs/development.md (Triage report) has the rules and their limits.

  triage_report.py [--repo OWNER/NAME] [--git DIR] [--main BRANCH] [--fetch REMOTE] [--limit N] [--save FILE | --load FILE]

The API part (open PRs and issues, a few GraphQL queries) needs a token in GITHUB_TOKEN or GH_TOKEN. The git part needs a clone with main's history and the head of
every open PR as refs/triage/pull/<N>: --fetch REMOTE fetches them from REMOTE's refs/pull/<N>/head. Nothing from a pull request is checked out or run: git merges
trees in its object store (merge-tree) and reads commit messages."""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone

GRAPHQL = 'https://api.github.com/graphql'
PRS = '''query($owner: String!, $name: String!, $after: String) { repository(owner: $owner, name: $name) {
  pullRequests(states: OPEN, first: 50, after: $after, orderBy: {field: CREATED_AT, direction: ASC}) { pageInfo { hasNextPage endCursor }
    nodes { number title url createdAt isDraft baseRefName author { login } body closingIssuesReferences(first: 10) { nodes { number } } } } } }'''
ISSUES = '''query($owner: String!, $name: String!, $after: String) { repository(owner: $owner, name: $name) {
  issues(states: OPEN, first: 100, after: $after, orderBy: {field: CREATED_AT, direction: ASC}) { pageInfo { hasNextPage endCursor }
    nodes { number title url createdAt author { login } labels(first: 10) { nodes { name } } comments(first: 30) { totalCount nodes { author { __typename login } } } } } } }'''

PR_REF = 'refs/triage/pull/%d'
NAMED = re.compile(r'\bPRs?\s*#\d+(?:\s*(?:,|and|&)\s*#\d+)*')   # "PR #709", "(PR #667, #681)": how a commit on main names the pull request it takes in
NAMED_SUBJECT = re.compile(r'\(#(\d+)\)')   # "... (#408)" in a subject, GitHub's squash merge and main's own convention
LINK_WORDS = r'close[sd]?|fix(?:e[sd])?|resolve[sd]?|part\s+of|refs?|references|relate[sd]\s+to|see|issue'
PROFILE = re.compile(r'dm/games/([0-9A-Fa-f]{16})/game\.tsv$')
TITLE_STOP = {'the', 'and', 'for', 'with', 'from', 'into', 'when', 'not', 'are', 'its', 'that', 'this', 'add', 'adds', 'fix', 'fixes', 'support', 'game'}
SIMILAR_TITLE, SIMILAR_FILES = 0.3, 0.15   # likely duplicates by content: Jaccard index of the title words, and of the changed files, at least these


# ---- GitHub API ----

def graphql(query, variables, token):
    req = urllib.request.Request(GRAPHQL, json.dumps({'query': query, 'variables': variables}).encode(),
                                 {'Authorization': 'bearer ' + token, 'Content-Type': 'application/json', 'User-Agent': 'triage_report.py'})
    with urllib.request.urlopen(req, timeout=120) as r:
        out = json.load(r)
    if out.get('errors'):
        sys.exit('GraphQL: ' + json.dumps(out['errors'])[:500])
    return out['data']


def fetch_api(repo, token):
    """Every open PR and issue, oldest first: 4 queries for 200 PRs and 150 issues."""
    owner, name = repo.split('/')
    out = {'repo': repo}
    for key, field, query in (('prs', 'pullRequests', PRS), ('issues', 'issues', ISSUES)):
        out[key], after = [], None
        while True:
            conn = graphql(query, {'owner': owner, 'name': name, 'after': after}, token)['repository'][field]
            out[key] += conn['nodes']
            if not conn['pageInfo']['hasNextPage']:
                break
            after = conn['pageInfo']['endCursor']
    return out


def login(node):
    return (node.get('author') or {}).get('login') or 'ghost'


def linked_issues(pr, repo, pr_numbers):
    """Issues a PR names: those GitHub links (closing keywords, the sidebar) and `#N` or an issue URL after Part of, Refs, See, Issue, Related to and the closing
    keywords, in its title or body (HTML comments left out). Numbers of open PRs are left out."""
    text = re.sub(r'<!--.*?-->', '', (pr.get('title') or '') + '\n' + (pr.get('body') or ''), flags=re.S)
    rx = r'\b(?:%s)\b:?\s+(?:#|https://github\.com/%s/issues/)(\d+)\b' % (LINK_WORDS, re.escape(repo))
    found = {int(n) for n in re.findall(rx, text, re.I)} | {n['number'] for n in pr['closingIssuesReferences']['nodes']}
    return sorted(found - pr_numbers)


def replied(issue):
    """A comment from someone other than the issue's author and other than a bot."""
    c = issue['comments']
    if c['totalCount'] > len(c['nodes']):
        return True
    return any(n['author'] is None or (n['author']['__typename'] != 'Bot' and n['author']['login'] != login(issue)) for n in c['nodes'])


# ---- git ----

def git(repo, *args, ok=(0,)):
    r = subprocess.run(['git', '-C', repo, *args], capture_output=True, text=True)
    if r.returncode not in ok:
        raise RuntimeError('git %s: %s' % (args[0], r.stderr.strip()[:300]))
    return r


def fetch_heads(repo, remote, numbers):
    for i in range(0, len(numbers), 100):
        git(repo, 'fetch', '--no-tags', '--quiet', remote, *['+refs/pull/%d/head:%s' % (n, PR_REF % n) for n in numbers[i:i + 100]])


def merge(repo, base, head):
    """(clean, tree, conflicting paths) of merging head into base, in the object store only."""
    r = git(repo, 'merge-tree', '--write-tree', '--name-only', '--no-messages', base, head, ok=(0, 1))
    lines = [x for x in r.stdout.splitlines() if x]
    return r.returncode == 0, lines[0], lines[1:]


def commit_date(repo, rev):
    return git(repo, 'show', '-s', '--format=%cI', rev).stdout.strip()


def first_conflict(repo, merge_base, main, head, paths):
    """The first commit on main after the merge base that head does not merge cleanly with, by bisection over main's commits that touch the PR's files. It assumes
    that a conflict stays once it appears; None when that does not hold."""
    spec = ['--', *paths] if 0 < len(paths) <= 500 else []
    commits = git(repo, 'rev-list', '--first-parent', '--reverse', '%s..%s' % (merge_base, main), *spec).stdout.split()
    if not commits:
        return None
    lo, hi = 0, len(commits) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if merge(repo, commits[mid], head)[0]:
            lo = mid + 1
        else:
            hi = mid
    if merge(repo, commits[lo], head)[0]:
        return None
    return commits[lo], commit_date(repo, commits[lo])


def analyse(repo, main, main_tree, number):
    """What merging PR <number> into main does: state no-op, clean or conflict (with the paths and since when), or missing / unrelated when git cannot tell."""
    head = PR_REF % number
    if git(repo, 'rev-parse', '-q', '--verify', head + '^{commit}', ok=(0, 1)).returncode:
        return {'state': 'missing', 'files': {}}
    r = git(repo, 'merge-base', main, head, ok=(0, 1))
    if r.returncode:
        return {'state': 'unrelated', 'files': {}}
    mb = r.stdout.strip()
    diff = git(repo, 'diff', '--name-status', '--no-renames', '-z', mb, head).stdout.split('\0')
    files = {diff[i + 1]: diff[i] for i in range(0, len(diff) - 1, 2)}
    clean, tree, conflicts = merge(repo, main, head)
    if clean:
        return {'state': 'no-op' if tree == main_tree else 'clean', 'files': files}
    return {'state': 'conflict', 'files': files, 'conflicts': conflicts, 'since': first_conflict(repo, mb, main, head, list(files))}


def named_on_main(repo, main, since):
    """PR number → [(commit, date, 'subject' or 'body')], oldest first: commits on main since `since` whose message names the PR as "PR #N", or whose subject
    names it as "(#N)"."""
    out = {}
    log = git(repo, 'log', '--first-parent', '--reverse', '--since=' + since, '--format=%H%x1f%cI%x1f%s%x1f%b%x1e', main).stdout
    for rec in log.split('\x1e'):
        if rec.strip():
            sha, date, subject, body = rec.strip('\n').split('\x1f', 3)
            for where, text in (('subject', subject), ('body', body)):
                found = {int(n) for m in NAMED.finditer(text) for n in re.findall(r'\d+', m.group(0))}
                if where == 'subject':
                    found |= {int(n) for n in NAMED_SUBJECT.findall(text)}
                for n in found:
                    if not any(c[0] == sha for c in out.get(n, [])):
                        out.setdefault(n, []).append((sha, date, where))
    return out


def main_profiles(repo, main):
    return {p.rsplit('/', 1)[-1].upper() for p in git(repo, 'ls-tree', '--name-only', main, 'dm/games/').stdout.split()}


def analyse_git(data, repo, main):
    """Per PR number: the merge analysis, plus `named` (main's commits that name it, made after it was opened)."""
    main_tree = git(repo, 'rev-parse', main + '^{tree}').stdout.strip()
    prs = data['prs']
    named = named_on_main(repo, main, min((p['createdAt'] for p in prs), default='1970-01-01T00:00:00Z'))
    out = {}
    for p in prs:
        a = analyse(repo, main, main_tree, p['number']) if p['baseRefName'] == main else {'state': 'other base', 'files': {}}
        a['named'] = [c for c in named.get(p['number'], []) if when(c[1]) >= when(p['createdAt'])]
        out[p['number']] = a
    return {'main': git(repo, 'rev-parse', main).stdout.strip(), 'profiles': sorted(main_profiles(repo, main)), 'prs': out}


# ---- rules ----

def when(iso):
    return datetime.fromisoformat(iso.replace('Z', '+00:00'))


def app_id(title_id):
    """A title ID's application: an update (+0x800) and add-on content (+0x1000 + n) share the base game's."""
    return '%016X' % (int(title_id, 16) & ~0x1FFF)


def words(title):
    return {w for w in re.findall(r'[a-z0-9]+', title.lower()) if len(w) > 2 and w not in TITLE_STOP}


def jaccard(a, b):
    return len(a & b) / len(a | b) if a and b else 0.0


def duplicates(prs, analysis, profiles):
    """Groups of PRs: those adding a profile (game.tsv) for the same application, or for one main already has; and PRs whose title words and changed files overlap
    by at least SIMILAR_TITLE and SIMILAR_FILES (Jaccard index), joined transitively."""
    main_apps = {app_id(t) for t in profiles if re.fullmatch(r'[0-9A-F]{16}', t)}
    by_app = {}
    for p in prs:
        for path, status in analysis[p['number']]['files'].items():
            m = PROFILE.match(path)
            if m and status == 'A' and p not in by_app.setdefault(app_id(m.group(1)), []):
                by_app[app_id(m.group(1))].append(p)
    groups = [{'kind': 'profile', 'key': t, 'in_main': t in main_apps, 'prs': ps} for t, ps in by_app.items() if len(ps) > 1 or t in main_apps]
    parent = {p['number']: p['number'] for p in prs}

    def root(n):
        while parent[n] != n:
            n = parent[n]
        return n
    for i, a in enumerate(prs):
        fa, wa = set(analysis[a['number']]['files']), words(a['title'])
        for b in prs[i + 1:]:
            fb = set(analysis[b['number']]['files'])
            if jaccard(wa, words(b['title'])) >= SIMILAR_TITLE and jaccard(fa, fb) >= SIMILAR_FILES:
                parent[root(b['number'])] = root(a['number'])
    sets = {}
    for p in prs:
        sets.setdefault(root(p['number']), []).append(p)
    in_profile_groups = [{q['number'] for q in g['prs']} for g in groups]
    for ps in sets.values():
        if len(ps) > 1 and {q['number'] for q in ps} not in in_profile_groups:
            common = set.intersection(*(set(analysis[q['number']]['files']) for q in ps))
            groups.append({'kind': 'files', 'key': sorted(common), 'in_main': False, 'prs': ps})
    for g in groups:
        g['prs'].sort(key=lambda q: q['createdAt'])
    return sorted(groups, key=lambda g: g['prs'][0]['createdAt'])


def triage(data, git_data):
    """The report's rows, by rule."""
    repo, analysis = data['repo'], git_data['prs']
    prs = sorted(data['prs'], key=lambda p: p['createdAt'])
    numbers = {p['number'] for p in prs}
    links = {p['number']: linked_issues(p, repo, numbers) for p in prs}
    no_op = [p for p in prs if analysis[p['number']]['state'] == 'no-op']
    rest = [p for p in prs if analysis[p['number']]['state'] != 'no-op']
    named = [p for p in rest if analysis[p['number']]['named']]   # often a PR main took only part of, or one a commit just mentions: it stays in the other tables
    conflicts = sorted((p for p in rest if analysis[p['number']]['state'] == 'conflict'),
                       key=lambda p: (analysis[p['number']]['since'] or ('', '9999'))[1])
    naming = {}
    for p in rest:
        for i in links[p['number']]:
            naming.setdefault(i, []).append(p['number'])
    issues = sorted(data['issues'], key=lambda i: i['createdAt'])
    return {
        'no_op': no_op, 'named': named, 'duplicates': duplicates(rest, analysis, git_data['profiles']), 'conflicts': conflicts,
        'unlinked': [p for p in rest if not links[p['number']]], 'links': links,
        'untriaged': [i for i in issues if not replied(i) and not i['labels']['nodes']], 'naming': naming,
        'no_reply': sum(not replied(i) for i in issues), 'no_label': sum(not i['labels']['nodes'] for i in issues),
        'skipped': [p for p in rest if analysis[p['number']]['state'] in ('missing', 'unrelated', 'other base')],
    }


# ---- Markdown ----

def cell(text, n=70):
    text = ' '.join((text or '').split()).replace('|', '\\|').replace('@', '@\u200b')   # no @mention in an issue body
    return text if len(text) <= n else text[:n - 1] + '…'


def ref(item):
    return '[#%d](%s)' % (item['number'], item['url'])


def title(p):
    return ('draft: ' if p.get('isDraft') else '') + cell(p['title'])


def commit_link(repo, sha):
    return '[`%s`](https://github.com/%s/commit/%s)' % (sha[:8], repo, sha)


def table(head, rows, limit, what):
    out = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)] + ['| ' + ' | '.join(r) + ' |' for r in rows[:limit or None]]
    if limit and len(rows) > limit:
        out.append('\n%d more %s not shown.' % (len(rows) - limit, what))
    return out if rows else ['None.']


def report(data, git_data, t, now, limit=25):
    repo, analysis = data['repo'], git_data['prs']
    day = lambda iso: iso[:10]
    issue_refs = lambda ns: ', '.join('#%d' % n for n in ns) or '-'
    drafts = sum(bool(p.get('isDraft')) for p in data['prs'])
    in_dups = sum(len(g['prs']) for g in t['duplicates'])
    out = [
        '# Triage report', '',
        '%s, %d open pull requests (%d drafts) and %d open issues on %s, against `main` at %s. Fixed rules over the API and git, listed in '
        '[docs/development.md](https://github.com/%s/blob/main/docs/development.md#triage-report). The report never closes, labels or comments.'
        % (now.strftime('%Y-%m-%d %H:%M UTC'), len(data['prs']), drafts, len(data['issues']), repo, commit_link(repo, git_data['main']), repo), '',
        '| | |', '|---|---|',
        '| Pull requests that merge as a no-op | %d |' % len(t['no_op']),
        '| Pull requests named by a commit on `main` | %d |' % len(t['named']),
        '| Likely duplicates | %d groups, %d pull requests |' % (len(t['duplicates']), in_dups),
        '| Pull requests that conflict with `main` | %d |' % len(t['conflicts']),
        '| Pull requests without a linked issue | %d |' % len(t['unlinked']),
        '| Issues with no reply and no label | %d (%d with no reply, %d with no label) |' % (len(t['untriaged']), t['no_reply'], t['no_label']),
        '', '## Merges as a no-op', '',
        'Merging the PR into `main` leaves `main`\'s tree as it is: its change is in `main` already. Oldest first. These PRs show in no other table.', '']
    named_by = lambda a: ', '.join('%s of %s (%s)' % (w, commit_link(repo, s), day(d)) for s, d, w in a['named'][:2]) or '-'
    rows = [[ref(p), title(p), login(p), day(p['createdAt']), named_by(analysis[p['number']]), issue_refs(t['links'][p['number']])] for p in t['no_op']]
    out += table(['PR', 'Title', 'Author', 'Opened', 'Named by', 'Issues'], rows, limit, 'pull requests')
    out += ['', '## Named by a commit on `main`', '',
            'A commit on `main` made after the PR was opened names it as "PR #N", or as "(#N)" in its subject. The commit may have taken all of it, part of it, '
            'or only mention it: read it before closing. "Merges" is what merging the PR into `main` does now. Oldest first.', '']
    rows = [[ref(p), title(p), login(p), day(p['createdAt']), named_by(analysis[p['number']]), analysis[p['number']]['state'],
             issue_refs(t['links'][p['number']])] for p in t['named']]
    out += table(['PR', 'Title', 'Author', 'Opened', 'Named by', 'Merges', 'Issues'], rows, limit, 'pull requests')
    out += ['', '## Likely duplicates', '',
            'Open PRs that add a profile for the same game (by title ID), or for one `main` already has. Also PRs whose title words overlap by at least %d%% and whose '
            'changed files overlap by at least %d%%. Oldest PR first.' % (SIMILAR_TITLE * 100, SIMILAR_FILES * 100), '']
    rows = []
    for g in t['duplicates']:
        if g['kind'] == 'profile':
            what = 'profile `%s`%s' % (g['key'], ', already in `main`' if g['in_main'] else '')
        else:
            what = ', '.join('`%s`' % f for f in g['key'][:2]) + (' +%d' % (len(g['key']) - 2) if len(g['key']) > 2 else '') if g['key'] else 'similar titles and files'
        rows.append([what, ', '.join('%s %s (%s, %s)' % (ref(p), cell(p['title'], 50), login(p), day(p['createdAt'])) for p in g['prs'])])
    out += table(['Same', 'Pull requests'], rows, limit, 'groups')
    out += ['', '## Conflicts with `main`', '',
            '"Since" is the first commit on `main` after the PR\'s merge base that the PR no longer merges cleanly with. A rebase moves it. Longest first.', '']
    rows = []
    for p in t['conflicts']:
        a = analysis[p['number']]
        since = '%s %s' % (day(a['since'][1]), commit_link(repo, a['since'][0])) if a['since'] else 'unknown'
        files = ', '.join('`%s`' % f for f in a['conflicts'][:2]) + (' +%d' % (len(a['conflicts']) - 2) if len(a['conflicts']) > 2 else '')
        rows.append([ref(p), title(p), login(p), since, files])
    out += table(['PR', 'Title', 'Author', 'Since', 'Conflicting files'], rows, limit, 'pull requests')
    out += ['', '## Pull requests without a linked issue', '',
            'No issue linked on GitHub, and none named as "Fixes #N", "Part of #N", "Refs #N", "Issue #N" or similar in the title or body. Oldest first.', '']
    rows = [[ref(p), title(p), login(p), day(p['createdAt'])] for p in t['unlinked']]
    out += table(['PR', 'Title', 'Author', 'Opened'], rows, limit, 'pull requests')
    out += ['', '## Issues with no reply and no label', '',
            'No comment from anyone but the author or a bot, and no label. "Named by" lists open PRs that link the issue. Oldest first.', '']
    rows = [[ref(i), cell(i['title']), login(i), day(i['createdAt']), issue_refs(t['naming'].get(i['number'], []))] for i in t['untriaged']]
    out += table(['Issue', 'Title', 'Author', 'Opened', 'Named by'], rows, limit, 'issues')
    if t['skipped']:
        out += ['', 'Not analysed by git (head not fetched, no common history with `main`, or another base branch): %s.'
                % ', '.join('%s (%s)' % (ref(p), analysis[p['number']]['state']) for p in t['skipped'])]
    return '\n'.join(out) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description='Triage report on open pull requests and issues (Markdown on stdout)')
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', 'seedds/nxnative'), help='OWNER/NAME (default: $GITHUB_REPOSITORY or seedds/nxnative)')
    ap.add_argument('--git', default='.', help='a clone with main and the PR heads (default: the current folder)')
    ap.add_argument('--main', default='main', help='the branch PRs are checked against (default: main)')
    ap.add_argument('--fetch', metavar='REMOTE', help="fetch every open PR's head from REMOTE into refs/triage/pull/<N> first")
    ap.add_argument('--limit', type=int, default=25, help='rows per table, 0 for all (default: 25)')
    ap.add_argument('--save', metavar='FILE', help="write the API's answer here (JSON)")
    ap.add_argument('--load', metavar='FILE', help="read the API's answer from FILE instead of asking the API")
    a = ap.parse_args(argv)
    if a.load:
        with open(a.load, encoding='utf-8') as f:
            data = json.load(f)
    else:
        token = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')
        if not token:
            sys.exit('set GITHUB_TOKEN or GH_TOKEN')
        data = fetch_api(a.repo, token)
    if a.save:
        with open(a.save, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=1)
    if a.fetch:
        fetch_heads(a.git, a.fetch, [p['number'] for p in data['prs']])
    git_data = analyse_git(data, a.git, a.main)
    sys.stdout.write(report(data, git_data, triage(data, git_data), datetime.now(timezone.utc), a.limit))


if __name__ == '__main__':
    main()

"""The rewriter's per-build patch files (dm/tools/sdk_patches.py, dm/games/<id>/patches.py, loaded by dm/tools/patch_nso.py): no build ID is claimed twice, every entry has
the form patch_nso.py expects, a game's file names only builds of its own profile's non-sdk modules, sdk_patches.py only sdk modules some profile ships; and the NSP script's
rewrite step keys its reuse on the patch files of the game's builds only. No game files needed (temporary files only in a temporary folder)."""
import ast
import glob
import importlib.util
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/games'))
import game   # noqa: E402
import docs_patches   # noqa: E402

FILES = [REPO / 'dm/tools/sdk_patches.py'] + sorted((REPO / 'dm/games').glob('*/patches.py'))


def load(path, n):
    spec = importlib.util.spec_from_file_location(f'patches_{n}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


MODS = {p: load(p, n) for n, p in enumerate(FILES)}
HEX = re.compile(r'[0-9a-f]+$')

# Execute the driver's actual file discovery and registry without loading a game image.
source = REPO / 'dm/tools/patch_nso.py'; tree = ast.parse(source.read_text())
start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'PATCH_FILES')
end = next(i for i, n in enumerate(tree.body[start:], start) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'BUILD_ID')
REGISTRY_CODE = compile(ast.Module(body=tree.body[start:end], type_ignores=[]), str(source), 'exec')


def builds_of(g):   # {build ID: module name} of a profile's build rows (a profile or its title ID)
    return {bid: m for row in (game.load(g) if isinstance(g, str) else g).builds().values() for m, bid in row.items() if m != 'display'}


class Registry(unittest.TestCase):
    def test_no_build_claimed_twice(self):
        for kind in ('CUTS', 'OBS', 'FRAME_STEP', 'CAPABILITIES'):
            seen = {}
            for p, m in MODS.items():
                for bid in getattr(m, kind, {}):
                    self.assertNotIn(bid, seen, f'{kind} of {bid} in {seen.get(bid)} and {p}'); seen[bid] = p

    def test_entry_forms(self):
        for p, m in MODS.items():
            for bid, cuts in getattr(m, 'CUTS', {}).items():
                self.assertRegex(bid, r'^[0-9a-f]{32}$')
                for off, ob, nb, why in cuts:
                    self.assertTrue(isinstance(off, int) and HEX.match(ob) and HEX.match(nb) and len(ob) == len(nb) and len(ob) % 8 == 0 and why, f'{p} {bid} {off:#x}')
            for bid, gates in getattr(m, 'OBS', {}).items():
                for off, oid, word, why in gates:
                    self.assertTrue(isinstance(off, int) and isinstance(oid, int) and HEX.match(word) and len(word) == 8 and why, f'{p} {bid} {off:#x}')
            for bid, fn in getattr(m, 'FRAME_STEP', {}).items():
                self.assertTrue(callable(fn), f'{p} {bid}')
            for bid, names in getattr(m, 'CAPABILITIES', {}).items():
                self.assertRegex(bid, r'^[0-9a-f]{32}$')
                self.assertEqual(len(names), len(set(names)))
                for name in names: self.assertRegex(name, r'^[A-Z][A-Z0-9_]*$')
            self.assertFalse({k for k in vars(m) if k.isupper() and k not in ('CUTS', 'OBS', 'FRAME_STEP', 'CAPABILITIES', 'MATCH_CUTS', 'BINDING')} - set(dir(__import__('nso_asm'))),
                             f'{p}: a table patch_nso.py does not read')
            # every patch file states each patch's binding (docs_patches.py "Binding"), and no goal is tighter than the binding now
            self.assertTrue(getattr(m, 'BINDING', None), f'{p}: no BINDING (docs/agents/game-patches.md, "Binding")')
            for e in m.BINDING: self.assertIsNone(docs_patches.binding_error(e, own=p.name == 'patches.py'), f'{p}')

    def test_unreviewed_goals_stay_in_a_games_own_build_bound_patches(self):
        e = {'what': 'a cut', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'memory', 'why': 'found at bring-up'}
        self.assertIsNone(docs_patches.binding_error(e, own=True))
        self.assertIsNotNone(docs_patches.binding_error(e))   # sdk_patches.py, host rows, translator rules and switches review theirs
        self.assertIsNotNone(docs_patches.binding_error(dict(e, now='game'), own=True))
        self.assertIsNotNone(docs_patches.binding_error(dict(e, blocked='why'), own=True))
        self.assertEqual(docs_patches.status(e), 'unreviewed')
        reviewed = (dict(e, goal='sdk'), [], {'a', 'b'})
        self.assertLess(docs_patches.payoff(reviewed), docs_patches.payoff((e, [], {'a', 'b', 'c'})))   # after every reviewed gap

    def test_function_match_rules(self):
        from nso_match import validate
        for p, m in MODS.items():
            for module, rules in getattr(m, 'MATCH_CUTS', {}).items():
                if p.name == 'sdk_patches.py':   # SDK-wide: Nintendo's sdk module of any game
                    self.assertEqual(module, 'sdk')
                else:
                    self.assertEqual(p.name, 'patches.py')
                    self.assertIn(module, game.load(p.parent.name).words('modules'))
                    self.assertNotEqual(module, 'sdk', 'SDK fixes belong in sdk_patches.py, for every game')
                self.assertTrue(rules)
                for rule in rules: validate(rule)

    def test_sdk_wide_rules_change_what_their_names_say(self):
        # the rules were taken from registered SDK cuts, and the builds they reproduce exactly have no row any more: each cut replaces the constant its rule names (0x3fe00000, the node
        # area; 0x400000, 0x2000000 and 0x40000000, the alignments of the 4 MB, 32 MB and 1 GB classes; as movz or orr from wzr) with the reviewed one (62 MB; SDK 20's 2 MB) in the same register
        m = MODS[REPO / 'dm/tools/sdk_patches.py']
        words = {m.node_area: ({'08fca752', '09fca752', 'e8230b32', 'e9230b32'}, {'087ca052', '097ca052'}),
                 m.alignment: ({'0808a052', 'e8030a32', '0b40a052', 'eb030732', '0a00a852', 'ea030232'}, {'0804a052', '0b04a052', '0a04a052'})}
        for rule in m.MATCH_CUTS['sdk']:
            old_words, new_words = words[rule['name']]
            for delta, old, new, why in rule['cuts']:
                self.assertIn(old, old_words, why); self.assertIn(new, new_words, why)
                self.assertEqual(int(old[:2], 16) & 31, int(new[:2], 16) & 31, why)

    def test_game_files_name_their_own_builds(self):
        for p, m in MODS.items():
            if p.name != 'patches.py' or not (p.parent / 'game.tsv').is_file(): continue
            own = builds_of(p.parent.name)
            for kind in ('CUTS', 'OBS', 'FRAME_STEP', 'CAPABILITIES'):
                for bid in getattr(m, kind, {}):
                    self.assertIn(bid, own, f'{p}: {kind} of {bid}, which is no build of profile {p.parent.name}')
                    self.assertNotEqual(own[bid], 'sdk', f'{p}: {bid} is an sdk build; its patches go in dm/tools/sdk_patches.py')

    def test_sdk_file_names_sdk_builds(self):
        sdk = {bid for g in game.all_games() for bid, m in builds_of(g).items() if m == 'sdk'}
        m = MODS[REPO / 'dm/tools/sdk_patches.py']
        self.assertFalse(getattr(m, 'OBS', {}) or getattr(m, 'FRAME_STEP', {}))
        for bid in m.CUTS: self.assertIn(bid, sdk, f'sdk_patches.py: {bid} is no sdk build of any profile')


class ExternalRegistry(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.tools = self.tmp / 'repo/tools'; self.tools.mkdir(parents=True)
        (self.tools / 'sdk_patches.py').write_text('')
        self.gid = '0100000000000000'
        self.external = self.tmp / 'profiles'

    def patch_file(self, root, text):
        p = root / self.gid / 'patches.py'; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
        return p

    def registry(self, roots=None):
        ns = dict(__file__=str(self.tools / 'patch_nso.py'), os=os, glob=glob, importlib=importlib)
        with mock.patch.dict(os.environ, DM_GAMES_DIR=os.pathsep.join(map(str, roots or [self.external]))):
            exec(REGISTRY_CODE, ns)
        return ns['PATCHES'], ns['PATCH_WHERE']

    def test_external_registered_patches_are_loaded(self):
        bid = '5e' * 16
        p = self.patch_file(self.external, f"CUTS = {{'{bid}': []}}\nOBS = {{'{bid}': []}}\nFRAME_STEP = {{'{bid}': lambda ctx: None}}\n")
        tables, where = self.registry()
        for kind in ('CUTS', 'OBS', 'FRAME_STEP'):
            self.assertIn(bid, tables[kind]); self.assertEqual(Path(where[kind, bid]), p)

    def test_derived_profile_keeps_repository_match_rules(self):
        self.patch_file(self.tools.parent / 'games', "MATCH_CUTS = {'main': ['repository']}\n")
        self.patch_file(self.external, "MATCH_CUTS = {'main': ['external']}\n")
        tables, _ = self.registry()
        self.assertEqual(tables['MATCH_CUTS'][self.gid], {'main': ['repository']})

    def test_external_rules_cannot_enroll_a_repository_profile(self):
        (self.tools.parent / 'games' / self.gid).mkdir(parents=True)
        self.patch_file(self.external, "MATCH_CUTS = {'main': ['external']}\n")
        tables, _ = self.registry()
        self.assertNotIn(self.gid, tables['MATCH_CUTS'])

    def test_provisional_profile_can_supply_match_rules(self):
        self.patch_file(self.external, "MATCH_CUTS = {'main': ['external']}\n")
        tables, _ = self.registry()
        self.assertEqual(tables['MATCH_CUTS'][self.gid], {'main': ['external']})

    def test_provisional_rules_follow_the_first_profile(self):
        second = self.tmp / 'second'
        self.patch_file(second, "MATCH_CUTS = {'main': ['second']}\n")
        first = self.patch_file(self.external, "MATCH_CUTS = {'main': ['first']}\n")
        tables, _ = self.registry([self.external, second])
        self.assertEqual(tables['MATCH_CUTS'][self.gid], {'main': ['first']})
        first.unlink()   # a selected profile with no rules must not inherit another directory's rules
        tables, _ = self.registry([self.external, second])
        self.assertNotIn(self.gid, tables['MATCH_CUTS'])

    def test_repeated_roots_do_not_load_a_file_twice(self):
        self.patch_file(self.external, "CUTS = {'" + '5e' * 16 + "': []}\n")
        alias = self.tmp / 'alias'; alias.symlink_to(self.external, target_is_directory=True)
        tables, _ = self.registry([self.external, self.external, alias])
        self.assertEqual(len(tables['CUTS']), 1)

    def test_distinct_files_cannot_claim_the_same_build(self):
        text = "CUTS = {'" + '5e' * 16 + "': []}\n"
        self.patch_file(self.tools.parent / 'games', text)
        self.patch_file(self.external, text)
        with self.assertRaisesRegex(AssertionError, 'in both'):
            self.registry()


class RewriteKey(unittest.TestCase):
    def test_patch_files_of_the_games_builds(self):
        spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
        nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        gid = '0100ECD018EBE000'   # Paper Mario: The Thousand-Year Door: a main in its own patches.py, an sdk build in sdk_patches.py
        for bid, m in builds_of(gid).items():
            if m in ('main', 'sdk'): (tmp / m).write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid))
        got = nsp.patch_files(REPO, tmp, ['main', 'sdk'])
        self.assertEqual(got, [REPO / 'dm/tools/sdk_patches.py', REPO / 'dm/games' / gid / 'patches.py'])
        (tmp / 'main').write_bytes(b'NSO0' + bytes(0x3c) + bytes(16)); (tmp / 'sdk').write_bytes(b'NSO0' + bytes(0x3c) + bytes(16))
        sdk_wide = REPO / 'dm/tools/sdk_patches.py'   # its SDK-wide MATCH_CUTS apply to any build, so it always counts
        self.assertEqual(nsp.patch_files(REPO, tmp, ['main', 'sdk']), [sdk_wide])   # an unregistered build: no game's patch file counts
        self.assertEqual(nsp.patch_files(REPO, tmp, ['main', 'sdk'], gid), [sdk_wide, REPO / 'dm/games' / gid / 'patches.py'])
        g = game.load(gid)
        self.assertNotIn(REPO / 'dm/games' / gid / 'patches.py', nsp.profile_files(g))   # only the rewrite step reads it

    def test_patch_files_of_a_provisional_profile(self):
        spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
        nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        bid = '5e' * 16   # a build that no repository profile knows
        (tmp / 'exe').mkdir(); (tmp / 'exe/main').write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid))
        mine, other = tmp / 'profiles/0100000000000000/patches.py', tmp / 'profiles/0100000000000001/patches.py'
        for f, text in ((mine, f"CUTS = {{'{bid}': []}}\n"), (other, "CUTS = {'" + '6f' * 16 + "': []}\n")):
            f.parent.mkdir(parents=True); f.write_text(text)
        sdk_wide = REPO / 'dm/tools/sdk_patches.py'
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(tmp / 'profiles')}):
            self.assertEqual(nsp.patch_files(REPO, tmp / 'exe', ['main']), [sdk_wide, mine])   # of the games' files, only the one that names the module's build
            self.assertEqual(nsp.patch_files(REPO, tmp / 'exe', ['main'], '0100000000000001'), [sdk_wide, mine, other])   # the selected game's file counts for any build
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': ''}):
            self.assertEqual(nsp.patch_files(REPO, tmp / 'exe', ['main']), [sdk_wide])
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': os.pathsep.join([str(tmp / 'profiles')] * 2)}):
            self.assertEqual(nsp.patch_files(REPO, tmp / 'exe', ['main']), [sdk_wide, mine])


if __name__ == '__main__':
    unittest.main()

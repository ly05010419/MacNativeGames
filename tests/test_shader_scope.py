"""The runtime only filters warm-up when its bundled shader inventory is valid."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
CHAIN = re.search(r'#define DMSH_CHAIN "([^"]+)"', (INCLUDE/'dmsh_chain.h').read_text())[1]
KEY = 'a'*64
VARIANT = KEY+'-'+'b'*12+'_b'


@unittest.skipUnless(shutil.which('xcrun'), 'native Foundation scope loader is Mac-only')
class ShaderScope(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        source = cls.root/'probe.m'
        source.write_text('''#import "totk_shader_scope.h"
#import "totk_shader_seed.h"
#import "totk_shader_observed.h"
int main(int argc, const char **argv) { @autoreleasepool {
 NSError *error = nil; NSDictionary *result = nil;
 if (argc == 4 && !strcmp(argv[1], "--import")) result = totk_seed_import(@(argv[2]), @(argv[3]), &error);
 else if (argc == 4 && !strcmp(argv[1], "--fill")) result = totk_fill_import(@(argv[2]), @(argv[3]));
 else if (argc == 6 && !strcmp(argv[1], "--observed")) {
   NSMutableDictionary *state = totk_shader_observed_open(totk_shader_scope_metadata(@(argv[2]), @(argv[4])), @(argv[3]), @(argv[4]));
   NSArray *queries = [NSJSONSerialization JSONObjectWithData:[@(argv[5]) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
   NSMutableArray *added = [NSMutableArray array];
   for (id key in queries) [added addObject:@(totk_shader_observed_note(state, key))];
   result = @{@"added": added, @"keys": [[totk_shader_observed_keys(state) allObjects] sortedArrayUsingSelector:@selector(compare:)]};
 }
 else if (argc == 5 && !strcmp(argv[1], "--scope")) {
   NSDictionary *metadata = totk_shader_scope_metadata(@(argv[2]), @(argv[3]));
   NSSet<NSString *> *scope = metadata ? [NSSet setWithArray:metadata[@"keys"]] : nil;
   NSArray *queries = [NSJSONSerialization JSONObjectWithData:[@(argv[4]) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
   NSMutableArray *answers = [NSMutableArray array];
   for (id key in queries) [answers addObject:@(totk_shader_scope_contains(scope, key))];
   result = @{@"loaded": @(scope != nil), @"count": @(scope.count), @"answers": answers};
 } else return 2;
 if (!result) { fprintf(stderr, "%s\\n", error.localizedDescription.UTF8String); return 1; }
 NSData *data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
 puts([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String); return 0;
} }
''')
        cls.binary = cls.root/'probe'
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-I'+str(INCLUDE), str(source),
                        '-framework', 'Foundation', '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixture(self, root, scoped=True):
        source, cache = root/'seed', root/'cache'
        source.mkdir(); cache.mkdir()
        files = {VARIANT+'.metal': b'// synthetic', VARIANT+'.json': b'{}',
                 KEY+'.variants': json.dumps([dict(variant=VARIANT[:-2], deps={})]).encode()}
        identity = dict(schema=1, chain=CHAIN, version='1.2.1', source_romfs_sha256='c'*64)
        if scoped:
            files['shader-keys.json'] = json.dumps(dict(**identity, keys=[KEY])).encode()
        for name, data in files.items():
            (source/name).write_bytes(data)
        (source/'seed-manifest.json').write_text(json.dumps(dict(**identity, shaders=1,
            files={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})))
        return source, cache

    def probe(self, *args, good=True):
        result = subprocess.run([str(self.binary), *map(str, args)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if good else 1, result.stderr)
        return json.loads(result.stdout) if good else result.stderr

    def scope(self, source, queries=(), chain=CHAIN):
        return self.probe('--scope', source, chain, json.dumps(queries))

    def observed(self, source, cache, queries=(), chain=CHAIN):
        return self.probe('--observed', source, cache, chain, json.dumps(queries))

    def test_actual_observations_are_deduplicated_and_survive_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            unseen = 'd'*64
            result = self.observed(source, cache, [KEY, unseen, unseen, 'bad', None])
            self.assertEqual(result, dict(added=[False, True, False, False, False], keys=[unseen]))
            self.assertEqual(self.observed(source, cache), dict(added=[], keys=[unseen]))
            lines = (cache/'observed'/('c'*64+'.keys')).read_text().splitlines()
            self.assertEqual(lines[1:], [unseen])
            self.assertEqual(json.loads(lines[0])['chain'], CHAIN)

    def test_observations_of_another_game_or_chain_are_not_reused(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            self.observed(source, cache, ['d'*64])
            path = cache/'observed'/('c'*64+'.keys')
            original = path.read_text().splitlines()
            for field, value in [('version', 'v0'), ('chain', 'obsolete'), ('source_romfs_sha256', 'e'*64)]:
                header = json.loads(original[0]); header[field] = value
                path.write_text(json.dumps(header)+'\n'+'d'*64+'\n')
                self.assertEqual(self.observed(source, cache)['keys'], [])
                self.assertEqual(self.observed(source, cache, ['e'*64])['keys'], ['e'*64])
                self.assertEqual(path.read_text().splitlines()[1:], ['e'*64])

    def test_corrupt_observation_lines_are_ignored_and_absent_scope_does_not_write(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            self.observed(source, cache, ['d'*64])
            path = cache/'observed'/('c'*64+'.keys')
            with path.open('a') as stream:
                stream.write('bad\n'+KEY+'\n'+'D'*64+'\n')
            self.assertEqual(self.observed(source, cache)['keys'], ['d'*64])
            (source/'shader-keys.json').unlink()
            before = path.read_bytes()
            self.assertEqual(self.observed(source, cache, ['e'*64]), dict(added=[False], keys=[]))
            self.assertEqual(path.read_bytes(), before)

    def test_unwritable_observation_store_keeps_the_live_key_without_repeated_attempts(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            (cache/'observed').write_text('not a directory')
            self.assertEqual(self.observed(source, cache, ['d'*64, 'd'*64]),
                             dict(added=[True, False], keys=['d'*64]))

    def mutate_scope(self, source, change, rehash=True):
        path = source/'shader-keys.json'
        scope = json.loads(path.read_text())
        change(scope)
        path.write_text(json.dumps(scope))
        if rehash:
            manifest = source/'seed-manifest.json'
            data = json.loads(manifest.read_text())
            data['files']['shader-keys.json'] = hashlib.sha256(path.read_bytes()).hexdigest()
            manifest.write_text(json.dumps(data))

    def test_verified_scope_accepts_raw_and_variant_keys_and_rejects_malformed_names(self):
        queries = [KEY, VARIANT, 'd'*64, 'd'*64+'-'+'b'*12+'_b', KEY[:-1], KEY.upper(),
                   KEY+'-'+'b'*12, VARIANT+'.metal', KEY+'-'+'B'*12+'_b',
                   KEY+'-'+'b'*11+'_b', KEY+'_'+'b'*12+'_b', None, 42, []]
        with tempfile.TemporaryDirectory() as folder:
            source, _ = self.fixture(Path(folder))
            self.assertEqual(self.scope(source, queries),
                             dict(loaded=True, count=1, answers=[True, True]+[False]*(len(queries)-2)))

    def test_wrong_chain_or_game_identity_falls_back_without_filtering(self):
        changes = [('chain', 'other-chain'), ('version', '1.0.0'), ('schema', 2),
                   ('source_romfs_sha256', 'd'*64)]
        for field, value in changes:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as folder:
                source, _ = self.fixture(Path(folder))
                self.mutate_scope(source, lambda scope: scope.__setitem__(field, value))
                self.assertEqual(self.scope(source, [KEY, 'd'*64]),
                                 dict(loaded=False, count=0, answers=[True, True]))
        with tempfile.TemporaryDirectory() as folder:
            source, _ = self.fixture(Path(folder))
            self.assertFalse(self.scope(source, chain='other-chain')['loaded'])

    def test_empty_or_malformed_keys_and_invalid_romfs_hash_fall_back(self):
        for keys in [[], 'a'*64, [KEY, 'bad'], [KEY.upper()], [42], [None], [KEY+'\n']]:
            with self.subTest(keys=keys), tempfile.TemporaryDirectory() as folder:
                source, _ = self.fixture(Path(folder))
                self.mutate_scope(source, lambda scope: scope.__setitem__('keys', keys))
                self.assertFalse(self.scope(source)['loaded'])
        with tempfile.TemporaryDirectory() as folder:
            source, _ = self.fixture(Path(folder))
            self.mutate_scope(source, lambda scope: scope.__setitem__('source_romfs_sha256', 'bad'))
            manifest = source/'seed-manifest.json'
            data = json.loads(manifest.read_text()); data['source_romfs_sha256'] = 'bad'
            manifest.write_text(json.dumps(data))
            self.assertFalse(self.scope(source)['loaded'])

    def test_malformed_manifest_identity_falls_back(self):
        for field, value in [('schema', None), ('chain', []), ('version', ''), ('version', 121),
                             ('source_romfs_sha256', 'C'*64), ('files', [])]:
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as folder:
                source, _ = self.fixture(Path(folder))
                manifest = source/'seed-manifest.json'
                data = json.loads(manifest.read_text()); data[field] = value
                manifest.write_text(json.dumps(data))
                self.assertFalse(self.scope(source)['loaded'])
        for target in ['seed-manifest.json', 'shader-keys.json']:
            for value in [[], None, 'not a document']:
                with self.subTest(target=target, value=value), tempfile.TemporaryDirectory() as folder:
                    source, _ = self.fixture(Path(folder))
                    (source/target).write_text(json.dumps(value))
                    self.assertFalse(self.scope(source)['loaded'])

    def test_integrity_failure_missing_files_and_older_seed_fall_back(self):
        for mutation in ['hash', 'missing_scope', 'missing_manifest', 'broken_json', 'unlisted', 'old_seed']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as folder:
                source, _ = self.fixture(Path(folder), scoped=mutation != 'old_seed')
                if mutation == 'hash':
                    self.mutate_scope(source, lambda scope: scope.__setitem__('keys', ['d'*64]), rehash=False)
                elif mutation == 'missing_scope':
                    (source/'shader-keys.json').unlink()
                elif mutation == 'missing_manifest':
                    (source/'seed-manifest.json').unlink()
                elif mutation == 'broken_json':
                    (source/'shader-keys.json').write_text('{')
                elif mutation == 'unlisted':
                    manifest = source/'seed-manifest.json'
                    data = json.loads(manifest.read_text()); del data['files']['shader-keys.json']
                    manifest.write_text(json.dumps(data))
                self.assertEqual(self.scope(source, ['d'*64]), dict(loaded=False, count=0, answers=[True]))

    def test_import_checks_the_scope_but_leaves_it_in_the_seed(self):
        # The runtime reads shader-keys.json from the seed folder (totk_shader_rt.m shader_scope), so the import
        # verifies it with the other files but never writes it into the cache; collected shaders are kept.
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            self.probe('--import', source, cache)
            self.assertFalse((cache/'shader-keys.json').exists())
            (cache/(VARIANT+'.metal')).write_bytes(b'// collected here')
            self.mutate_scope(source, lambda scope: scope.__setitem__('keys', [KEY, 'd'*64]))
            self.assertEqual(self.probe('--import', source, cache)['imported_files'], 0)
            self.assertFalse((cache/'shader-keys.json').exists())
            self.assertEqual((cache/(VARIANT+'.metal')).read_bytes(), b'// collected here')
            self.assertEqual(self.scope(source, ['d'*64])['loaded'], True)

    def test_a_damaged_scope_fails_the_import_like_any_seed_file(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache = self.fixture(Path(folder))
            (source/'shader-keys.json').write_text('{"schema": 1}')
            self.probe('--import', source, cache, good=False)

if __name__ == '__main__':
    unittest.main()

"""tools/builder/manifests.py: inspect_versions (S1.1a, C2). Errors are simulated by patching the module's own
_list_names and _read_bytes, so no test depends on file permissions."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.builder import manifests
from tools.builder.games import BOTW, TOTK
from tools.builder.manifests import inspect_versions

REPO = Path(__file__).resolve().parents[1]
GOOD = {'title_id': TOTK.title_id}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.folder = self.root / TOTK.assets_dir
        self.folder.mkdir(parents=True)

    def write(self, name, content):
        path = self.folder / name
        path.write_bytes(content if isinstance(content, bytes) else json.dumps(content).encode())
        return path

    def write_all_valid(self, versions=TOTK.versions):
        for v in versions:
            self.write(v.manifest, {**GOOD, 'display_version': v.display})

    def inspect(self, spec=TOTK):
        try:
            return inspect_versions(spec, self.root)
        except Exception as e:      # the contract: nothing on disk makes it raise
            self.fail(f'inspect_versions raised {e!r}')

    def causes(self, report):
        return [(p.path.name, p.field, p.cause) for p in report.problems]


class RealRepository(unittest.TestCase):
    def test_this_clone_builds_every_declared_version(self):
        for spec, versions in ((TOTK, ('1.0.0', '1.1.2', '1.2.1', '1.4.3')), (BOTW, ('1.6.0', '1.9.0'))):
            report = inspect_versions(spec, REPO)
            self.assertEqual(report.buildable, versions)
            self.assertEqual(report.problems, ())
            self.assertEqual(report.unrecognised, ())


class ReadStage(Base):
    def test_all_valid(self):
        self.write_all_valid()
        self.assertEqual(self.inspect().buildable, ('1.0.0', '1.1.2', '1.2.1', '1.4.3'))

    def one_bad(self, content):
        self.write_all_valid()
        self.write('expected-v1.1.2.json', content)
        report = self.inspect()
        self.assertEqual(report.buildable, ('1.0.0', '1.2.1', '1.4.3'))
        return self.causes(report)

    def test_missing(self):
        self.write_all_valid()
        (self.folder / 'expected-v1.1.2.json').unlink()
        self.assertEqual(self.causes(self.inspect()), [('expected-v1.1.2.json', None, 'missing')])

    def test_unreadable(self):
        self.write_all_valid()
        real = manifests._read_bytes
        def read(path):
            if path.name == 'expected-v1.1.2.json':
                raise PermissionError('denied')
            return real(path)
        with patch.object(manifests, '_read_bytes', side_effect=read):
            report = self.inspect()
        self.assertEqual(self.causes(report), [('expected-v1.1.2.json', None, 'unreadable')])
        self.assertIn('denied', report.problems[0].detail)

    def test_not_utf8_stops_the_read_stage(self):
        self.assertEqual(self.one_bad(b'\xff\xfe{'), [('expected-v1.1.2.json', None, 'not_utf8')])

    def test_invalid_json_with_position(self):
        self.write_all_valid()
        self.write('expected-v1.1.2.json', b'{"a":')
        problem = self.inspect().problems[0]
        self.assertEqual((problem.cause, problem.field), ('invalid_json', None))
        self.assertIn('line 1 column 6', problem.detail)

    def test_nesting_too_deep_for_the_json_parser_is_invalid_json(self):
        self.assertEqual(self.one_bad(b'[' * 200_000), [('expected-v1.1.2.json', None, 'invalid_json')])

    def test_integer_over_the_conversion_limit_is_invalid_json(self):   # ValueError, not JSONDecodeError
        content = b'{"other": ' + b'9' * 5000 + b'}'
        causes = self.one_bad(content)
        self.assertEqual(causes, [('expected-v1.1.2.json', None, 'invalid_json')])
        self.write('expected-v1.1.2.json', content)
        self.assertIn('integer string conversion', self.inspect().problems[0].detail)

    def test_not_object(self):
        self.assertEqual(self.one_bad([]), [('expected-v1.1.2.json', None, 'not_object')])


class ContentStage(Base):
    def one_bad(self, content):
        self.write_all_valid()
        self.write('expected-v1.1.2.json', content)
        report = self.inspect()
        self.assertNotIn('1.1.2', report.buildable)
        return [(f, c) for _, f, c in self.causes(report)]

    def test_each_field_cause_alone(self):
        cases = [
            ({'title_id': TOTK.title_id}, [('display_version', 'missing_field')]),
            ({**GOOD, 'display_version': 112}, [('display_version', 'wrong_type')]),
            ({**GOOD, 'display_version': '1.1.3'}, [('display_version', 'mismatch')]),
            ({'display_version': '1.1.2'}, [('title_id', 'missing_field')]),
            ({'display_version': '1.1.2', 'title_id': None}, [('title_id', 'wrong_type')]),
            ({'display_version': '1.1.2', 'title_id': BOTW.title_id}, [('title_id', 'mismatch')]),
        ]
        for content, expected in cases:
            with self.subTest(content=content):
                self.assertEqual(self.one_bad(content), expected)

    def test_every_content_defect_is_reported_in_order(self):
        self.assertEqual(self.one_bad({'display_version': 112}),
                         [('display_version', 'wrong_type'), ('title_id', 'missing_field')])

    def test_mismatch_says_what_was_expected_and_found(self):
        self.write_all_valid()
        self.write('expected-v1.1.2.json', {**GOOD, 'display_version': '1.1.3'})
        self.assertEqual(self.inspect().problems[0].detail, "expected '1.1.2', found '1.1.3'")


class Unrecognised(Base):
    def test_listed_never_read_and_never_affect_a_declared_version(self):
        self.write_all_valid()
        self.write('expected-v9.9.9.json', {**GOOD, 'display_version': '1.4.3'})
        self.write('expected-v8.0.0.json', b'{broken')
        self.write('shader_list.tsv', b'')
        real = manifests._read_bytes
        read = []
        with patch.object(manifests, '_read_bytes', side_effect=lambda p: read.append(p.name) or real(p)):
            report = self.inspect()
        self.assertEqual([p.name for p in report.unrecognised], ['expected-v8.0.0.json', 'expected-v9.9.9.json'])
        self.assertEqual(report.buildable, ('1.0.0', '1.1.2', '1.2.1', '1.4.3'))
        self.assertEqual(report.problems, ())
        self.assertEqual(sorted(read), sorted(v.manifest for v in TOTK.versions))


class Folder(Base):
    def test_missing_folder(self):
        self.folder.rmdir()
        report = self.inspect()
        self.assertEqual(report.buildable, ())
        self.assertEqual(self.causes(report)[0], ('assets', None, 'folder_missing'))
        self.assertEqual([c for _, _, c in self.causes(report)[1:]], ['missing'] * 4)

    def test_listing_error_still_checks_declared_manifests(self):
        self.write_all_valid()
        self.write('expected-v9.9.9.json', GOOD)
        with patch.object(manifests, '_list_names', side_effect=OSError('I/O error')):
            report = self.inspect()
        self.assertEqual(self.causes(report), [('assets', None, 'folder_unreadable')])
        self.assertEqual(report.unrecognised, ())
        self.assertEqual(report.buildable, ('1.0.0', '1.1.2', '1.2.1', '1.4.3'))


class Order(Base):
    def test_buildable_follows_the_declared_order(self):
        self.write_all_valid(tuple(reversed(TOTK.versions)))
        self.assertEqual(self.inspect().buildable, ('1.0.0', '1.1.2', '1.2.1', '1.4.3'))

    def test_problems_sorted_by_path_then_stage(self):
        self.write('expected-v1.4.3.json', {'display_version': 1})
        self.write('expected-v1.0.0.json', b'[')
        names = [(n, f) for n, f, _ in self.causes(self.inspect())]
        self.assertEqual(names, [('expected-v1.0.0.json', None), ('expected-v1.1.2.json', None),
                                 ('expected-v1.2.1.json', None), ('expected-v1.4.3.json', 'display_version'),
                                 ('expected-v1.4.3.json', 'title_id')])


if __name__ == '__main__':
    unittest.main()

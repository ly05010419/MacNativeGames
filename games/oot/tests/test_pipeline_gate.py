"""#2070: a compute pipeline the Metal compiler of this Mac cannot build stops the package with its shader named, and the
player reads it on the "ERROR:" line build_app.py prints (the install docs send them there), not in a Python traceback."""
import hashlib
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('oot_pipeline_gate', GAME / 'build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)


def index_id(row):
    return 'c' + hashlib.sha256(row[2:].encode()).hexdigest()[:40]


class MissingComputeTests(unittest.TestCase):
    ROWS = ['C\t' + 'a' * 64 + '-b7530f698e76_b', 'C\t' + 'b' * 64 + '-b7530f698e76_b']

    def test_every_planned_pipeline_in_the_index_is_fine(self):
        self.assertEqual(build.missing_compute(self.ROWS, {index_id(r) for r in self.ROWS}), [])

    def test_the_missing_ones_are_named_by_their_shader(self):
        missing = build.missing_compute(self.ROWS, {index_id(self.ROWS[0])})
        self.assertEqual(missing, ['b' * 64 + '-b7530f698e76_b'])

    def test_the_message_names_the_shader_the_gpu_and_the_log(self):
        text = build.missing_compute_message(['b' * 64 + '-b7530f698e76_b'], 'applegpu_g17g', Path('/w/logs/compute-pipelines.log'))
        for part in ('1 compute pipeline', 'b' * 64 + '-b7530f698e76_b', 'applegpu_g17g', '/w/logs/compute-pipelines.log'):
            self.assertIn(part, text)


class BuildErrorTests(unittest.TestCase):
    def test_build_app_turns_the_ports_errors_into_its_error_line(self):
        root = str(GAME.parents[1])
        if root not in sys.path:
            sys.path.insert(0, root)
        import build_app
        class Port:
            @staticmethod
            def build(args, source):
                raise ValueError('compute archive index omits planned pipelines')
        with mock.patch('importlib.util.module_from_spec', return_value=Port), \
             mock.patch('importlib.util.spec_from_file_location', return_value=mock.Mock(loader=mock.Mock())):
            with self.assertRaisesRegex(build_app.BuildError, 'compute archive index omits planned pipelines'):
                build_app.build_oot(mock.Mock(), Path('/game.nup'))


if __name__ == '__main__':
    unittest.main()

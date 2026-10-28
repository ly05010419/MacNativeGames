"""tools/builder/strings.py (S2.1, T1-T2): text() and the drift test. The ids are read from the source with ast, not
listed by hand, so a new check, message id or outcome without its text fails here, and so does a text left behind."""
import ast
from pathlib import Path
import unittest

from tools.builder.session import Outcome
from tools.builder.strings import EN, check_sentence, text

BUILDER = Path(__file__).resolve().parents[1] / 'tools' / 'builder'
CHECK_SOURCES = ('checks.py', 'checks_system.py', 'checks_inputs.py')
VIEW_SOURCES = ('cli.py', 'gui.py', 'gui_files.py', 'gui_checks.py', 'gui_build.py', 'strings.py', '__main__.py')
MESSAGE_ARG = {'CheckResult': 2, '_block': 1}       # the positional index of the message id in each call
PREFIXES = ('check.', 'msg.', 'outcome.', 'reason.')


def _literal(node):
    return node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None


def _check_ids(node):
    """`id = '<id>'` or `id, requires = '<id>', (...)` in a class body."""
    target, value = node.targets[0], node.value
    pairs = zip(target.elts, value.elts) if isinstance(target, ast.Tuple) and isinstance(value, ast.Tuple) \
        else [(target, value)]
    return {_literal(v) for t, v in pairs if isinstance(t, ast.Name) and t.id == 'id' and _literal(v)}


def emitted_ids(source):
    """(check ids, message ids) of one module: literal message ids of CheckResult(...) and _block(...), by position
    (MESSAGE_ARG) or as message_id=. Non-literal arguments (a helper passing its parameter on) are not ids."""
    check_ids, message_ids = set(), set()
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Assign):
            check_ids |= _check_ids(node)
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in MESSAGE_ARG:
            args = [kw.value for kw in node.keywords if kw.arg == 'message_id']
            if len(node.args) > MESSAGE_ARG[node.func.id]:
                args.append(node.args[MESSAGE_ARG[node.func.id]])
            message_ids |= {_literal(a) for a in args if _literal(a)}
    return check_ids, message_ids


def reason_ids(source):
    """The first item of each (reason, fires) pair in inputs.py's BLOCK_RULES (S3.4, f1)."""
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'BLOCK_RULES' for t in node.targets):
            return {_literal(pair.elts[0]) for pair in node.value.elts}
    return set()


def expected_keys(sources):
    keys = {f'outcome.{o.value}' for o in Outcome}
    keys |= {f'reason.{r}' for r in reason_ids((BUILDER / 'inputs.py').read_text(encoding='utf-8'))}
    for source in sources:
        check_ids, message_ids = emitted_ids(source)
        keys |= {f'check.{i}' for i in check_ids} | {f'msg.{m}' for m in message_ids}
    return keys


def real_sources():
    return [(BUILDER / name).read_text(encoding='utf-8') for name in CHECK_SOURCES]


class Drift(unittest.TestCase):
    def test_the_scan_finds_the_known_ids(self):
        checks, messages = set(), set()
        for source in real_sources():
            c, m = emitted_ids(source)
            checks, messages = checks | c, messages | m
        self.assertEqual((len(checks), len(messages)), (14, 26))
        self.assertLessEqual({'ok', 'skipped', 'check_crashed', 'command_failed', 'input_missing', 'disk_space'},
                             messages)

    def test_every_emitted_id_has_a_text(self):
        self.assertEqual(expected_keys(real_sources()) - set(EN), set())

    def test_no_text_is_left_for_an_id_no_longer_emitted(self):
        ours = {k for k in EN if k.startswith(PREFIXES)}
        self.assertEqual(ours - expected_keys(real_sources()), set())

    def test_a_new_id_is_reported(self):
        source = ("class New:\n    id, requires = 'new_check', ()\n\n    def run(self, ctx):\n"
                  "        if ctx:\n            return _block(self.id, 'brand_new', '')\n"
                  "        return CheckResult(self.id, Status.OK, message_id='by_keyword')\n")
        self.assertEqual(emitted_ids(source), ({'new_check'}, {'brand_new', 'by_keyword'}))
        self.assertEqual(expected_keys([source]) - set(EN), {'check.new_check', 'msg.brand_new', 'msg.by_keyword'})

    def test_a_parameter_passed_on_is_not_an_id(self):
        source = ("def _block(check_id, message_id, detail):\n"
                  "    return CheckResult(check_id, Status.BLOCK, message_id)\n")
        self.assertEqual(emitted_ids(source), (set(), set()))


def _called(node):
    func = node.func
    return func.id if isinstance(func, ast.Name) else func.attr if isinstance(func, ast.Attribute) else None


def literal_keys(source):
    """Every literal key passed to text(...) or a view's say(...) (S2.3); f-string keys such as
    f'gui.field.{field}' are not literals and are covered by their own tests."""
    return {_literal(node.args[0]) for node in ast.walk(ast.parse(source))
            if isinstance(node, ast.Call) and _called(node) in ('text', 'say') and node.args and _literal(node.args[0])}


class ViewKeys(unittest.TestCase):                                              # S2.3, revision 2 point 5
    def test_every_literal_key_the_views_use_exists(self):
        keys = set()
        for name in VIEW_SOURCES:
            keys |= literal_keys((BUILDER / name).read_text(encoding='utf-8'))
        self.assertGreater(len(keys), 50)
        self.assertEqual(keys - set(EN), set())

    def test_a_missing_key_is_reported(self):
        source = "text('gui.nope')\nself.say('cli.nope')\ntext(f'gui.field.{x}')\n"
        self.assertEqual(literal_keys(source) - set(EN), {'gui.nope', 'cli.nope'})


class Reasons(unittest.TestCase):                                               # S3.4, f1
    def test_the_scan_finds_every_block_reason(self):
        found = reason_ids((BUILDER / 'inputs.py').read_text(encoding='utf-8'))
        self.assertEqual(found, {'base_kind', 'update_kind', 'fonts_both', 'fonts_needed', 'botw_needs_update'})

    def test_input_rules_reads_as_sentences(self):
        self.assertEqual(check_sentence('input_rules', 'fonts_needed'),
                         'These inputs cannot be built: this game needs the system fonts (choose a firmware folder or '
                         'a shared-fonts folder).')
        self.assertNotIn('fonts_both', check_sentence('input_rules', 'fonts_both, update_kind'))
        self.assertEqual(check_sentence('disk_space', '3.0 GB free'), 'Not enough free space: 3.0 GB free.')


class Text(unittest.TestCase):
    def test_with_and_without_values(self):
        self.assertEqual(text('msg.disk_space', detail='3.0 GB free'), 'Not enough free space: 3.0 GB free.')
        self.assertEqual(text('cli.checks_ready'), 'Checks: ready.')

    def test_unknown_key(self):
        with self.assertRaises(KeyError):
            text('msg.no_such_id')


if __name__ == '__main__':
    unittest.main()

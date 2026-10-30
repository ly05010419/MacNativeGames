"""tools/builder/checks.py: run_checks and ready (S1.2a, K4), with fake checks that record whether they ran."""
import errno
import unittest

from tools.builder.checks import CheckContext, CheckResult, Status, ready, run_checks


class Fake:
    def __init__(self, id, status=Status.OK, requires=(), raises=None, returns=None):
        self.id, self.requires = id, tuple(requires)
        self.status, self.raises, self.returns = status, raises, returns
        self.ran = False

    def run(self, ctx):
        self.ran = True
        if self.raises:
            raise self.raises
        if self.returns is not None:
            return self.returns
        return CheckResult(self.id, self.status, 'fake')


CTX = CheckContext(runner=None)


def statuses(results):
    return [(r.id, r.status) for r in results]


class Order(unittest.TestCase):
    def test_results_follow_the_order_of_the_checks(self):
        checks = [Fake('c'), Fake('a', Status.WARN), Fake('b', Status.BLOCK)]
        self.assertEqual(statuses(run_checks(checks, CTX)),
                         [('c', Status.OK), ('a', Status.WARN), ('b', Status.BLOCK)])


class Skip(unittest.TestCase):
    def test_block_skips_dependants_transitively_and_independents_still_run(self):
        a, b, c, d = Fake('a', Status.BLOCK), Fake('b', requires=['a']), Fake('c', requires=['b']), Fake('d')
        results = run_checks([a, b, c, d], CTX)
        self.assertEqual(statuses(results), [('a', Status.BLOCK), ('b', Status.SKIP), ('c', Status.SKIP), ('d', Status.OK)])
        self.assertEqual([x.ran for x in (a, b, c, d)], [True, False, False, True])
        self.assertEqual([r.detail for r in results[1:3]], ['requires a', 'requires b'])

    def test_warn_does_not_skip(self):
        b = Fake('b', requires=['a'])
        self.assertEqual(statuses(run_checks([Fake('a', Status.WARN), b], CTX))[1], ('b', Status.OK))
        self.assertTrue(b.ran)


class Isolation(unittest.TestCase):
    def test_a_raising_check_becomes_block_and_the_next_one_runs(self):
        nxt = Fake('next')
        results = run_checks([Fake('boom', raises=OSError(errno.EMFILE, 'Too many open files')), nxt], CTX)
        self.assertEqual((results[0].status, results[0].message_id), (Status.BLOCK, 'check_crashed'))
        self.assertIn('Too many open files', results[0].detail)
        self.assertTrue(nxt.ran)


class Validation(unittest.TestCase):
    def assertRejected(self, checks, pattern):
        with self.assertRaisesRegex(ValueError, pattern):
            run_checks(checks, CTX)
        self.assertFalse(any(c.ran for c in checks), 'a check ran before validation failed')

    def test_duplicate_ids(self):
        self.assertRejected([Fake('a'), Fake('b'), Fake('a')], "share the id 'a'")

    def test_unknown_requirement(self):
        self.assertRejected([Fake('a'), Fake('b', requires=['nope'])], "'b' requires 'nope'")

    def test_requirement_later_in_the_list(self):
        self.assertRejected([Fake('b', requires=['a']), Fake('a')], "'b' requires 'a'")


class ResultIntegrity(unittest.TestCase):
    def test_a_result_with_another_id_is_blocked_under_the_registered_id(self):
        liar = Fake('a', returns=CheckResult('someone_else', Status.OK, 'fake'))
        dependant = Fake('b', requires=['a'])
        results = run_checks([liar, dependant], CTX)
        self.assertEqual(statuses(results), [('a', Status.BLOCK), ('b', Status.SKIP)])
        self.assertEqual((results[0].message_id, results[0].detail), ('check_crashed', "returned id 'someone_else'"))
        self.assertFalse(dependant.ran)
        self.assertNotIn('someone_else', [r.id for r in results])

    def test_a_result_of_another_type_is_blocked(self):
        results = run_checks([Fake('a', returns='not a result'), Fake('b')], CTX)
        self.assertEqual((results[0].status, results[0].detail), (Status.BLOCK, 'returned str'))

    def test_none_is_blocked(self):
        class ReturnsNone(Fake):
            def run(self, ctx):
                self.ran = True
        self.assertEqual(run_checks([ReturnsNone('a')], CTX)[0].detail, 'returned NoneType')


class Ready(unittest.TestCase):
    def test_ready(self):
        ok, warn, block, skip = (CheckResult('x', s, 'm') for s in (Status.OK, Status.WARN, Status.BLOCK, Status.SKIP))
        self.assertTrue(ready([ok, warn]))
        self.assertFalse(ready([ok, block, skip]))
        self.assertTrue(ready([]))


if __name__ == '__main__':
    unittest.main()

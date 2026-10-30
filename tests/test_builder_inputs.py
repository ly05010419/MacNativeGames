"""tools/builder/inputs.py: evaluate_inputs (S1.1a, C3). Rules one by one, their intersections, and invariants over
the whole input space."""
from dataclasses import replace
from itertools import product
import unittest

from tools.builder.games import BOTW, GAMES, TOTK
from tools.builder.inputs import (BLOCK_RULES, BaseKind, Fonts, InputChoice, Status, UpdateKind,
                                  evaluate_inputs)

RULE_ORDER = [reason for reason, _ in BLOCK_RULES]


def verdict(spec, base, update=UpdateKind.NONE, fonts=Fonts.NONE, cached=False):
    v = evaluate_inputs(spec, InputChoice(base, update, fonts, cached))
    return v.status, v.reasons, v.notes


OK = (Status.OK, (), ())


class EachRule(unittest.TestCase):
    def test_rule_order(self):
        self.assertEqual(RULE_ORDER, ['base_kind', 'update_kind', 'fonts_both', 'fonts_needed', 'botw_needs_update'])

    def test_base_kind(self):
        self.assertEqual(verdict(TOTK, BaseKind.XCI), OK)
        for base in (BaseKind.NSZ, BaseKind.XCZ, BaseKind.UNKNOWN):
            self.assertEqual(verdict(TOTK, base), (Status.BLOCK, ('base_kind',), ()))

    def test_update_kind(self):
        for update in (UpdateKind.NONE, UpdateKind.NSP, UpdateKind.NSZ):
            self.assertEqual(verdict(TOTK, BaseKind.XCI, update), OK)
        for update in (UpdateKind.XCI, UpdateKind.UNKNOWN):
            self.assertEqual(verdict(TOTK, BaseKind.XCI, update), (Status.BLOCK, ('update_kind',), ()))

    def test_fonts_both(self):
        self.assertEqual(verdict(TOTK, BaseKind.XCI, fonts=Fonts.SHARED), OK)
        self.assertEqual(verdict(TOTK, BaseKind.XCI, fonts=Fonts.BOTH), (Status.BLOCK, ('fonts_both',), ()))

    def test_fonts_needed_totk_only_for_nsp(self):
        self.assertEqual(verdict(TOTK, BaseKind.XCI), OK)
        self.assertEqual(verdict(TOTK, BaseKind.NSP), (Status.BLOCK, ('fonts_needed',), ()))
        self.assertEqual(verdict(TOTK, BaseKind.NSP, fonts=Fonts.FIRMWARE), OK)
        self.assertEqual(verdict(TOTK, BaseKind.NSP, cached=True), (Status.OK, (), ('fonts_cached',)))

    def test_fonts_needed_botw_for_any_base(self):
        self.assertEqual(verdict(BOTW, BaseKind.XCI), (Status.BLOCK, ('fonts_needed',), ()))
        self.assertEqual(verdict(BOTW, BaseKind.XCI, fonts=Fonts.SHARED), OK)
        self.assertEqual(verdict(BOTW, BaseKind.XCI, cached=True), (Status.OK, (), ('fonts_cached',)))

    def test_botw_needs_update(self):
        self.assertEqual(verdict(BOTW, BaseKind.NSP, UpdateKind.NSZ, Fonts.SHARED), OK)
        self.assertEqual(verdict(BOTW, BaseKind.NSP, fonts=Fonts.SHARED), (Status.BLOCK, ('botw_needs_update',), ()))
        self.assertEqual(verdict(BOTW, BaseKind.XCI, fonts=Fonts.SHARED), OK)
        self.assertEqual(verdict(TOTK, BaseKind.NSP, fonts=Fonts.SHARED), OK)


class Intersections(unittest.TestCase):
    def test_cache_does_not_lift_botw_needs_update(self):          # revision 2: B3 against B5
        self.assertEqual(verdict(BOTW, BaseKind.NSP, cached=True), (Status.BLOCK, ('botw_needs_update',), ()))

    def test_any_update_does_not_allow_an_xci_update(self):         # revision 2: T3 against R5
        self.assertEqual(verdict(TOTK, BaseKind.XCI, UpdateKind.XCI, Fonts.SHARED), (Status.BLOCK, ('update_kind',), ()))

    def test_three_blocks_accumulate_in_order(self):
        self.assertEqual(verdict(TOTK, BaseKind.NSZ, UpdateKind.UNKNOWN, Fonts.BOTH),
                         (Status.BLOCK, ('base_kind', 'update_kind', 'fonts_both'), ()))

    def test_botw_nsp_without_update_or_fonts(self):
        self.assertEqual(verdict(BOTW, BaseKind.NSP), (Status.BLOCK, ('fonts_needed', 'botw_needs_update'), ()))

    def test_both_fonts_with_cache_blocks_without_note(self):
        self.assertEqual(verdict(TOTK, BaseKind.NSP, fonts=Fonts.BOTH, cached=True), (Status.BLOCK, ('fonts_both',), ()))

    def test_fonts_needed_only_for_nsp_even_when_base_is_bad(self):
        self.assertEqual(verdict(TOTK, BaseKind.NSZ), (Status.BLOCK, ('base_kind',), ()))


class Scenarios(unittest.TestCase):
    """The examples in the mini plan's "Input scenarios" table."""
    def test_table(self):
        rows = [
            ('T1', TOTK, BaseKind.XCI, UpdateKind.NONE, Fonts.NONE, False, OK),
            ('T2', TOTK, BaseKind.XCI, UpdateKind.NSP, Fonts.NONE, False, OK),
            ('T5', TOTK, BaseKind.NSP, UpdateKind.NSZ, Fonts.NONE, True, (Status.OK, (), ('fonts_cached',))),
            ('T6', TOTK, BaseKind.NSP, UpdateKind.NONE, Fonts.NONE, False, (Status.BLOCK, ('fonts_needed',), ())),
            ('B1', BOTW, BaseKind.XCI, UpdateKind.NONE, Fonts.SHARED, False, OK),
            ('B2', BOTW, BaseKind.NSP, UpdateKind.NSZ, Fonts.FIRMWARE, False, OK),
            ('B4', BOTW, BaseKind.XCI, UpdateKind.NONE, Fonts.NONE, False, (Status.BLOCK, ('fonts_needed',), ())),
            ('B5', BOTW, BaseKind.NSP, UpdateKind.NONE, Fonts.SHARED, False, (Status.BLOCK, ('botw_needs_update',), ())),
        ]
        for case, spec, base, update, fonts, cached, expected in rows:
            with self.subTest(case=case):
                self.assertEqual(verdict(spec, base, update, fonts, cached), expected)


ALL = [(spec, InputChoice(b, u, f, c)) for spec, b, u, f, c in
       product(GAMES.values(), BaseKind, UpdateKind, Fonts, (False, True))]


def faults(choice):
    """Every choice that is exactly one fault worse than choice (the mini plan's definition)."""
    if choice.base_kind in (BaseKind.XCI, BaseKind.NSP):
        yield from (replace(choice, base_kind=b) for b in (BaseKind.NSZ, BaseKind.XCZ, BaseKind.UNKNOWN))
    if choice.update_kind in (UpdateKind.NSP, UpdateKind.NSZ):
        yield from (replace(choice, update_kind=u) for u in (UpdateKind.XCI, UpdateKind.UNKNOWN))
    if choice.fonts is not Fonts.BOTH:
        yield replace(choice, fonts=Fonts.BOTH)
    if choice.fonts in (Fonts.FIRMWARE, Fonts.SHARED):
        yield replace(choice, fonts=Fonts.NONE)
    if choice.fonts_cached:
        yield replace(choice, fonts_cached=False)


class Invariants(unittest.TestCase):
    def test_space_is_complete(self):
        self.assertEqual(len(ALL), 400)

    def test_block_if_and_only_if_reasons(self):
        for spec, choice in ALL:
            v = evaluate_inputs(spec, choice)
            self.assertEqual(v.status is Status.BLOCK, bool(v.reasons), (spec.id, choice))

    def test_reasons_in_rule_order_without_repeats(self):
        for spec, choice in ALL:
            reasons = list(evaluate_inputs(spec, choice).reasons)
            self.assertEqual(reasons, sorted(set(reasons), key=RULE_ORDER.index), (spec.id, choice))

    def test_no_notes_when_blocked(self):
        for spec, choice in ALL:
            v = evaluate_inputs(spec, choice)
            if v.status is Status.BLOCK:
                self.assertEqual(v.notes, (), (spec.id, choice))

    def test_one_more_fault_never_turns_block_into_ok(self):
        checked = 0
        for spec, choice in ALL:
            if evaluate_inputs(spec, choice).status is Status.BLOCK:
                for worse in faults(choice):
                    self.assertIs(evaluate_inputs(spec, worse).status, Status.BLOCK, (spec.id, choice, worse))
                    checked += 1
        self.assertGreater(checked, 0)


if __name__ == '__main__':
    unittest.main()

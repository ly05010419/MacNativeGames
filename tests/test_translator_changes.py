"""Every change of a shader translator answers why it changed and whether that was necessary (user 2026-11-18: "以后
改变翻译器 必须过测试 回答一个重要问题 为什么 翻译器改变 有没有必要 这种事情不允许在发生！").

A translator that changes makes every player translate every shader again on the next update (Ocarina of Time: 42,848
shaders, about 14 minutes on an M4), and its device caches start over. 2026-11-18 showed it happening seven times in two
days for nothing: the Launcher's sources carried seven files of one unchanged translator (a new Developer ID signature
each time), and the games named the translator by its file. Now the translator is its code (tools/translator_code.py,
any signature the same), and tools/translator_changes.json records each translator's code and, for every change, why
(what it fixes or adds), necessary (why nothing short of a new translator does it) and evidence (commit, test, data).

When this fails because a translator's code changed: do not just copy the new hash in. Answer the two questions first;
if the change is not necessary (a rebuild with another SDK, a stray edit, a re-signing tool that rewrote code), put the
recorded program back instead. Then add an entry to that translator's history and set its code_sha256."""
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import translator_code  # noqa: E402

LEDGER = ROOT / 'tools/translator_changes.json'
ANSWERS = ('why', 'necessary', 'evidence')


class TranslatorChanges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ledger = json.loads(LEDGER.read_text())
        cls.files = translator_code.translator_files(ROOT)

    def test_every_translator_is_recorded(self):
        self.assertTrue(self.files)
        missing = [f for f in self.files if f not in self.ledger['translators']]
        self.assertEqual(missing, [], 'a new translator: add it to tools/translator_changes.json with why it is needed '
                                      '(a game of its own? could it use an existing one?)')
        gone = [f for f in self.ledger['translators'] if f not in self.files]
        self.assertEqual(gone, [], 'a recorded translator is gone: remove its entry, saying why in the commit')

    def test_every_translator_is_the_recorded_code(self):
        changed = []
        for rel in self.files:
            entry = self.ledger['translators'].get(rel)
            if entry and translator_code.code_sha256(ROOT / rel) != entry['code_sha256']:
                changed.append(rel)
        self.assertEqual(changed, [], '\n'.join([
            '翻译器的代码变了。先回答：为什么变？有没有必要？没必要就换回记录的那一份。',
            'A translator\'s code changed. Answer first: why did it change, and was it necessary? If not, put the recorded '
            'program back. If it was, add {code_sha256, date, commit, why, necessary, evidence} to its history in '
            'tools/translator_changes.json and set its code_sha256. A new signature alone is not a change (same code).',
            *[f'  {rel}: now {translator_code.code_sha256(ROOT / rel)[:12]}' for rel in changed]]))

    def test_every_change_is_answered(self):
        for rel, entry in self.ledger['translators'].items():
            with self.subTest(translator=rel):
                history = entry.get('history') or []
                self.assertTrue(history, 'no history')
                self.assertEqual(history[-1]['code_sha256'], entry['code_sha256'], 'the current code has no entry of its own')
                for change in history:
                    for answer in ANSWERS:
                        self.assertTrue(str(change.get(answer, '')).strip(), f'{answer} is empty')
                    self.assertRegex(change['code_sha256'], r'^[0-9a-f]{64}$')

    def test_a_new_signature_is_not_a_change(self):
        """The hash that decides is the code's: tools/translator_code.py is the same function the games and the runtime
        use (tests/test_translator_code_identity.py signs one program two ways)."""
        import importlib.util
        names = ('prepare_shader_seed', 'ns2_shader_corpus', 'shader_list')
        other = {n: sys.modules.pop(n) for n in names if n in sys.modules}   # another game's, from another test module
        sys.path.insert(0, str(ROOT / 'games/oot/scripts'))
        try:
            spec = importlib.util.spec_from_file_location('oot_seed_for_ledger', ROOT / 'games/oot/scripts/prepare_ns2_seed.py')
            seed = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(seed)
        finally:
            sys.path.remove(str(ROOT / 'games/oot/scripts'))
            for n in names:
                sys.modules.pop(n, None)
            sys.modules.update(other)
        oot = ROOT / 'games/oot/prebuilt/macos-arm64/totk_xlate'
        self.assertEqual(translator_code.code_sha256(oot), seed.code_sha256(oot))


if __name__ == '__main__':
    unittest.main()

"""Verify checked TMNT gate output from original ExeFS without executing it."""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exefs', required=True, type=Path)
parser.add_argument('--rewritten', required=True, type=Path)
args = parser.parse_args()
game = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(game / 'scripts'))
sys.path.insert(0, str(game / 'third_party/d2r'))
from module_metadata import verify_rewrite_report
from nsoimg import Patched, direct_target, load
from tmnt_graphics_rules import SITE_WORDS, preflight

exefs, rewritten = args.exefs.resolve(), args.rewritten.resolve()
verify_rewrite_report(exefs, rewritten)
raw = (exefs / 'main').read_bytes()
assert preflight(raw, load(exefs / 'main', bss=False), 'main', True)
image = Patched(rewritten, 'main')
rules = (rewritten / 'main.rules.tsv').read_text().splitlines()
assert rules[0] == '# RULESET\ttmnt-graphics-phase-v1'
assert rules[1] == '# BUILD_ID\t' + raw[0x40:0x50].hex()
assert rules[2] == '# ORIGINAL_SHA256\t' + hashlib.sha256(raw).hexdigest()
assert rules[3] == '# IMAGE_SHA256\t' + hashlib.sha256((rewritten / 'main.patched.bin').read_bytes()).hexdigest()
assert rules[4:] == [f'{site:#x}\t{word:08x}' for site, word in SITE_WORDS]

expected = {
    0xfd34c: (0x90, 0x95), 0xfd28c: (0x96, 0x91),
    0xfdf70: (0x92,), 0xfdf74: (0x93,),
    0x8a8920: (0x98,), 0x8a9034: (0x97,),
    0x8a9060: (0x99,),
}
found = {site: [] for site in expected}
for line in (rewritten / 'main.gates.tsv').read_text().splitlines():
    ret, kind, arg, source = line.split()
    source = int(source, 16)
    if source in found:
        ret, arg = int(ret, 16), int(arg)
        assert kind == 'svc' and image.in_patch(ret) and ret % 4 == 0
        assert struct.unpack('<I', image[ret - 32:ret - 28])[0] == 0xa9bf7bfd
        assert struct.unpack('<I', image[ret - 4:ret])[0] == 0xd63f03c0
        assert struct.unpack('<I', image[ret:ret + 4])[0] == 0xa8c17bfd
        found[source].append((ret, arg))

for site, word in SITE_WORDS:
    records = found[site]
    assert tuple(arg for _, arg in records) == expected[site], (site, records)
    assert len({ret for ret, _ in records}) == len(records)
    rewritten_word = struct.unpack('<I', image[site:site + 4])[0]
    assert direct_target(rewritten_word, site) == records[0][0] - 32
    first = records[0][0]
    if site in (0xfd34c, 0xfd28c, 0xfdf70, 0x8a9034, 0x8a9060):
        assert struct.unpack('<I', image[first + 4:first + 8])[0] == word
    elif site == 0x8a8920:
        branch = struct.unpack('<I', image[first + 4:first + 8])[0]
        assert direct_target(branch, first + 4) == site + 4
    elif site == 0xfdf74:
        assert direct_target(word, site) == 0xfdff8
        branch_at = first + 4
        cbz, error, success = struct.unpack('<III', image[branch_at:branch_at + 12])
        assert cbz == 0x34000049
        assert direct_target(error, branch_at + 4) == 0xfdf78
        assert direct_target(success, branch_at + 8) == 0xfdff8

print('validated TMNT original-input graphics rules:', sum(map(len, found.values())),
      'gates; image SHA-256:', hashlib.sha256((rewritten / 'main.patched.bin').read_bytes()).hexdigest(),
      '; guest code executed: no')

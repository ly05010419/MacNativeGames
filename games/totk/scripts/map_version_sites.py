#!/usr/bin/env python3
"""Find where runtime/game_version.h's main offsets moved to in another game version.

Each code site is matched by the instructions around it in a known version (by default 1.1.2, then 1.0.0):
PC-relative fields (B/BL, B.cond, CBZ/TBZ, ADR/ADRP, LDR literal) and the page offsets that follow an ADRP
are masked; the window grows until exactly one place in the new main matches. Data sites are found through
code that addresses them (ADRP + LDR/ADD); the GOT slot or PLT stub of an imported function through its
JUMP_SLOT relocation (by symbol name); a function entry whose surroundings changed through its direct callers
(every caller that maps must branch to the same place).
The original instruction words at every code site are printed for both versions: runtime patches still
check them before writing, so a moved site with a different word needs a new expected value, not just an offset.

usage: map_version_sites.py --to v121 [--root <tree with extracted/<version>/exefs/main>] [--from v112] [--header]
       map_version_sites.py --to v121 --code 0x80b3a0,...   (other code offsets of the --from version, e.g. test windows)
Self-check first: --from v0 --to v112 must reproduce the 1.1.2 block (2026-10-19: 32 of 32). Exit status 1 when a site
is not found or disagrees with the target version's block if it already has one. Reads only local images.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/d2r'))
import nsoimg  # noqa: E402

WINDOWS = (6, 12, 24, 48, 96)       # words on each side of the site
FUZZY_MISSES = 3                    # a near match may differ in this many words (reported, never chosen)


def header_blocks(path):
    """{version: {'addr': {v0: offset}, 'name': value}} for every block of game_version.h (v0 = the #else block)."""
    blocks, current = {}, None
    for line in path.read_text().splitlines():
        m = re.match(r'#(?:el)?if defined\(TOTK_GAME_(V\d+)\)', line)
        if m: current = blocks.setdefault(m[1].lower(), {'addr': {}}); continue
        if line.startswith('#else'): current = blocks.setdefault('v0', {'addr': {}}); continue
        if line.startswith('#endif'): current = None; continue
        if current is None: continue
        m = re.match(r'#define GAME_ADDR_(0x[0-9a-f]+) (0x[0-9a-f]+)', line)
        if m: current['addr'][int(m[1], 16)] = int(m[2], 16); continue
        m = re.match(r'#define (GAME_\w+) (\S+)', line)
        if m: current[m[1]] = m[2]
    return blocks


def segments(path):
    d = path.read_bytes()[:0x100]
    return [struct.unpack_from('<III', d, 0x10 + i * 0x10)[1:] for i in range(3)]   # (memory offset, size)


def mask_of(word, adrp_regs):
    """(mask, base register written by an ADRP or None) for one instruction word."""
    if word & 0x7c000000 == 0x14000000: return 0xfc000000, None                 # B, BL
    if word & 0xff000010 == 0x54000000: return 0xff00001f, None                 # B.cond
    if word & 0x7e000000 == 0x34000000: return 0xff00001f, None                 # CBZ, CBNZ
    if word & 0x7e000000 == 0x36000000: return 0xfff8001f, None                 # TBZ, TBNZ
    if word & 0x1f000000 == 0x10000000: return 0x9f00001f, word & 31            # ADR, ADRP
    if word & 0x3b000000 == 0x18000000: return 0xff00001f, None                 # LDR literal
    base = (word >> 5) & 31
    if base in adrp_regs:
        if word & 0x7f800000 == 0x11000000: return 0xffc003ff, None             # ADD immediate (page offset)
        if word & 0x3b000000 == 0x39000000: return 0xffc003ff, None             # LDR/STR unsigned offset
    return 0xffffffff, None


def pattern(image, start, count):
    words = struct.unpack_from(f'<{count}I', image, start)
    out, adrp_regs = [], set()
    for word in words:
        mask, written = mask_of(word, adrp_regs)
        if written is not None: adrp_regs.add(written)
        elif mask == 0xffffffff and word & 0x1f != 0x1f:
            adrp_regs.discard(word & 31)    # most writes to Rd end an ADRP's page register (conservative)
        out.append((word & mask, mask))
    return out


class Text:
    def __init__(self, image, start, size):
        self.image, self.start, self.end = image, start, start + size
        self.words = memoryview(image)[start:self.end].cast('I')
        self.counts = {}

    def count(self, value):
        if value not in self.counts: self.counts[value] = self.image.count(struct.pack('<I', value), self.start, self.end)
        return self.counts[value]

    def matches(self, pat, limit=4):
        """Offsets in the text where every masked word of pat matches (at most limit of them)."""
        anchors = [i for i, (_, m) in enumerate(pat) if m == 0xffffffff]
        if not anchors: return []
        anchor = min(anchors, key=lambda i: self.count(pat[i][0]))
        needle, found, at = struct.pack('<I', pat[anchor][0]), [], self.start
        while len(found) <= limit:
            at = self.image.find(needle, at, self.end)
            if at < 0: break
            if (at - self.start) % 4 == 0:
                base = (at - self.start) // 4 - anchor
                if base >= 0 and base + len(pat) <= len(self.words) and \
                        all(self.words[base + i] & m == v for i, (v, m) in enumerate(pat)):
                    found.append(self.start + 4 * base)
            at += 1
        return found

    def near(self, pat, misses):
        """Places that match all but at most misses words (anchored on each fully masked word in turn)."""
        scores = {}
        for i, (v, m) in enumerate(pat):
            if m != 0xffffffff: continue
            needle, at = struct.pack('<I', v), self.start
            for _ in range(200):
                at = self.image.find(needle, at, self.end)
                if at < 0: break
                if (at - self.start) % 4 == 0:
                    base = (at - self.start) // 4 - i
                    if 0 <= base and base + len(pat) <= len(self.words) and base not in scores:
                        scores[base] = sum(self.words[base + j] & mm != vv for j, (vv, mm) in enumerate(pat))
                at += 1
        return sorted((s, self.start + 4 * b) for b, s in scores.items() if s <= misses)


def map_code(site, sources, target):
    """(new offset, how, source version) for a code site; sources = [(version, image, offset in it)]."""
    for half in WINDOWS:
        for version, image, offset in sources:
            pat = pattern(image, offset - 4 * half, 2 * half + 1)
            found = target.matches(pat)
            if len(found) == 1: return found[0] + 4 * half, f'unique match of +-{half} words', version
            if not found: break     # a wider window will not match either
    near = []
    for version, image, offset in sources:
        for misses, at in target.near(pattern(image, offset - 4 * 24, 49), FUZZY_MISSES):
            near.append(f'{version}: {misses} words differ at 0x{at + 4 * 24:x}')
    return None, 'no unique match' + (' (near: ' + '; '.join(near[:4]) + ')' if near else ''), None


def adrp_target(image, at):
    """The address an ADRP at `at` plus the next LDR/ADD on its register (within 8 words) refers to, or None."""
    a = struct.unpack_from('<I', image, at)[0]
    if a & 0x9f000000 != 0x90000000: return None
    reg = a & 31
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    page = (at & ~4095) + (((imm ^ (1 << 20)) - (1 << 20)) << 12)
    for k in range(1, 9):
        w = struct.unpack_from('<I', image, at + 4 * k)[0]
        if (w >> 5) & 31 != reg: continue
        if w & 0x7f800000 == 0x11000000: return page + ((w >> 10) & 0xfff)           # ADD (64-bit and 32-bit)
        if w & 0x3b000000 == 0x39000000: return page + ((w >> 10) & 0xfff) * (1 << (w >> 30))
        return None
    return None


def data_refs(image, text, address):
    """Text offsets of ADRP instructions that address `address`."""
    refs, page = [], address & ~4095
    words = memoryview(image)[text.start:text.end].cast('I')
    for i, a in enumerate(words):
        if a & 0x9f000000 != 0x90000000: continue
        at = text.start + 4 * i
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        if (at & ~4095) + (((imm ^ (1 << 20)) - (1 << 20)) << 12) == page and adrp_target(image, at) == address:
            refs.append(at)
    return refs


def map_data(site_from, image_from, text_from, target, image_to):
    """Map a data address through the code that addresses it: every code reference that maps must agree."""
    votes, mapped = {}, 0
    for ref in data_refs(image_from, text_from, site_from)[:40]:
        new, _, _ = map_code(ref, [('from', image_from, ref)], target)
        if new is None: continue
        value = adrp_target(image_to, new)
        if value is None: continue
        mapped += 1
        votes[value] = votes.get(value, 0) + 1
    if len(votes) == 1: return next(iter(votes)), f'{mapped} code references agree'
    return None, f'code references disagree or none mapped: {votes}'


def map_by_callers(site_from, image_from, text_from, target, image_to):
    """Map a function entry through its direct callers (BL / B): every caller that maps must branch to one place."""
    votes, mapped = {}, 0
    words = memoryview(image_from)[text_from.start:text_from.end].cast('I')
    callers = [text_from.start + 4 * i for i, w in enumerate(words)
               if w & 0x7c000000 == 0x14000000 and nsoimg.direct_target(w, text_from.start + 4 * i) == site_from]
    for call in callers[:40]:
        new, _, _ = map_code(call, [('from', image_from, call)], target)
        if new is None: continue
        word = struct.unpack_from('<I', image_to, new)[0]
        dest = nsoimg.direct_target(word, new) if word & 0x7c000000 == 0x14000000 else None
        if dest is None: continue
        mapped += 1
        votes[dest] = votes.get(dest, 0) + 1
    if len(votes) == 1: return next(iter(votes)), f'{mapped} of {len(callers)} callers agree'
    return None, f'callers: {len(callers)}, targets {dict((hex(k), v) for k, v in votes.items())}'


def import_slots(image):
    """{GOT slot: imported symbol name} from the JUMP_SLOT relocations."""
    return {where: name for where, kind, name, _, _ in nsoimg.relocs(image) if kind == 1026 and name}


def stub_slot(image, at):
    """The GOT slot a PLT stub (adrp x16 / ldr x17, [x16, #o] / add x16 / br x17) at `at` jumps through, or None."""
    a, b, _, d = struct.unpack_from('<IIII', image, at)
    if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220: return None
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    return (at & ~4095) + (((imm ^ (1 << 20)) - (1 << 20)) << 12) + ((b >> 10) & 0xfff) * 8


def plt_stubs(image, text, slot):
    return [at for at in range(text.end - 0x20000, text.end - 12, 4) if stub_slot(image, at) == slot]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--to', required=True, help='version folder to map into, e.g. v121')
    ap.add_argument('--from', dest='source', default='v112', help='known version whose instructions are searched for')
    ap.add_argument('--root', type=Path, default=ROOT, help='tree with extracted/<version>/exefs/main')
    ap.add_argument('--header', action='store_true', help='print #define lines for the new block')
    ap.add_argument('--code', default='', help='only map these comma-separated code offsets of the --from version')
    args = ap.parse_args()
    if args.code: return map_extra(args, [int(x, 16) for x in args.code.split(',')])
    blocks = header_blocks(ROOT / 'runtime/game_version.h')
    sites = list(blocks['v0']['addr'])
    known = blocks[args.source]['addr'] if args.source != 'v0' else {k: k for k in sites}
    expected = blocks.get(args.to, {}).get('addr') if args.to != 'v0' else {k: k for k in sites}
    images, texts = {}, {}
    for version in sorted({'v0', args.source, args.to}):
        path = args.root / f'extracted/{version}/exefs/main'
        images[version] = nsoimg.load(path)
        start, size = segments(path)[0]
        texts[version] = Text(images[version], start, size)
    image_from, text_from = images[args.source], texts[args.source]
    target, image_to = texts[args.to], images[args.to]
    imports_from = import_slots(image_from)
    slots_to = {name: where for where, name in import_slots(image_to).items()}
    results, failed, disagree = {}, 0, 0
    for site in sites:
        source_site = known.get(site)
        if source_site is None:
            print(f'0x{site:x}: no {args.source} address in game_version.h'); failed += 1; results[site] = None; continue
        how, words = '', ''
        stub = stub_slot(image_from, source_site) if text_from.start <= source_site < text_from.end else None
        if source_site in imports_from:                              # a GOT slot of an imported function
            name = imports_from[source_site]
            new, how = slots_to.get(name), f'GOT slot of {name}'
        elif stub in imports_from:                                   # a PLT stub of an imported function
            name = imports_from[stub]
            found = plt_stubs(image_to, target, slots_to.get(name, -1))
            new = found[0] if len(found) == 1 else None
            how = f'PLT stub of {name}' + ('' if new is not None else f' (stubs {found})')
        elif text_from.start <= source_site < text_from.end:         # code
            sources = [(args.source, image_from, source_site)]
            if args.source != 'v0': sources.append(('v0', images['v0'], site))
            new, how, used = map_code(site, sources, target)
            if new is None:
                new, via = map_by_callers(source_site, image_from, text_from, target, image_to)
                how += f'; by callers: {via}'
                used = args.source
            if new is not None:
                a = struct.unpack_from('<I', image_from, source_site)[0]
                b = struct.unpack_from('<I', image_to, new)[0]
                how += f' from {used}'
                words = f'  word {a:08x} -> {b:08x}' + ('' if a == b else '  (DIFFERENT)')
        else:                                                        # data addressed by code
            new, how = map_data(source_site, image_from, text_from, target, image_to)
        results[site] = new
        failed += new is None
        note = '  <-- NOT FOUND' if new is None else ''
        if new is not None and expected and site in expected and expected[site] != new:
            note = f'  <-- DISAGREES with game_version.h (0x{expected[site]:x})'; disagree += 1
        elif new is not None and expected and site in expected:
            note = '  (agrees with game_version.h)'
        new_text = f'0x{new:x}' if new is not None else '-'
        print(f'0x{site:x} ({args.source} 0x{source_site:x}) -> {new_text}: {how}{words}{note}')
    if args.header:
        for site in sites:
            if results[site] is not None: print(f'#define GAME_ADDR_0x{site:x} 0x{results[site]:x}')
    print(f'{len(sites) - failed} of {len(sites)} sites mapped' + (f', {failed} not found' if failed else '')
          + (f', {disagree} disagree with game_version.h' if disagree else ''))
    return 1 if failed or disagree else 0


def map_extra(args, offsets):
    """Map arbitrary code offsets of the --from version (e.g. the atomic windows the rewrite tests run)."""
    images, texts = {}, {}
    for version in sorted({args.source, args.to}):
        path = args.root / f'extracted/{version}/exefs/main'
        images[version] = nsoimg.load(path)
        start, size = segments(path)[0]
        texts[version] = Text(images[version], start, size)
    failed = 0
    for offset in offsets:
        new, how, _ = map_code(offset, [(args.source, images[args.source], offset)], texts[args.to])
        a = struct.unpack_from('<4I', images[args.source], offset)
        b = struct.unpack_from('<4I', images[args.to], new) if new is not None else ()
        failed += new is None
        print(f'0x{offset:x} -> ' + (f'0x{new:x}' if new is not None else '-') + f': {how}  words '
              + ' '.join(f'{w:08x}' for w in a) + ' -> ' + ' '.join(f'{w:08x}' for w in b))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

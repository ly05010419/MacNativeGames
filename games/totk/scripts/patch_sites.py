#!/usr/bin/env python3
"""The main-module instruction sites that the runtime (or the build-time rewriter) patches itself.

A mod patch merged at build time (scripts/mod_exefs.py) must not touch any of them: this runtime writes its own
patches over the shipping instructions and checks each site's original word first, so a mod that lands on one of
them makes the game refuse to start ("does not match the selected game version"), and a mod that lands on one of
the rewriter's registered cuts makes the rewrite itself stop. Both cases are caught here instead, before the build
rewrites anything, with the site named.

Where the list comes from (no offset is written down twice in this repository):

  games/totk/runtime/game_version.h     GAME_ADDR_<logical 1.0.0 address> -> the address in each version
  games/totk/runtime/*.c                every GAME_ADDR(...) use (main, sdk and subsdk0 runtimes)
  games/totk/runtime/game_patches.c     the literal tables of the versions 1.0.0 / 1.1.2 / 1.2.1
                                        (cloth/hair clock, ultrawide stub sites), per #if block
  games/totk/runtime/game_patches_v143.c the literal Patch tables of 1.4.3
  games/totk/runtime/game_patches_botw.c the rows {offset, original word, ...} of Breath of the Wild's check, put and
                                        job-queue tables and its multi-word sequences (the branch that GAME_BOTW190 or
                                        GAME_BOTW160 compiles)
  third_party/d2r/patch_nso.py          CUTS, OBS, PARAMS and SIGNED_TABLES (the signed jump tables) for the
                                        build ID being rewritten

usage: patch_sites.py --version v121 [--build-id <32 or 40 hex digits>] [--json]
"""
import argparse
import json
import re
import sys
from collections import namedtuple
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # games/totk
RUNTIME = ROOT / 'runtime'
REWRITER = ROOT / 'third_party/d2r/patch_nso.py'

VERSIONS = ('v0', 'v112', 'v121', 'v143', 'botw190', 'botw160')
# The #if guard that selects a version's block in game_version.h / game_patches.c; 1.0.0 is the #else block.
GUARDS = {'v112': 'TOTK_GAME_V112', 'v121': 'TOTK_GAME_V121', 'v143': 'TOTK_GAME_V143', 'botw190': 'GAME_BOTW190',
          'botw160': 'GAME_BOTW160'}
# The version-specific patch source (game_patches.c is shared by 1.0.0, 1.1.2 and 1.2.1).
PATCHER = {'v0': 'game_patches.c', 'v112': 'game_patches.c', 'v121': 'game_patches.c', 'v143': 'game_patches_v143.c',
           'botw190': 'game_patches_botw.c', 'botw160': 'game_patches_botw.c'}
# What the compiler defines for the Breath of the Wild runtime (game_version.h): the branches of game_patches_botw.c it keeps.
BOTW_DEFINED = {'botw190': {'GAME_BOTW', 'GAME_BOTW190'}, 'botw160': {'GAME_BOTW', 'GAME_BOTW160'}}

Site = namedtuple('Site', 'label offset size')
MAX_IMAGE = 0x8000000     # 128 MiB: above every main image, below every instruction word a table mentions

DEFINE = re.compile(r'#define\s+GAME_ADDR_(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)')
GAME_ADDR = re.compile(r'\bGAME_ADDR\s*\(\s*(0x[0-9a-fA-F]+)\s*\)')
DIRECTIVE = re.compile(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)')
# Literal site tables: Patch initializers ({address, original, replacement}, the address first), flat uint64_t site
# lists (the ultrawide stub sites) and the two ways a site is written through a helper call.
PATCH_ENTRY = re.compile(r'\{[ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*0x[0-9a-fA-F]+[ \t]*,')
SITE_LIST = re.compile(r'uint64_t[ \t]+sites\[\][ \t]*=[ \t]*\{([^}]*)\}')
BRANCH_ARG = re.compile(r'\bbranch[ \t]*\([ \t]*(0x[0-9a-fA-F]+)')
BASE_PLUS = re.compile(r'm->base[ \t]*\+[ \t]*(0x[0-9a-fA-F]+)')
CLOTH = re.compile(r'cloth_inline\[3\][ \t]*=[ \t]*\{(.*?)\};', re.S)
CLOTH_ENTRY = re.compile(r'\{([^{}]*)\}')
BOTW_ROW = re.compile(r'\{[ \t]*(0x[0-9a-fA-F]+)u?[ \t]*,')       # a row of game_patches_botw.c's tables: {main offset, word, ...}
BOTW_SLINK = re.compile(r'enum \{[ \t]*SITE[ \t]*=[ \t]*(0x[0-9a-fA-F]+),[ \t]*BACK[ \t]*=[ \t]*(0x[0-9a-fA-F]+)')   # 1.6.0's sound-link rate: the branch site and where it returns
BOTW_UW = re.compile(r'#define UW_WRITER_AT[ \t]+(0x[0-9a-fA-F]+)u?\b.*?\buw_writer_original\[\][ \t]*=[ \t]*\{([^}]*)\}', re.S)   # the ultrawide aspect writer: its address, then its original words
BOTW_PHONE = re.compile(r'\bPHONE_RENDER[ \t]*=[ \t]*(0x[0-9a-fA-F]+)')   # enum { PHONE_RENDER = 0x.. }: the phone's mov, movk, str
BOTW_SIZE_INIT = re.compile(r'\bsize_init\[\][ \t]*=[ \t]*\{([^}]*)\}')   # 1.6.0's output size: movz at each, movk 12 bytes on
BOTW_MOVK = re.compile(r'\bRENDER_MOVK[ \t]*=[ \t]*(0x[0-9a-fA-F]+)')
BOTW_WORDS = re.compile(r'\b(\w+)[ \t]*=[ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*\1_WORDS[ \t]*=[ \t]*(\d+)')   # enum { PAD_LOOP = 0x.., PAD_LOOP_WORDS = 38 }
PCHTXT_TABLE = re.compile(r'g_pchtxt_reserved\[\][ \t]*=[ \t]*\{.*?\n\};', re.S)   # TOTK_PCHTXT's copy of this list, not sites
JQ_TABLE = re.compile(r'g_jq_site\[JQ_SITES\][ \t]*=[ \t]*\{.*?\n\};', re.S)
CONDITION = re.compile(r'\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)')
CLOTH_CALL = re.compile(r'cloth_call[ \t]*=[ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*cloth_update[ \t]*=[ \t]*(0x[0-9a-fA-F]+)')

_unresolved = set()     # logical GAME_ADDR names a version does not define (drift alarm; expected empty)


def _guard_version(guard):
    for version, macro in GUARDS.items():
        if macro in guard:
            return version
    # An unrelated conditional (TARGET_OS_IPHONE, the trailing #ifndef GAME_NAME): no site tables.
    return 'v0' if not guard.strip() else None


def blocks(text):
    """(version, body) for each branch of a file's top-level #if / #elif / #else chain."""
    found, version, body, depth = [], 'v0', [], 0
    for line in text.splitlines():
        match = DIRECTIVE.match(line.strip())
        if match:
            kind, guard = match.group(1), match.group(2)
            if kind in ('if', 'ifdef', 'ifndef'):
                depth += 1
                if depth == 1:
                    version, body = _guard_version(guard), []
                continue
            if kind == 'endif':
                depth -= 1
                if depth == 0:
                    found.append((version, '\n'.join(body)))
                continue
            if depth == 1 and kind in ('elif', 'else'):
                found.append((version, '\n'.join(body)))
                version, body = (_guard_version(guard) if kind == 'elif' else 'v0'), []
                continue
        if depth:
            body.append(line)
    if depth:
        found.append((version, '\n'.join(body)))
    return found


def branch_spans(text):
    """(version, start, end) character spans of the body of each branch of a file's top-level #if chain.

    An #else belongs to the version its chain names (1.0.0 is written as the #else of the version chain) and to no
    version when the chain is an unrelated conditional (TARGET_OS_IPHONE): those branches are shared by
    every version. Directives stay outside the spans, so line numbers do not move."""
    spans, depth, start, version, chain = [], 0, 0, None, []
    position = 0
    for line in text.splitlines(keepends=True):
        match = DIRECTIVE.match(line.strip())
        if match:
            kind, guard = match.group(1), match.group(2)
            if kind in ('if', 'ifdef', 'ifndef'):
                depth += 1
                if depth == 1:
                    version = _guard_version(guard)
                    start, chain = position + len(line), [version]
                position += len(line)
                continue
            if kind == 'endif':
                depth -= 1
                if depth == 0:
                    spans.append((version, start, position))
                position += len(line)
                continue
            if depth == 1:
                spans.append((version, start, position))
                version = _guard_version(guard) if kind == 'elif' else ('v0' if any(chain) else None)
                chain.append(version)
                start = position + len(line)
                position += len(line)
                continue
        position += len(line)
    if depth:
        spans.append((version, start, position))
    return spans


def version_offsets(version):
    """{logical 1.0.0 address: the address in this version}: the GAME_ADDR table of runtime/game_version.h."""
    for found, body in blocks((RUNTIME / 'game_version.h').read_text()):
        if found == version:
            return {name.lower(): int(offset, 16) for name, offset in DEFINE.findall(body)}
    raise ValueError(f'game_version.h has no block for {version}')


def _blank(text, spans):
    """The same text with the spans (newlines kept) replaced by spaces, so line numbers do not move."""
    for start, end in sorted(spans, reverse=True):
        text = text[:start] + re.sub(r'[^\n]', ' ', text[start:end]) + text[end:]
    return text


def _sites_in_literals(text, source):
    found = []
    for match in PATCH_ENTRY.finditer(text):
        found.append((int(match.group(1), 16), _line_of(text, match.start())))
    for match in SITE_LIST.finditer(text):
        for number in re.findall(r'0x[0-9a-fA-F]+', match.group(1)):
            found.append((int(number, 16), _line_of(text, match.start())))
    for pattern in (BRANCH_ARG, BASE_PLUS):
        for match in pattern.finditer(text):
            found.append((int(match.group(1), 16), _line_of(text, match.start())))
    return [Site(f'{source}:{line}', offset, 4) for offset, line in found]


def _line_of(text, index):
    return text.count('\n', 0, index) + 1


def game_addr_sites(version):
    """Every GAME_ADDR(...) use in the runtime sources this version compiles, resolved for it."""
    offsets, found = version_offsets(version), []
    skipped = {'game_version.h', 'game_patches_botw.c'} | {name for name in PATCHER.values() if name != PATCHER[version]}
    for path in sorted(RUNTIME.glob('*.c')) + sorted(RUNTIME.glob('*.h')):
        if path.name in skipped:
            continue
        text = path.read_text()
        for match in GAME_ADDR.finditer(text):
            name = match.group(1).lower()
            if name not in offsets:
                if version not in BOTW_DEFINED:     # Breath of the Wild compiles the TOTK-only uses out (GAME_BOTW)
                    _unresolved.add(name)
                continue
            found.append(Site(f'{path.name}:{_line_of(text, match.start())}', offsets[name], 4))
    return found


def patcher_sites(version):
    """The literal Patch / veneer sites of the version-specific patch source.

    game_patches.c carries one cloth table and one ultrawide site list per version inside #if blocks; each is read for
    its own version only, so an offset that belongs to another version can never be reported here: the generic scan
    runs on the file with the branches of the other versions blanked, and this version's block supplies the cloth
    sites. Branches of unrelated conditionals are shared and stay in."""
    source = PATCHER[version]
    text = (RUNTIME / source).read_text()
    if version in BOTW_DEFINED:
        return botw_sites(text, source, BOTW_DEFINED[version])
    foreign = [(start, end) for block, start, end in branch_spans(text) if block not in (version, None)]
    scoped = _blank(text, foreign)
    found = _sites_in_literals(_blank(scoped, [m.span() for m in CLOTH.finditer(text)]), source)
    for block, body in blocks(text):
        if block != version:
            continue
        for match in CLOTH.finditer(body):
            for entry in CLOTH_ENTRY.findall(match.group(1)):
                numbers = re.findall(r'(?:-)?0x[0-9a-fA-F]+', entry)
                for number in numbers[:2]:       # {fmul, store, ...}: the two instructions the clock veneers replace
                    found.append(Site(f'{source} cloth_inline+{int(number, 16):#x}', int(number, 16), 4))
        for match in CLOTH_CALL.finditer(body):
            for number in match.groups():
                found.append(Site(f'{source} cloth_call/update+{int(number, 16):#x}', int(number, 16), 4))
    return found


def compiled_text(text, defined):
    """The text of a C file with the lines of the branches that a build with these macros does not compile blanked (the
    directives too; newlines stay, so line numbers do not move). #ifdef, #ifndef and #if defined(NAME) are evaluated; any
    other condition counts as true, so a site under it is reserved rather than missed."""
    stack, out = [], []        # stack of [parent compiles, this branch compiles, the condition is evaluated, a branch was taken]
    for line in text.splitlines(keepends=True):
        match = CONDITION.match(line)
        if not match:
            out.append(line if all(entry[1] for entry in stack) else re.sub(r'[^\n]', ' ', line))
            continue
        kind, rest = match.groups()
        parent = all(entry[1] for entry in stack)
        if kind in ('ifdef', 'ifndef', 'if'):
            name = rest.split()[0] if kind != 'if' and rest.split() else (re.fullmatch(r'\s*defined\s*\(\s*(\w+)\s*\)\s*', rest) or [None, None])[1]
            known = name is not None
            on = (name in defined) != (kind == 'ifndef') if known else True
            stack.append([parent, on, known, on])
        elif kind in ('elif', 'else') and stack:
            entry = stack[-1]
            entry[1] = not entry[3] if entry[2] and kind == 'else' else True
            entry[3] = entry[3] or entry[1]
        elif kind == 'endif' and stack:
            stack.pop()
        out.append(re.sub(r'[^\n]', ' ', line))
    return ''.join(out)


def botw_sites(text, source, defined):
    """The code sites Breath of the Wild's runtime reads or writes (game_patches_botw.c, the branches `defined` keeps): the
    rows {main offset, original word, ...} of its check, put and (1.6.0 only) job-queue tables, and the render-size site, each one
    instruction, and the sequences it checks and rewrites word by word (1.6.0's pad loop and menu countdown: NAME = offset,
    NAME_WORDS = count), the phone's handheld render size (PHONE_RENDER: three instructions), 1.6.0's sound-link rate (SITE and BACK), the ultrawide aspect writer (UW_WRITER_AT and uw_writer_original: 14 words on 1.6.0, 13 on 1.9.0), and 1.6.0's output-size initialisers (size_init: four instructions from each). A mod patch on one would make the runtime refuse the start (the check) or write over the mod (the put)."""
    active = PCHTXT_TABLE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group()), compiled_text(text, defined))
    if 'GAME_BOTW160' not in defined:     # TOTK_JQ_CHECK's table holds 1.6.0 addresses only (on 1.9.0 the words differ and
        active = JQ_TABLE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group()), active)    # the check stays unarmed)
    found = [(int(match.group(1), 16), _line_of(active, match.start()), 4) for pattern in (BOTW_ROW, BOTW_MOVK)
             for match in pattern.finditer(active)]
    found += [(int(match.group(2), 16), _line_of(active, match.start()), 4 * int(match.group(3)))
              for match in BOTW_WORDS.finditer(active)]
    found += [(int(match.group(1), 16), _line_of(active, match.start()), 12) for match in BOTW_PHONE.finditer(active)]
    found += [(int(number, 16), _line_of(active, match.start()), 4) for match in BOTW_SLINK.finditer(active) for number in match.groups()]
    found += [(int(match.group(1), 16), _line_of(active, match.start()), 4 * len(re.findall(r'0x[0-9a-fA-F]+', match.group(2))))
              for match in BOTW_UW.finditer(active)]
    found += [(int(number, 16), _line_of(active, match.start()), 16) for match in BOTW_SIZE_INIT.finditer(active)
              for number in re.findall(r'0x[0-9a-fA-F]+', match.group(1))]
    return [Site(f'{source}:{line}', offset, size) for offset, line, size in found]


def _table_body(text, table):
    """The body of a module-level table of patch_nso.py, without its closing bracket."""
    match = re.search(rf'\n{table} = \{{(.*?)\n[\]}}]*\}}', text, re.S)
    if not match:
        raise ValueError(f'patch_nso.py: cannot read the {table} table')
    return match.group(1)


def _section(body, build_id):
    """The part of a table that belongs to one NSO build ID (entries are grouped under a quoted 32-hex key)."""
    parts = re.split(r'(?:^|\n)[ \t]*[\'"]?([0-9a-fA-F]{32})[\'"]?[ \t]*:', body)
    for index in range(1, len(parts), 2):
        if parts[index].lower() == build_id.lower():
            return parts[index + 1]
    return None


SIGNED_ENTRY = re.compile(r'(0x[0-9a-fA-F]+)[ \t]*:[ \t]*\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*(0x[0-9a-fA-F]+)'
                           r'[ \t]*,[ \t]*\(([-0-9,\s]*)\)')      # an offset may be negative


def signed_table_sites(build_id):
    """The signed 32-bit jump tables patch_nso.py knows for a build ID (SIGNED_TABLES): the dispatch instruction whose
    bytes it checks, and the table of offsets it reads. A mod patch on either moves a jump the rewriter relies on, so both
    are reserved - the same reason CUTS and PARAMS are.

    The dispatch is reserved with the 16 bytes before it: patch_nso.py asserts IMG[dispatch-0x10:dispatch+4] against the
    table's own copy of those bytes, so an edit anywhere in that window fires its assert mid-rewrite."""
    if not build_id:
        return []
    body = _section(_table_body(REWRITER.read_text(), 'SIGNED_TABLES'), build_id)
    if body is None:
        return []
    found = []
    for match in SIGNED_ENTRY.finditer(body):      # 0xdispatch: (0xadrbase, 0xtable, (offsets...), 'dispatch word')
        dispatch, _, table, offsets = match.groups()
        found.append(Site('patch_nso.py SIGNED_TABLES', int(dispatch, 16) - 16, 0x14))
        count = len([offset for offset in offsets.split(',') if offset.strip()])
        if count:
            found.append(Site('patch_nso.py SIGNED_TABLES', int(table, 16), 4*count))
    return found


def rewriter_sites(build_id):
    """The rewriter's registered cuts (CUTS), observation gates (OBS) and parameter sites (PARAMS) for a build ID."""
    if not build_id:
        return []
    text = REWRITER.read_text()
    found = []
    for table in ('CUTS', 'OBS', 'PARAMS'):
        body = _section(_table_body(text, table), build_id)
        if body is None:
            continue
        if table == 'CUTS':      # (offset, original bytes, new bytes, reason[, scope])
            for match in re.finditer(r'\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*[\'"]([0-9a-fA-F]*)[\'"]', body):
                found.append(Site('patch_nso.py CUTS', int(match.group(1), 16), len(match.group(2)) // 2))
        elif table == 'OBS':     # (offset, observation number, original word, reason)
            for match in re.finditer(r'\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*\d+[ \t]*,', body):
                found.append(Site('patch_nso.py OBS', int(match.group(1), 16), 4))
        else:                    # (offset a, word a, offset b, word b, register, key, reason)
            for match in re.finditer(r'\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,[ \t]*[\'"][0-9a-fA-F]+[\'"]'
                                     r'[ \t]*,[ \t]*(0x[0-9a-fA-F]+)', body):
                for offset in match.groups():
                    found.append(Site('patch_nso.py PARAMS', int(offset, 16), 4))
    return found


def sites(version, build_id=None):
    """Every reserved site for this version, sorted by address and without duplicates.

    build_id may be given as the 16 bytes the rewriter's tables are keyed on (32 hex digits) or as the 20 bytes the
    runtime compares (40 hex digits): only the first 16 bytes select a table.

    Values at or above MAX_IMAGE are instruction words and register constants that a table happens to mention, not
    addresses (the largest main image is under 80 MiB); they are dropped, so a mod patch in the image never sees one.
    """
    if version not in VERSIONS:
        raise ValueError(f'unknown version {version}')
    if build_id:
        build_id = build_id.lower()[:32]
    every = game_addr_sites(version) + patcher_sites(version) + rewriter_sites(build_id) + signed_table_sites(build_id)
    unique = {}
    for site in every:
        if not 0 < site.offset < MAX_IMAGE or not 0 < site.size < 0x1000:
            continue
        unique.setdefault((site.offset, site.size), site)
    return [unique[key] for key in sorted(unique)]


def overlaps(edits, reserved):
    """[(offset, size, site)]: every reserved site an edit range [offset, offset + size) touches."""
    hits = []
    for offset, size in edits:
        for site in reserved:
            if offset < site.offset + site.size and site.offset < offset + size:
                hits.append((offset, size, site))
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--version', choices=VERSIONS, required=True)
    parser.add_argument('--build-id', help='the 32-hex-digit NSO build ID being rewritten (16 bytes at 0x40) (adds that build\u2019s cuts)')
    parser.add_argument('--json', action='store_true', help='print a JSON array instead of one line per site')
    args = parser.parse_args()
    found = sites(args.version, args.build_id)
    if args.json:
        print(json.dumps([dict(label=s.label, offset=f'{s.offset:#x}', size=s.size) for s in found], indent=2))
        return
    print(f'# {len(found)} reserved sites for {args.version}' + (f' (build {args.build_id})' if args.build_id else ''))
    for site in found:
        print(f'{site.offset:#010x}  {site.size:2d}  {site.label}')
    if _unresolved:
        print(f'# no {args.version} address for: {", ".join(sorted(_unresolved))}', file=sys.stderr)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Does every SDK function the Smash host replaces answer in the type the original answers in? (Q69)

The host replaces SDK functions by name (the {"<mangled name>", fn} tables in runtime/*.c, reached through
runtime_override). A replacement that answers in the wrong type does not crash and logs no warning: the host reports
success, the game reads a failure and tries again. That was the Spirit Board's endless refill animation (Q66): the SDK's
nn::time::CalculateSpanBetween returns a bool (1 = span stored), the host returned an nn::Result (0 = success).

For each table entry this reads
  1. the host side: the C return type of the replacement and the values its return statements give;
  2. the SDK side: what the original leaves in w0/x0 at each of its returns, from the rewritten sdk module
     (capstone, following the branches into each return and the callee of a call one level deep):
     constants 0/1, cset, `and #1`, ldrb = bool; a constant above 1 = nn::Result; a 64-bit load or pointer = 64-bit;
and prints one row per entry with a verdict:
  MISMATCH  the two types disagree in a way the game can read (bool vs Result, value vs void, 64 vs 32 bit);
  CHECK     the SDK side is a bool and the host answers in 32 bits (often fine, `no` = false; read the row), or one
            side could not be classified;
  ok        the same class on both sides.

    override_audit.py                 all entries, MISMATCH and CHECK first
    override_audit.py --only MISMATCH just those rows
    override_audit.py --name <substr> the evidence for matching entries
"""
import argparse
import bisect
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
RUNTIME = HERE / 'runtime'
WORK = HERE / 'SSBU_build'
SDK_IMAGE = WORK / 'rewritten/v1302/sdk.patched.bin'

PAIR = re.compile(r'\{\s*"(_Z[^"]+|nn[a-z][A-Za-z0-9_]*)"\s*,\s*(?:\(void\s*\*\)\s*)?([A-Za-z_]\w*)\s*\}')
BOOL_TYPES = {'bool', '_Bool', 'uint8_t', 'unsigned char', 'char', 'int8_t'}
U32_TYPES = {'uint32_t', 'unsigned', 'unsigned int', 'int', 'int32_t', 'Result', 'uint16_t', 'int16_t'}
U64_TYPES = {'uint64_t', 'int64_t', 'long', 'unsigned long', 'long long', 'unsigned long long', 'size_t', 'uintptr_t',
             'intptr_t', 'ssize_t'}


TABLE_FN = re.compile(r'void\s*\*\s*(runtime_\w+)\s*\(\s*const char\s*\*\s*\w+\s*\)')
# Rows read by hand against the SDK code and the game's call sites (name -> reason); the tool's reading of them is wrong.
RESULT_IN_SDK = 'nn::Result in the SDK (the original ends in an IPC or tail call the tool does not follow); host 0 = success'
REVIEWED = {
    '_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE':
        'void in the SDK (sdk+0x99ad0: one return leaves the table pointer in x0, the other tail-calls the unlock)',
    '_ZN2nn6applet26RequestToLaunchApplicationENS_3ncm13ApplicationIdENS0_13StorageHandleE':
        'Result in the SDK (w19 from the service, sdk+0x80430); the host restarts the game and never returns (Q74)',
    '_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi': RESULT_IN_SDK,
    '_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi': RESULT_IN_SDK,
    '_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE': 'void in the SDK (aborts on failure); the 0 is a leftover',
    '_ZN2nn6applet23GetAppletResourceUserIdEv': '64-bit id in the SDK, host answers 64 bits',
    '_ZN2nn2oe25GetProgramTotalActiveTimeEv': 'nn::TimeSpan (nanoseconds, 64 bits) in the SDK, host answers nanoseconds',
    '_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE': 'bool read in 32 bits; the Mac ABI widens a uint8_t answer',
    '_ZN2nn2fs6detail18IsEnabledAccessLogEv': 'bool in the SDK, host answers false',
    '_ZN2nn3hid26FinalizeSevenSixAxisSensorEv': RESULT_IN_SDK,
    '_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm': RESULT_IN_SDK,
    '_ZN2nn3hid22StopSevenSixAxisSensorEv': RESULT_IN_SDK,
    '_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE': 'bool read in 32 bits; the Mac ABI widens it',
    '_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE': RESULT_IN_SDK,
    '_ZN2nn2ro10InitializeEv': RESULT_IN_SDK,
    '_ZN2nn2pl8IsVrModeEv': 'bool read in 32 bits; the Mac ABI widens it',
    '_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE': RESULT_IN_SDK,
    '_ZN2nn3mii8Database9IsUpdatedEi': 'bool read in 32 bits; the Mac ABI widens it',
    '_ZN2nn4pctl26GetStereoVisionRestrictionEv': 'bool read in 32 bits; the Mac ABI widens it',
    '_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE': 'CalendarTime (8 bytes) in x0 on both sides',
    '_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE': 'PosixTime (64 bits) on both sides',
}


def host_entries(runtime=RUNTIME):
    """(mangled name, host function, file, table function) for every table pair in the runtime sources. Pairs in a
    runtime_*_observer table wrap the original and hand its answer back, so their type is the original's."""
    out = []
    for f in sorted(runtime.glob('*.c')):
        text = f.read_text(errors='replace')
        tables = [(m.start(), m.group(1)) for m in TABLE_FN.finditer(text)]
        for m in PAIR.finditer(text):
            owner = [t for p, t in tables if p < m.start()]
            out.append((m.group(1), m.group(2), f, owner[-1] if owner else '?'))
    return out


def body_after(text, i):
    """The function body starting at the brace at i, braces matched, strings and comments skipped."""
    depth, j, n = 0, i, len(text)
    while j < n:
        c = text[j]
        if c == '"' or c == "'":
            q = c; j += 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
        elif text.startswith('//', j):
            j = text.find('\n', j)
            if j < 0: return text[i:]
        elif text.startswith('/*', j):
            j = text.find('*/', j) + 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return text[i:j + 1]
        j += 1
    return text[i:]


def host_type(fn, files):
    """(class, C type, return expressions) of the host function fn, searched in its own file first."""
    pat = re.compile(r'(?:^|[;}])\s*(?:static\s+)?(?:inline\s+)?((?:const\s+)?[A-Za-z_][\w ]*?[\s*]+)' + re.escape(fn)
                     + r'\s*\(([^;{}]*)\)\s*\{', re.M)
    for f in files:
        text = f.read_text(errors='replace')
        m = pat.search(text)
        if not m: continue
        ctype = ' '.join(m.group(1).replace('*', ' * ').split()).replace(' *', '*')
        body = body_after(text, m.end() - 1)
        rets = [r.strip() for r in re.findall(r'\breturn\b([^;]*);', body)]
        base = ctype.replace('const ', '').strip()
        if '*' in base: cls = 'u64'
        elif base == 'void': cls = 'void'
        elif base in BOOL_TYPES: cls = 'bool'
        elif base in U32_TYPES: cls = 'u32'
        elif base in U64_TYPES: cls = 'u64'
        else: cls = 'other'
        return cls, ctype, rets
    return 'unknown', '?', []


class Sdk:
    def __init__(self):
        import capstone
        mods = {m['name']: m for m in json.loads((WORK / 'analysis/runtime-layout.json').read_text())}
        self.img = SDK_IMAGE.read_bytes()
        syms = mods['sdk']['symbols']
        self.by_name = {s['name']: (s['value'], s['size']) for s in syms if s['defined'] and s['type'] == 2 and s['value']}
        rows = sorted((v, sz, n) for n, (v, sz) in self.by_name.items())
        self.starts = [r[0] for r in rows]
        self.rows = rows
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.cache = {}

    def function_at(self, off):
        i = bisect.bisect_right(self.starts, off) - 1
        if i >= 0 and self.rows[i][0] == off: return self.rows[i]
        return None

    def classify(self, start, size, depth=0):
        """Set of what reaches w0/x0 at the function's returns: ('const', v) ('bool',) ('u8',) ('u32',) ('u64',)
        ('call', target or None) ('none',)."""
        key = (start, depth)
        if key in self.cache: return self.cache[key]
        self.cache[key] = {('loop',)}
        ins = list(self.md.disasm(self.img[start:start + size], start))
        at = {x.address: k for k, x in enumerate(ins)}
        into = {}                                            # branch target -> branch instructions
        for k, x in enumerate(ins):
            t = branch_target(x)
            if t is not None and t in at: into.setdefault(t, []).append(k)
        found, seen = set(), set()

        def walk(k, reg):                                    # backward from index k (inclusive): who wrote register reg
            while k >= 0:
                if (k, reg) in seen: return
                seen.add((k, reg))
                x = ins[k]
                src = reg_source(x, reg)
                if src and src[0] == 'copy':
                    reg = src[1]
                elif src and src[0] == 'fork':                # csel: either register, a zero register is 0
                    for r in src[1]:
                        if r is None: found.add(('const', 0))
                        else: walk(k - 1, r)
                    return
                elif src:
                    found.add(src); return
                for b in into.get(x.address, ()):
                    walk(b - 1, reg)
                prev = ins[k - 1] if k else None
                if prev is None or prev.mnemonic in ('ret', 'br') or prev.mnemonic == 'b' and branch_target(prev) is not None:
                    return
                k -= 1
            found.add(('none',))

        for k, x in enumerate(ins):
            if x.mnemonic == 'ret':
                walk(k - 1, 0)
            elif x.mnemonic == 'b' and branch_target(x) is not None and branch_target(x) not in at:
                found.add(('call', branch_target(x)))        # tail call
        out = set()
        for s in found:                                      # follow a call one level: its answer is the answer
            if s[0] == 'call' and s[1] is not None and depth < 2:
                f = self.function_at(s[1])
                out |= self.classify(f[0], f[1], depth + 1) if f and f[1] else {s}
            else:
                out.add(s)
        self.cache[key] = out
        return out


def branch_target(x):
    if x.mnemonic in ('b', 'bl') or x.mnemonic.startswith('b.') or x.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
        try: return int(x.op_str.split('#')[-1], 16)
        except ValueError: return None
    return None


REG = re.compile(r'^[wx](\d+)$')


def reg_source(x, reg):
    """What instruction x puts into register reg (w<reg>/x<reg>): None when it leaves it alone, ('copy', n) when it
    moves register n there, else what the value is (see Sdk.classify)."""
    m, ops = x.mnemonic, x.op_str
    if m in ('bl', 'blr'):                                   # a call answers in x0 and clobbers x1-x18
        if reg == 0: return ('call', branch_target(x) if m == 'bl' else None)
        return ('clobbered',) if reg <= 18 else None
    if m.startswith(('st', 'cmp', 'cmn', 'tst', 'cb', 'tb', 'prfm')) or m.startswith('b.') or m in ('b', 'ret', 'br'):
        return None
    parts = [p.strip() for p in ops.split(',')]
    dests = [parts[0]] + ([parts[1]] if m == 'ldp' and len(parts) > 1 else [])
    hit = [d for d in dests if REG.match(d) and int(REG.match(d).group(1)) == reg]
    if not hit:
        return None
    if m == 'ldp':
        return ('u64',) if hit[0].startswith('x') else ('u32',)
    wide = hit[0].startswith('x')
    rest = ops.split(',', 1)[1].strip()
    if m in ('mov', 'orr') and REG.match(rest.replace('xzr, ', '').replace('wzr, ', '')) and rest not in ('wzr', 'xzr'):
        return ('copy', int(REG.match(rest.replace('xzr, ', '').replace('wzr, ', '')).group(1)))
    if m == 'csel' and len(parts) >= 3:
        return ('fork', [None if p in ('wzr', 'xzr') else int(REG.match(p).group(1)) if REG.match(p) else None
                         for p in parts[1:3]])
    if m in ('mov', 'movz', 'orr') and (rest in ('wzr', 'xzr') or rest.startswith('#') or rest.startswith('wzr, #')):
        v = 0 if rest in ('wzr', 'xzr') else int(rest.split('#')[-1].split()[0], 16 if '0x' in rest else 10)
        return ('const', v)
    if m == 'movk':
        return ('const', 0x10000)
    if m in ('cset', 'csinc', 'cinc') or m == 'and' and rest.endswith('#1'):
        return ('bool',)
    if m == 'ldrb' or m == 'and' and rest.endswith('#0xff'):
        return ('u8',)
    if m == 'and' and rest.endswith('#0xffffffff') or m in ('uxtw', 'ubfx') and rest.endswith('#32'):
        return ('u32',)                                  # nn::Result is a 32-bit struct, returned zero-extended in x0
    return ('u64',) if wide else ('u32',)


def sdk_class(srcs):
    if not srcs: return 'unknown'                            # no return reached (the function ends in an abort)
    kinds = {s[0] for s in srcs}
    consts = {s[1] for s in srcs if s[0] == 'const'}
    if any(v > 1 for v in consts): return 'result'
    if 'u64' in kinds: return 'u64'
    if kinds <= {'const', 'bool', 'u8'} and (1 in consts or kinds & {'bool', 'u8'}): return 'bool'
    if kinds <= {'const'} and consts == {0}: return 'zero'
    if kinds <= {'const', 'u32', 'bool', 'u8'}: return 'r32'
    if kinds == {'none'}: return 'void'
    return 'unknown'


RESULT_CODE = re.compile(r'^(R_\w+|NFP_\w+|0x[0-9a-fA-F]+|[2-9]\d*|1\d+)$')


def verdict(host, sdk, rets):
    """MISMATCH / CHECK / ok for one replacement (see the module docstring)."""
    codes = any(RESULT_CODE.match(r) for r in rets)
    if host in ('unknown', 'other'):
        return 'CHECK'
    if sdk == 'bool':
        if host == 'bool' or rets and set(rets) <= {'0', '1'} and '1' in rets: return 'ok'
        return 'MISMATCH' if host == 'void' or codes else 'CHECK'   # Result codes where the game reads a bool (Q66);
        #                                                             CHECK: a 32-bit 0 there is "false", meant so?
    if sdk == 'result':
        return 'ok' if host in ('u32', 'u64') else 'MISMATCH'
    if sdk == 'r32':
        return 'ok' if host in ('u32', 'u64') else ('MISMATCH' if host == 'void' else 'CHECK')
    if sdk == 'u64':
        return 'ok' if host == 'u64' else 'MISMATCH'
    if sdk == 'zero':
        return 'ok' if host != 'void' else 'CHECK'
    if sdk == 'void':
        return 'ok'
    return 'ok' if host == 'void' else 'CHECK'                    # unknown SDK side: a host value may be unread


class Callers:
    """Where the game calls an imported SDK function, and whether it reads w0/x0 afterwards (main and subsdk0; the
    fighter NROs load at run time and are not in the layout)."""

    def __init__(self, names):
        import capstone
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.sites = {}                                      # name -> [(module, call address, 'reads' | 'ignores' | '?')]
        mods = json.loads((WORK / 'analysis/runtime-layout.json').read_text())
        for m in mods:
            if m['name'] == 'sdk': continue
            img = (WORK / f"rewritten/v1302/{m['name']}.patched.bin").read_bytes()
            text_end = m['layout'].get('TEXT_END', len(img))
            got = {r['where']: m['symbols'][r['symbol']]['name'] for r in m['relocs'] if r['kind'] == 1026}
            words = list(struct.iter_unpack('<I', img[:text_end - text_end % 4]))
            stubs = {}
            for i in range(len(words) - 3):                  # adrp x16, page; ldr x17, [x16, #off]; add; br x17
                w = words[i][0]
                if w & 0x9f00001f != 0x90000010 or words[i + 1][0] & 0xffc003ff != 0xf9400211: continue
                if words[i + 3][0] != 0xd61f0220: continue
                pc = i * 4
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                if imm & (1 << 20): imm -= 1 << 21
                page = (pc & ~0xfff) + (imm << 12)
                slot = page + ((words[i + 1][0] >> 10) & 0xfff) * 8
                if got.get(slot) in names: stubs[pc] = got[slot]
            for i, (w,) in enumerate(words):                 # bl <stub>
                if w & 0xfc000000 != 0x94000000: continue
                off = w & 0x3ffffff
                if off & (1 << 25): off -= 1 << 26
                t = i * 4 + off * 4
                if t in stubs:
                    self.sites.setdefault(stubs[t], []).append((m['name'], i * 4, self.use(img, i * 4 + 4)))

    def use(self, img, pc):
        """'reads' when an instruction after the call reads w0/x0 before anything writes it, 'ignores' when it is
        overwritten or a call comes first, '?' when control leaves the straight line first."""
        for x in self.md.disasm(img[pc:pc + 40], pc):
            parts = [p.strip() for p in x.op_str.replace('[', ' ').replace(']', ' ').replace('!', ' ').split(',')]
            regs = [p.split()[0] for p in parts if p]
            if x.mnemonic in ('ret',):
                return 'reads'                               # passed on to the game's caller
            if x.mnemonic in ('bl', 'blr', 'b', 'br') or x.mnemonic.startswith('b.'):
                return 'ignores' if x.mnemonic in ('bl', 'blr') else '?'
            reads_from = regs if x.mnemonic.startswith(('st', 'cmp', 'cmn', 'tst', 'cb', 'tb')) else regs[1:]
            if x.mnemonic == 'ldp' or x.mnemonic.startswith('ld'): reads_from = regs[1:]
            if any(r in ('w0', 'x0') for r in reads_from): return 'reads'
            if regs and regs[0] in ('w0', 'x0'): return 'ignores'
        return '?'


def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--only', choices=['MISMATCH', 'CHECK', 'ok'])
    ap.add_argument('--name')
    ap.add_argument('--runtime', type=Path, default=RUNTIME, help='host sources to read (default games/ssbu/runtime)')
    a = ap.parse_args()
    files = sorted(a.runtime.glob('*.c'))
    sdk = Sdk()
    entries = host_entries(a.runtime)
    stale = sorted(set(REVIEWED) - {e[0] for e in entries})
    if stale:
        print(f'reviewed rows no table replaces any more: {" ".join(stale)}')
    dem = demangle(sorted({e[0] for e in entries}))
    callers = Callers({e[0] for e in entries})
    rows, seen = [], set()
    for name, fn, f, table in entries:
        if (name, fn) in seen: continue
        seen.add((name, fn))
        if 'observer' in table:
            continue                                         # wraps the original and returns its answer
        hcls, ctype, rets = host_type(fn, [f] + [g for g in files if g != f])
        if name in sdk.by_name:
            v, sz = sdk.by_name[name]
            srcs = sdk.classify(v, sz)
            scls = sdk_class(srcs)
            ev = ' '.join(sorted({s[0] if s[0] != 'const' else f'#{s[1]:#x}' for s in srcs}))
        else:
            scls, ev = 'absent', 'not in sdk'
        verd = verdict(hcls, scls, rets) if scls != 'absent' else 'ok'
        sites = callers.sites.get(name, [])
        use = {k: sum(1 for s in sites if s[2] == k) for k in ('reads', 'ignores', '?')}
        if verd != 'ok' and sites and not use['reads'] and not use['?']:
            verd = 'ok'                                      # the game never looks at the answer
        calls = f"{len(sites)} calls: reads {use['reads']}, ignores {use['ignores']}, unclear {use['?']}"
        if name in REVIEWED:
            verd, calls = 'ok', calls + '; reviewed: ' + REVIEWED[name]
        if a.name and (a.name in name or a.name in dem.get(name, '')):
            calls += ' at ' + ' '.join(f'{s[0]}+{s[1]:#x}:{s[2]}' for s in sites[:12])
        rows.append((verd, dem.get(name, name), fn, f.name, hcls, ctype, rets, scls, ev, name, calls))
    order = {'MISMATCH': 0, 'CHECK': 1, 'ok': 2}
    rows.sort(key=lambda r: (order[r[0]], r[3], r[1]))
    counts = {}
    for r in rows: counts[r[0]] = counts.get(r[0], 0) + 1
    absent = sum(1 for r in rows if r[7] == 'absent')
    print(f'{len(rows)} replaced functions ({absent} not in the sdk module): '
          + ', '.join(f'{k} {counts.get(k, 0)}' for k in ('MISMATCH', 'CHECK', 'ok')))
    for r in rows:
        if a.only and r[0] != a.only: continue
        if a.name and a.name not in r[1] and a.name not in r[9]: continue
        rets = '; '.join(dict.fromkeys(r[6]))[:60]
        print(f'{r[0]:8} {r[1][:90]:90} host {r[2]}() {r[3]} {r[5]} [{rets}] | sdk {r[7]} ({r[8]}) | game {r[10]}')


if __name__ == '__main__':
    sys.exit(main())

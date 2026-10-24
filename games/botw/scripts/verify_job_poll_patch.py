#!/usr/bin/env python3
"""Run Breath of the Wild 1.9.0's job poll (main+0xef6354..0xef63e8) with Unicorn, as shipped and with the job poll pause of
games/totk/runtime/game_patches_botw.c (patch_job_poll_pause; docs/botw-v1.9.0-port.md, "Job poll pause").

sead's job workers poll a queue with no SVC. A pass takes the test-and-test-and-set lock on the word at queue+0x1c (plain
loads while it reads 1, then ldaxr / stxr of 1; the worker's status word, +0x100 of x20 when x20 is not null, is set to 3
before and 4 after) and reads the gate: a worker that is not on core 0 (w0, the core number) waits while the queue's flag
byte at +0x98 is set and nothing has been taken yet (+0x94 <= 0), that is until the core-0 worker has taken the first
batch. It unlocks (stlxr of 0) and goes round again from main+0xef6354 while it waits: the back-edge cbz w14 at
main+0xef63e8. Otherwise (w14 = 1, the queue is open to it) it takes a batch, moving the next index at +0x94 on by at most
w22 up to the count at +0x90 (nothing when the queue is drained), and leaves. The patch makes the back-edge a branch to a
veneer in the zero padding after the text, which leaves as shipped when the queue is open and otherwise waits a pause on
cntvct_el0 before polling again.

Checked, for a 1 GHz and a 24 MHz counter (the pause is a number of ticks the C computes from cntfrq_el0 when it installs it):
1. game_patches_botw.c is read, not restated: the enum (LOOP, SITE, EXIT, VENEER, VENEER_WORDS), check[], veneer[] (hex words,
   branch_to(a, b) and the add of the pause in ticks) and the site's branch_to, with ticks computed as the C does after
   checking that the C still computes them that way. Any other form stops the script.
2. The image holds every check[] word, and the veneer's 4 x VENEER_WORDS bytes are zero padding after the text, in its executable
   pages (TEXT_END / RO_OFF from main.fixups.tsv next to the image, as the runtime's room check uses them).
3. Static: x14, x15 and NZCV are dead at LOOP (every path from LOOP writes them before it reads them), so the veneer may
   change them; the veneer writes nothing else, touches no memory and branches only to LOOP, EXIT or into itself; every
   counter read is cntvct_el0 straight after an isb; no checked word is a plain exclusive load, so the exclusive-acquire
   rewrite that runs after this patch cannot change them.
4. Unicorn, from the poll's setup (the six instructions before LOOP: w13 / w24 zeroed, the lock address, the status values)
   on a synthetic queue built from the poll's own instructions (the lock register and offset, the fields loaded and stored
   through the queue register, the status store), in six scenarios: empty passes and then a batch another worker made
   available, the lock held by another worker for a few spins, no status word (x20 = 0), worker 0 (never waits), a queue
   drained by others, and a counter about to wrap. Shipped and patched leave at the same pc after the same number of
   passes, take the same batch, make the same stores in the same order and end with the same registers; at the start of
   each pass they differ only in registers item 3 proved dead. The shipped runs must also match this script's reading of
   the poll (passes, first job, jobs taken, next index, queue done).
5. A hook on every veneer instruction: the lock word is 0 there (the worker holds no lock while it waits, so a preemption
   may take the core there: loader.c runtime_guest_code_pc), and the veneer is entered only from SITE.
6. cntvct_el0 / cntfrq_el0 reads are intercepted (UC_HOOK_INSN on MRS) by a fake counter that moves on by an uneven step at
   each read: an empty pass polls again only once the counter is ticks past its first read, and at the first read that
   is (no later); a pass that took a batch never reads the counter.
Negative controls, which must fail: the veneer's test inverted (cbnz), the wait removed, the site branching into the wait,
the veneer entered before the unlock, no isb before the first counter read, the wait's comparison inverted, and polling
again one instruction after LOOP (skips the x20 test: the x20 = 0 scenario catches it).
Limits: Unicorn runs one thread and has no memory model. It cannot show the ordering the isb gives the counter read, the
lock's acquire and release, or the store-exclusive failing; the other worker is simulated by writing the queue between
passes. The runtime's exclusive-acquire rewrite (it later makes the unlock's ldxr an ldaxr) is not applied: Unicorn would
run both the same.
usage: verify_job_poll_patch.py [main.patched.bin]   (default totk/rewritten/botw190/main.patched.bin); exit status 1 on any
failure, including a negative control that passes."""
import argparse
import ast
import operator
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import capstone
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_INSN, UC_HOOK_MEM_WRITE, UC_MODE_ARM, UC_PROT_EXEC,
                     UC_PROT_READ)
from unicorn import arm64_const as A

sys.path.insert(0, str(Path(__file__).resolve().parent))
from botw190_unicorn import ROOT

parser = argparse.ArgumentParser(description="Unicorn check of the job poll pause (Breath of the Wild 1.9.0)")
parser.add_argument("image", nargs="?", type=Path)
args = parser.parse_args()
image_path = args.image or ROOT / "rewritten/botw190/main.patched.bin"
image = image_path.read_bytes()
word = lambda a: struct.unpack_from("<I", image, a)[0]
MASK64, NOP = (1 << 64) - 1, 0xd503201f

# 1. patch_job_poll_pause as the C writes it
OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.BitOr: operator.or_, ast.LShift: operator.lshift}


def form_error(what):
    raise SystemExit(f"game_patches_botw.c patch_job_poll_pause: {what} is in a form this script cannot read")


def cexpr(text, names):
    """A C integer expression of literals (u suffixes, (uint32_t) casts), names, +, -, *, | and <<."""
    t = re.sub(r"\b(0x[0-9a-fA-F]+|[0-9]+)[uU]\b", r"\1", text.replace("(uint32_t)", "")).strip()
    try:
        tree = ast.parse(t, mode="eval")
    except SyntaxError:
        form_error(repr(text))

    def ev(n):
        if isinstance(n, ast.Constant) and type(n.value) is int:
            return n.value
        if isinstance(n, ast.Name) and n.id in names:
            return names[n.id]
        if isinstance(n, ast.BinOp) and type(n.op) in OPS:
            return OPS[type(n.op)](ev(n.left), ev(n.right))
        form_error(repr(text))
    return ev(tree.body)


def flat(s):
    return " ".join(re.sub(r"/\*.*?\*/", " ", s, flags=re.S).split())


def found(m, what):
    if not m:
        form_error(what)
    return m


def top_level(text):
    """Split an initializer at the commas outside parentheses."""
    items, depth, cur = [], 0, ""
    for ch in text:
        depth += (ch == "(") - (ch == ")")
        if ch == "," and depth == 0:
            items.append(cur)
            cur = ""
        else:
            cur += ch
    return [i.strip() for i in items + [cur] if i.strip()]


src = (ROOT / "runtime/game_patches_botw.c").read_text()
start = src.find("static void patch_job_poll_pause(Module *m) {")
if start < 0 or src.count("static void patch_job_poll_pause(Module *m) {") != 1:
    form_error("the function")
body = flat(src[start:src.index("\n}\n", start)])
names = {}
for item in found(re.search(r"enum \{([^}]*)\};", body), "the enum").group(1).split(","):
    key, _, value = item.partition("=")
    names[key.strip()] = cexpr(value, {})
if set(names) != {"LOOP", "SITE", "EXIT", "VENEER", "VENEER_WORDS"}:
    form_error(f"the enum ({', '.join(names)})")
LOOP, SITE, EXIT, VENEER, NV = (names[k] for k in ("LOOP", "SITE", "EXIT", "VENEER", "VENEER_WORDS"))
VENEER_END = VENEER + 4 * NV
table = found(re.search(r"check\[\] = \{(.*?)\};", body), "check[]").group(1)
entries = re.findall(r"\{([^{},]+),([^{},]+)\}", table)
if not entries or len(entries) != table.count("{"):
    form_error("check[]")
CHECK = {cexpr(off, names): cexpr(original, {}) for off, original in entries}
veneer_src = []
for item in top_level(found(re.search(r"const uint32_t veneer\[VENEER_WORDS\] = \{(.*?)\};", body), "veneer[]").group(1)):
    if re.fullmatch(r"0x[0-9a-fA-F]{8}u", item):
        veneer_src.append(("word", cexpr(item, {})))
    elif b := re.fullmatch(r"branch_to\(([^(),]+),([^(),]+)\)", item):
        veneer_src.append(("branch", cexpr(b[1], names), cexpr(b[2], names)))
    elif re.fullmatch(r"0x91000000u \| ticks << 10 \| \d+u << 5 \| \d+u", item.replace("(uint32_t)", "")):
        veneer_src.append(("ticks", item))
    else:
        form_error(f"veneer[] entry {item!r}")
if len(veneer_src) != NV:
    form_error(f"veneer[] ({len(veneer_src)} entries for VENEER_WORDS = {NV})")
site_src = found(re.search(r'put_word\(m, SITE, branch_to\(([^(),]+),([^(),]+)\), "job poll pause"\);', body), "the site's put_word")
SITE_BRANCH = (cexpr(site_src[1], names), cexpr(site_src[2], names))
DEFAULT_NS = int(found(re.search(r"strtoll\(e, &end, 10\) : (\d+);", body), "the default pause")[1])
for line in ('uint64_t hz; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));',
             "uint64_t ticks = (uint64_t)(ns < 1000000 ? ns : 1000000) * hz / 1000000000ull;",
             "if (!ticks) ticks = 1;", "if (ticks > 4095) {", "ticks = 4095;",
             'put_words(m, VENEER, veneer, VENEER_WORDS, "job poll pause veneer");'):
    if line not in body:
        form_error(f"the line {line!r} (ticks and the veneer write)")
bt = src.find("static uint32_t branch_to(uint64_t site, uint64_t target) {")
if bt < 0 or not flat(src[bt:]).startswith("static uint32_t branch_to(uint64_t site, uint64_t target) { int64_t d = (int64_t)target "
                                          "- (int64_t)site; return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffffu); }"):
    form_error("branch_to")


def branch_to(site, target):
    return 0x14000000 | (((target - site) >> 2) & 0x3ffffff)


def c_ticks(ns, hz):
    """The C: min(ns, 1 ms) x cntfrq_el0 / 1e9 counter ticks, at least 1 and at most 4095 (add's 12-bit immediate)."""
    return min(max(min(ns, 1000000) * hz // 1000000000, 1), 4095)


def c_patch(ticks):
    """{offset: word} of what the C writes for a pause of `ticks`: the veneer and the site."""
    out = {}
    for i, entry in enumerate(veneer_src):
        out[VENEER + 4 * i] = (entry[1] if entry[0] == "word" else branch_to(entry[1], entry[2]) if entry[0] == "branch"
                               else cexpr(entry[1], {"ticks": ticks}))
    out[SITE] = branch_to(*SITE_BRANCH)
    return out


failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


def plural(n, what):
    return f"{n} {what}{'' if n == 1 else 'es' if what.endswith('s') else 's'}"


print(f"game_patches_botw.c patch_job_poll_pause: LOOP main+{LOOP:#x}, SITE main+{SITE:#x} -> VENEER main+{VENEER:#x} ({NV} words), "
      f"EXIT main+{EXIT:#x}; {len(CHECK)} checked words; default pause {DEFAULT_NS} ns")
own = [(VENEER + 4 * i, e[1]) for i, e in enumerate(veneer_src) if e[0] == "branch"] + [(SITE, SITE_BRANCH[0])]
check(all(at == written for at, written in own) and len(own) > 1,
      f"C: each of the {len(own)} branch_to calls (veneer[] and the site) is computed for the address it is written at")

# 2. the image
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True


def dis(w, a):
    ins = list(md.disasm(struct.pack("<I", w), a))
    return ins[0] if ins else None


bad = [f"main+{off:#x} is {word(off):08x}, not {w:08x}" for off, w in CHECK.items() if word(off) != w]
check(not bad, f"image: the {len(CHECK)} check[] words are the shipped ones" + (": " + "; ".join(bad) if bad else ""))
if bad:
    raise SystemExit("not the image patch_job_poll_pause expects")
zero = not any(image[VENEER:VENEER_END])
fixups = image_path.parent / "main.fixups.tsv"
if fixups.exists():
    layout = {k: int(v, 0) for _, k, v in (line.split() for line in fixups.read_text().splitlines() if line.startswith("#"))}
    text_end, ro_off = layout["TEXT_END"], layout["RO_OFF"]
    rx_end = (text_end + 0x3fff) & ~0x3fff                     # loader.c: the text is r-x up to page_up(text_end)
    room = text_end <= VENEER and VENEER_END <= min(ro_off, rx_end)
    check(zero and room, f"image: main+{VENEER:#x}..{VENEER_END:#x} is zero padding after the text (TEXT_END {text_end:#x}, "
                         f"RO_OFF {ro_off:#x}, executable to {rx_end:#x})")
else:
    check(zero, f"image: main+{VENEER:#x}..{VENEER_END:#x} is zero (no main.fixups.tsv next to the image: its room against TEXT_END and "
                f"RO_OFF is not checked)")
excl = [off for off in CHECK if dis(word(off), off).mnemonic in ("ldxr", "ldxrb", "ldxrh", "ldxp")]
check(not excl, "image: no check[] word is a plain exclusive load, so the exclusive-acquire rewrite after this patch leaves them alone")

# 3. the poll's own instructions: the queue's layout, and what is dead at LOOP
EXIT_TARGET = dis(word(EXIT), EXIT).operands[0].imm            # EXIT is the poll's b out (where a taken batch goes)
SETUP = LOOP - 0x18                                            # zeroes w13 / w24, the lock address, the status values


def reg(ins, r):
    n = ins.reg_name(r)
    if n in ("wzr", "xzr"):
        return None
    if n in ("sp", "wsp"):
        return "sp"
    if not re.fullmatch(r"[wx]\d+", n):
        raise SystemExit(f"main+{ins.address:#x} {ins.mnemonic} {ins.op_str}: register {n} is not modelled")
    return "x" + n[1:]


ALU, ALU_S = {"mov", "add", "sub", "and", "orr", "eor", "lsl", "lsr", "asr", "neg", "mvn"}, {"adds", "subs", "ands", "negs"}
SELECT = {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg"}
LOADS = {"ldr", "ldrb", "ldrh", "ldrsw", "ldur", "ldxr", "ldxrb", "ldxrh", "ldaxr", "ldar"}
STORES, STORE_EX = {"str", "strb", "strh", "stur", "stlr"}, {"stxr", "stlxr", "stxrb", "stlxrb"}


def access(ins):
    """(registers read, registers written, touches memory) for the instruction kinds of the poll and the veneer; anything else
    stops the script (capstone 5's regs_access gets compares and the flags of cbz wrong on AArch64)."""
    m, ops = ins.mnemonic, ins.operands
    regs = [reg(ins, o.reg) if o.type == ARM64_OP_REG else None for o in ops]
    mem = [o.mem for o in ops if o.type == ARM64_OP_MEM]
    base = {reg(ins, x.base) for x in mem if x.base} | {reg(ins, x.index) for x in mem if x.index}
    r, w = set(), set()
    if m in ("cmp", "cmn", "tst"):
        r, w = set(regs), {"nzcv"}
    elif m in ("ccmp", "ccmn"):
        r, w = set(regs) | {"nzcv"}, {"nzcv"}
    elif m.startswith("b."):
        r = {"nzcv"}
    elif m in ("cbz", "cbnz", "tbz", "tbnz"):
        r = {regs[0]}
    elif m in SELECT:
        r, w = set(regs[1:]) | {"nzcv"}, {regs[0]}
    elif m in ALU | ALU_S:
        r, w = set(regs[1:]), {regs[0]} | ({"nzcv"} if m in ALU_S else set())
    elif m in LOADS:
        r, w = base, {regs[0]}
    elif m in STORES:
        r = {regs[0]} | base
    elif m in STORE_EX:
        r, w = {regs[1]} | base, {regs[0]}
    elif m == "mrs":
        w = {regs[0]}
    elif m not in ("b", "isb", "dmb", "dsb", "clrex", "nop"):
        raise SystemExit(f"main+{ins.address:#x} {m} {ins.op_str} is not modelled")
    if getattr(ins, "writeback", False):
        w |= base
    return r - {None}, w - {None}, bool(mem)


def is_branch(ins):
    return ins.mnemonic in ("b", "cbz", "cbnz", "tbz", "tbnz") or ins.mnemonic.startswith("b.")


def successors(ins):
    """The branch target first."""
    m, nxt = ins.mnemonic, ins.address + 4
    if m == "b":
        return [ins.operands[0].imm]
    if is_branch(ins):
        return [ins.operands[-1].imm, nxt]
    if m in ("ret", "br", "blr", "bl"):
        return [-1]
    return [nxt]


def first_use(code, start, regs):
    """None when every path from start writes each of regs before reading it, else why not (a read first, or leaving `code`
    with one of them unwritten)."""
    todo, seen = [(start, frozenset(regs))], set()
    while todo:
        pc, pending = todo.pop()
        if not pending or (pc, pending) in seen:
            continue
        seen.add((pc, pending))
        if pc not in code:
            return f"leaves for main+{pc:#x} with {', '.join(sorted(pending))} unwritten"
        ins = code[pc]
        r, w, _ = access(ins)
        if r & pending:
            return f"main+{pc:#x} ({ins.mnemonic} {ins.op_str}) reads {', '.join(sorted(r & pending))} first"
        todo += [(s, pending - w) for s in successors(ins)]
    return None


poll = {a: dis(word(a), a) for a in range(SETUP, EXIT_TARGET, 4)}
if None in poll.values():
    raise SystemExit("the poll does not decode")


@dataclass
class Layout:
    queue: str
    lock_reg: str
    lock_off: int
    total: int
    next: int
    flag: int
    status: str
    status_off: int
    tests: tuple
    unlock: int
    worker: str
    batch: int


def poll_layout():
    """The queue's fields from the poll's own instructions: the lock address made in the setup (add <lock>, <queue>, #off),
    every load and store through the queue register (the one byte read = the flag, the word loaded and stored = the next
    index, the other word loaded = the count), the status word stored through the third base register, the plain test loads
    of the lock and the exclusive load of the unlock (the one before the stlxr); the worker is the register compared with 0
    and the batch the one the csel picks against the jobs left."""
    adds = [i for a, i in poll.items() if a < LOOP and i.mnemonic == "add" and i.operands[2].type == ARM64_OP_IMM]
    if len(adds) != 1:
        raise SystemExit("the poll's setup does not make one lock address")
    lock_reg, queue, lock_off = reg(adds[0], adds[0].operands[0].reg), reg(adds[0], adds[0].operands[1].reg), adds[0].operands[2].imm
    fields, status, tests, release = {}, set(), [], None
    for a, i in poll.items():
        mem = [o.mem for o in i.operands if o.type == ARM64_OP_MEM]
        if not mem:
            continue
        b, d, first = reg(i, mem[0].base), mem[0].disp, i.reg_name(i.operands[0].reg)
        size = 1 if i.mnemonic.endswith("b") else 2 if i.mnemonic.endswith("h") else 4 if first.startswith("w") else 8
        if b == queue:
            f = fields.setdefault(d, {"size": size, "load": False, "store": False})
            f["load" if i.mnemonic in LOADS else "store"] = True
        elif b == lock_reg:
            if d:
                raise SystemExit(f"main+{a:#x} reaches the lock word at an offset")
            if i.mnemonic == "ldr":
                tests.append(a)
            if i.mnemonic == "stlxr":
                release = a
        else:
            status.add((b, d, size, i.mnemonic))
    flag = [d for d, f in fields.items() if f == {"size": 1, "load": True, "store": False}]
    nxt = [d for d, f in fields.items() if f == {"size": 4, "load": True, "store": True}]
    total = [d for d, f in fields.items() if f == {"size": 4, "load": True, "store": False}]
    if len(fields) != 3 or len(flag) != 1 or len(nxt) != 1 or len(total) != 1 or len(status) != 1 or len(tests) != 2 or not release:
        raise SystemExit(f"the poll's queue accesses are not the ones this script models: {fields}, {status}, {tests}")
    (status_reg, status_off, status_size, status_op), = status
    if status_size != 4 or status_op != "str":
        raise SystemExit(f"the poll's status store is {status_op} of {status_size} bytes")
    unlock = max(a for a, i in poll.items()
                 if a < release and i.mnemonic in ("ldxr", "ldaxr") and reg(i, i.operands[1].mem.base) == lock_reg)
    worker = [reg(i, i.operands[0].reg) for i in poll.values()
              if i.mnemonic == "cmp" and i.operands[1].type == ARM64_OP_IMM and i.operands[1].imm == 0]
    batch = [reg(i, i.operands[1].reg) for i in poll.values() if i.mnemonic == "csel"]
    if len(worker) != 1 or len(batch) != 1:
        raise SystemExit("the poll's worker test or batch choice is not the one this script models")
    return Layout(queue, lock_reg, lock_off, total[0], nxt[0], flag[0], status_reg, status_off, tuple(tests), unlock, worker[0], batch[0])


lay = poll_layout()
inputs = {r: first_use(poll, SETUP, {r}) for r in (lay.queue, lay.status, lay.worker, lay.batch)}
check(all(w and "reads" in w for w in inputs.values()) and first_use(poll, SETUP, {lay.lock_reg}) is None,
      f"poll layout from its instructions: queue {lay.queue}, lock word queue+{lay.lock_off:#x} (in {lay.lock_reg}; plain test loads at "
      f"{', '.join(f'main+{a:#x}' for a in lay.tests)}, unlock from main+{lay.unlock:#x}), count +{lay.total:#x}, next index +{lay.next:#x} "
      f"(loaded and stored), flag byte +{lay.flag:#x}, status word +{lay.status_off:#x} of {lay.status}; worker {lay.worker}, batch "
      f"{lay.batch} (all four read before written)")
DEAD = {"x14", "x15", "nzcv"}
why = first_use(poll, LOOP, DEAD)
controls = {r: first_use(poll, LOOP, {r}) for r in ("x13", lay.status)}
check(why is None and all(controls.values()),
      f"static: {', '.join(sorted(DEAD))} are dead at LOOP: every path from main+{LOOP:#x} writes them before reading them"
      + (f" -- not proven: {why}" if why else "") + "; the same walk does not prove " + ", ".join(f"{r} ({w})" for r, w in controls.items()))
if why:
    raise SystemExit("the veneer's scratch registers are not dead at LOOP")

# 4-6. Unicorn
OBJECTS = 0x7000_0000
QUEUE, STATUS = OBJECTS, OBJECTS + 0x1000
REGS = {**{f"x{i}": getattr(A, f"UC_ARM64_REG_X{i}") for i in range(31)}, "sp": A.UC_ARM64_REG_SP, "nzcv": A.UC_ARM64_REG_NZCV}
CNTVCT, CNTFRQ = (3, 3, 14, 0, 2), (3, 3, 14, 0, 0)            # op0, op1, CRn, CRm, op2
MAX_POLLS, MAX_STEPS = 40, 200000


@dataclass
class Scenario:
    what: str
    status: bool                  # a status word (x20 not null)
    worker: int                   # w0: worker 0 never waits
    batch: int                    # w22: the most jobs one pass takes
    total: int                    # the queue's count (+0x90)
    start: int = 0                # the next index (+0x94) at the first pass
    flag: int = 1                 # the byte at +0x98
    publish_at: int = -1          # at the start of this pass another worker sets the next index to `publish`
    publish: int = 0
    contend_at: int = -1          # at the start of this pass another worker holds the lock for `spins` test loads
    spins: int = 0
    wraps: bool = False           # the counter starts half a pause before 2^64

    def model(self):
        """(passes, first job, jobs taken, next index after, queue done) as this script reads the shipped poll."""
        k, nxt = 0, self.start
        while True:
            if k == self.publish_at:
                nxt = self.publish
            if self.worker == 0 or self.flag == 0 or (nxt ^ 0x80000000) - 0x80000000 > 0 or k == MAX_POLLS:
                break
            k += 1
        n = min(self.batch, (self.total - nxt) & 0xffffffff)   # csel ... lo: unsigned
        return k + 1, nxt, n, nxt + n, int(nxt + n >= self.total)


SCENARIOS = [
    Scenario("worker 1: 5 empty passes (the 3rd waits 4 test loads for another worker's lock), then jobs 0-2 of 8 taken elsewhere: takes 3-4",
             status=True, worker=1, batch=2, total=8, publish_at=5, publish=3, contend_at=2, spins=4),
    Scenario("worker 2, no status word (x20 = 0): 3 empty passes (the 2nd waits 3 test loads), then job 0 of 4 taken elsewhere: takes 1-3, the last",
             status=False, worker=2, batch=4, total=4, publish_at=3, publish=1, contend_at=1, spins=3),
    Scenario("worker 0 never waits: takes jobs 0-1 of 8 at the first pass", status=True, worker=0, batch=2, total=8),
    Scenario("jobs 0-5 of 8 already taken: worker 1 takes 6-7 at the first pass", status=True, worker=1, batch=4, total=8, start=6),
    Scenario("worker 3: 2 empty passes, then the queue drained by others (8 of 8): leaves with nothing",
             status=True, worker=3, batch=2, total=8, publish_at=2, publish=8),
    Scenario("the counter half a pause before 2^64: 2 empty passes, then job 0 of 2 taken elsewhere: takes 1",
             status=True, worker=1, batch=1, total=2, publish_at=2, publish=1, wraps=True),
]


def objects(sc):
    o = bytearray(b"\xa5" * 0x2000)
    struct.pack_into("<III", o, lay.lock_off, 0, 0, 0)
    struct.pack_into("<I", o, lay.total, sc.total)
    struct.pack_into("<I", o, lay.next, sc.start)
    o[lay.flag] = sc.flag
    struct.pack_into("<I", o, 0x1000 + lay.status_off, 0)
    return bytes(o)


def steps(ticks):
    """The fake counter's uneven steps per read: 1 .. ticks / 3."""
    x = 12345
    while True:
        x = (x * 1103515245 + 12345) & 0x7fffffff
        yield 1 + x % max(1, ticks // 3)


@dataclass
class Result:
    stop: object                  # None when the poll left at EXIT_TARGET
    polls: list                   # per pass: registers at its start, w14 at SITE, the veneer pcs run, the counter reads
    stores: list                  # (address, size, value), in order
    regs: dict                    # at the end
    memory: bytes                 # the queue and the status object at the end
    lock_held: list               # (pass, pc, lock word) for veneer instructions run with the lock word not 0
    entries: list                 # (from, to) for each entry into the veneer
    veneer_stores: int
    other_mrs: list               # system registers other than the counter and its frequency


def regs(uc):
    return {name: uc.reg_read(r) for name, r in REGS.items()}


def run(patch, sc, hz=0, ticks=1):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for page in sorted({p for lo, hi in ((SETUP, EXIT_TARGET + 4), (VENEER, VENEER_END)) for p in range(lo & ~0xfff, hi, 0x1000)}):
        mu.mem_map(page, 0x1000, UC_PROT_READ | UC_PROT_EXEC)  # only the poll's and the veneer's pages, not writable
        mu.mem_write(page, image[page:page + 0x1000])
    for off, w in patch.items():
        mu.mem_write(off, struct.pack("<I", w))
    mu.mem_map(OBJECTS, 0x2000)
    mu.mem_write(OBJECTS, objects(sc))
    for i in range(31):
        mu.reg_write(REGS[f"x{i}"], 0x5a00000000000000 | i << 40 | i * 0x01010101)
    for name, value in ((lay.queue, QUEUE), (lay.status, STATUS if sc.status else 0), (lay.worker, sc.worker), (lay.batch, sc.batch),
                        ("sp", OBJECTS + 0x1ff0), ("nzcv", 0x20000000)):
        mu.reg_write(REGS[name], value)
    lock = QUEUE + lay.lock_off
    st = {"pc": None, "contended": False, "spins": 0, "stop": None, "veneer_stores": 0,
          "counter": (1 << 64) - max(2, ticks // 2) if sc.wraps else 0x1234_5678_9abc}
    polls, stores, lock_held, entries, other = [], [], [], [], []
    step = steps(ticks)

    def code(uc, pc, size, _):
        prev, st["pc"] = st["pc"], pc
        back = prev is not None and VENEER <= prev < VENEER_END and LOOP < pc < EXIT_TARGET and pc != EXIT   # elsewhere than LOOP
        if pc == LOOP or back:                               # a pass starts
            k = len(polls)
            if k == MAX_POLLS:
                st["stop"] = f"still polling after {k} passes"
                uc.emu_stop()
                return
            polls.append({"regs": regs(uc), "veneer": [], "reads": []})
            if k == sc.contend_at:                           # another worker holds the lock
                uc.mem_write(lock, struct.pack("<I", 1))
                st["contended"], st["spins"] = True, 0
            if k == sc.publish_at:                           # another worker took jobs (under its lock, released by now)
                uc.mem_write(QUEUE + lay.next, struct.pack("<I", sc.publish))
        elif st["contended"] and pc in lay.tests:
            st["spins"] += 1
            if st["spins"] > sc.spins:
                uc.mem_write(lock, bytes(4))
                st["contended"] = False
        elif pc == SITE:                                     # the pass's result: w14 != 0 took a batch (shipped: cbz w14 loops)
            polls[-1]["took"] = uc.reg_read(A.UC_ARM64_REG_W14)
        if VENEER <= pc < VENEER_END:
            polls[-1]["veneer"].append(pc)
            held, = struct.unpack("<I", uc.mem_read(lock, 4))
            if held:
                lock_held.append((len(polls) - 1, pc, held))
            if prev is None or not VENEER <= prev < VENEER_END:
                entries.append((prev, pc))
        elif not SETUP <= pc <= EXIT_TARGET:
            st["stop"] = f"left the poll for main+{pc:#x}"
            uc.emu_stop()

    def store(uc, access, address, size, value, _):
        stores.append((address, size, value & ((1 << 8 * size) - 1)))
        st["veneer_stores"] += VENEER <= st["pc"] < VENEER_END

    def mrs(uc, rd, cp, _):
        key = (cp.op0, cp.op1, cp.crn, cp.crm, cp.op2)
        if key == CNTVCT:
            value = st["counter"]
            st["counter"] = (value + next(step)) & MASK64
        elif key == CNTFRQ:
            value = hz
        else:
            other.append((st["pc"], key))
            return 0
        polls[-1]["reads"].append((st["pc"], "cntvct_el0" if key == CNTVCT else "cntfrq_el0", value))
        uc.reg_write(rd, value)
        return 1

    mu.hook_add(UC_HOOK_CODE, code)
    mu.hook_add(UC_HOOK_MEM_WRITE, store)
    mu.hook_add(UC_HOOK_INSN, mrs, None, 1, 0, A.UC_ARM64_INS_MRS)
    try:
        mu.emu_start(SETUP, EXIT_TARGET, count=MAX_STEPS)
    except UcError as e:
        st["stop"] = f"{e} at main+{st['pc']:#x}"
    pc = mu.reg_read(A.UC_ARM64_REG_PC)
    if st["stop"] is None and pc != EXIT_TARGET:
        st["stop"] = f"at main+{pc:#x} after {MAX_STEPS} instructions"
    return Result(st["stop"], polls, stores, regs(mu), bytes(mu.mem_read(OBJECTS, 0x2000)), lock_held, entries,
                  st["veneer_stores"], other)


def taken(r):
    """(first job, jobs taken, next index, done) the way the poll leaves them: w25, w24, +0x94, w13."""
    nxt, = struct.unpack_from("<I", r.memory, lay.next)
    return r.regs["x25"] & 0xffffffff, r.regs["x24"] & 0xffffffff, nxt, r.regs["x13"] & 0xffffffff


SHIPPED = []
for n, sc in enumerate(SCENARIOS, 1):
    r = run({}, sc)
    SHIPPED.append(r)
    passes, first, count, nxt, done = sc.model()
    got = None if r.stop else (len(r.polls), *taken(r))
    check(r.stop is None and got == (passes, first, count, nxt, done) and (r.regs["x8"] - nxt + 1) & 0xffffffff == 0,
          f"shipped, scenario {n} ({sc.what}): {'stopped: ' + r.stop if r.stop else plural(len(r.polls), 'pass')} (model "
          f"{passes}); first job, taken, next index, done: {got and got[1:]} (model {(first, count, nxt, done)})")


def battery(patch, hz, ticks, label):
    """[(family, ok, line)] of every check of the patched words `patch` ({offset: word}) at a hz counter."""
    out = []

    def add(family, ok, line):
        out.append((family, ok, f"{label}: {line}"))
    words = [patch.get(VENEER + 4 * i, 0) for i in range(NV)]
    insns = [dis(w, VENEER + 4 * i) for i, w in enumerate(words)]
    site = dis(patch[SITE], SITE)
    add("site", site is not None and site.mnemonic == "b" and site.operands[0].imm == VENEER,
        f"the site main+{SITE:#x} is {site and site.mnemonic + ' ' + site.op_str}, a branch to the veneer main+{VENEER:#x}")
    problems = []
    for ins in insns:
        if ins is None:
            problems.append("an undefined word")
            continue
        try:
            r, w, mem = access(ins)
        except SystemExit as e:
            problems.append(str(e))
            continue
        if w - DEAD or mem:
            problems.append(f"main+{ins.address:#x} {ins.mnemonic} {ins.op_str} writes {sorted(w - DEAD)}{' / memory' if mem else ''}")
        for s in successors(ins) if is_branch(ins) else []:
            if s not in (LOOP, EXIT) and not VENEER <= s < VENEER_END:
                problems.append(f"main+{ins.address:#x} branches to main+{s:#x}")
    add("veneer", not problems, f"the veneer writes only {', '.join(sorted(DEAD))}, no memory, and branches only to LOOP, EXIT or into itself"
        + (": " + "; ".join(problems) if problems else ""))
    targets = {successors(ins)[0] for ins in insns if ins and is_branch(ins)}
    reads = [i for i, ins in enumerate(insns) if ins and ins.mnemonic == "mrs"]
    isb_ok = bool(reads) and all(ins.op_str.endswith("cntvct_el0") and i > 0 and insns[i - 1] and insns[i - 1].mnemonic == "isb"
                                 and VENEER + 4 * i not in targets for i, ins in ((i, insns[i]) for i in reads))
    add("isb", isb_ok, f"the veneer's {len(reads)} counter reads are each mrs cntvct_el0 straight after an isb, and no branch lands on one")
    adds = [ins for ins in insns if ins and ins.mnemonic == "add" and ins.operands[2].type == ARM64_OP_IMM]
    add("ticks", len(adds) == 1 and adds[0].operands[2].imm == ticks and adds[0].op_str.startswith("x15, x15"),
        f"the pause is {adds[0].mnemonic + ' ' + adds[0].op_str if len(adds) == 1 else 'not one add'}: {ticks} ticks of a {hz} Hz counter "
        f"({ticks * 1000000000 // hz} ns)")
    lock_ok, lock_lines, veneer_runs, waits, wrapped, takes = True, [], 0, [], [], []
    pause_problems, ran_veneer = [], []
    for n, (sc, ship) in enumerate(zip(SCENARIOS, SHIPPED), 1):
        r = run(patch, sc, hz, ticks)
        problems, dead_seen = [], set()
        if r.stop:
            problems.append("stopped: " + r.stop)
        else:
            if len(r.polls) != len(ship.polls):
                problems.append(f"{len(r.polls)} passes, shipped {len(ship.polls)}")
            if r.stores != ship.stores:
                i = next((i for i, (a, b) in enumerate(zip(r.stores, ship.stores)) if a != b), min(len(r.stores), len(ship.stores)))
                problems.append(f"{len(r.stores)} stores, shipped {len(ship.stores)}, first difference at store {i}")
            if r.memory != ship.memory:
                problems.append("the queue or the status word ends different")
            d = sorted(k for k in REGS if r.regs[k] != ship.regs[k])
            if d:
                problems.append(f"registers at the exit differ: {', '.join(d)}")
            for k, (a, b) in enumerate(zip(r.polls, ship.polls)):
                d = {x for x in REGS if a["regs"][x] != b["regs"][x]}
                dead_seen |= d & DEAD
                if d - DEAD:
                    problems.append(f"pass {k} starts with {', '.join(sorted(d - DEAD))} different")
                    break
        if problems:
            text = "; ".join(problems)
        else:
            first, count, nxt, _ = taken(r)
            jobs = f"takes jobs {first}-{first + count - 1}" if count > 1 else f"takes job {first}" if count else "takes nothing"
            text = (f"leaves at main+{EXIT_TARGET:#x} after {plural(len(r.polls), 'pass')} as shipped, {jobs} (next index {nxt}), the same "
                    f"{len(r.stores)} stores in order, the same registers at the exit; passes start different only in "
                    f"{', '.join(sorted(dead_seen)) or 'nothing'}")
        add("equal", not problems, f"scenario {n}: {text}")
        veneer_runs += sum(len(p["veneer"]) for p in r.polls)
        if r.lock_held or r.veneer_stores or any(f != SITE for f, _ in r.entries):
            lock_ok = False
            lock_lines.append(f"scenario {n}: lock word {r.lock_held[0][2] if r.lock_held else 0} at "
                              f"{', '.join(f'main+{pc:#x}' for _, pc, _ in r.lock_held[:2]) or '-'}, "
                              f"{r.veneer_stores} veneer stores, entered from {', '.join(f'main+{f:#x}' for f, _ in r.entries if f is not None)}")
        if len(ship.polls) > 1:
            ran_veneer.append(bool(r.entries))
        if r.other_mrs:
            pause_problems.append(f"scenario {n} reads system registers {r.other_mrs[:2]}")
        for k, p in enumerate(r.polls):
            names_read = {nm for _, nm, _ in p["reads"]}
            if names_read - {"cntvct_el0"}:
                pause_problems.append(f"scenario {n} pass {k} reads {sorted(names_read)}")
            vals = [v for _, nm, v in p["reads"] if nm == "cntvct_el0"]
            if "took" not in p:                           # stopped before the back-edge
                continue
            if p["took"]:                                 # took a batch (or found the queue done): must leave as shipped
                takes.append(len(p["veneer"]))
                if vals or len(p["veneer"]) > 2:
                    pause_problems.append(f"scenario {n} pass {k} took a batch, then ran {len(p['veneer'])} veneer instructions "
                                          f"and read the counter {len(vals)} times")
                continue
            if len(vals) < 2:
                pause_problems.append(f"scenario {n} pass {k} polls again after {len(vals)} counter reads")
                continue
            t0, last = vals[0], vals[-1]
            if t0 + ticks > MASK64:                       # the deadline wraps: the wait ends early, never hangs
                wrapped.append((last - t0) & MASK64)
                continue
            waits.append(last - t0)
            if last - t0 < ticks or any(v - t0 >= ticks for v in vals[1:-1]):
                pause_problems.append(f"scenario {n} pass {k}: reads {[v - t0 for v in vals]} ticks after the first, pause {ticks}")
    add("lock", lock_ok and veneer_runs > 0 and all(ran_veneer),
        f"the lock word queue+{lay.lock_off:#x} is 0 at all {veneer_runs} veneer instructions run, which store nothing and are entered only "
        f"from the site" + (": " + "; ".join(lock_lines) if lock_lines else "")
        + ("" if all(ran_veneer) else "; a scenario with empty passes never ran the veneer"))
    add("pause", not pause_problems and bool(waits),
        f"{len(waits)} empty passes polled again {min(waits, default=0)}..{max(waits, default=0)} ticks after their first counter read "
        f"(pause {ticks}, each at the first read past it)"
        + (f"; {len(wrapped)} with a deadline past 2^64 ended after {wrapped} ticks (no hang)" if wrapped else "")
        + f"; the {len(takes)} passes that took a batch read no counter and left after {max(takes, default=0)} veneer instructions at "
          f"most; cntfrq_el0 never read"
        + (": " + "; ".join(pause_problems[:3]) if pause_problems else ""))
    return out


for hz, label in ((1000000000, "1 GHz"), (24000000, "24 MHz")):
    ticks = c_ticks(DEFAULT_NS, hz)
    print(f"{label} (cntfrq_el0 {hz}): {ticks} ticks for the default {DEFAULT_NS} ns")
    for family, ok, line in battery(c_patch(ticks), hz, ticks, label):
        check(ok, line)
ADD_AT = VENEER + 4 * [e[0] for e in veneer_src].index("ticks")
enc = [(ns, hz, c_ticks(ns, hz), dis(c_patch(c_ticks(ns, hz))[ADD_AT], ADD_AT))
       for ns, hz in ((5000, 1000000000), (1, 24000000), (DEFAULT_NS, 1000000000), (1000000000, 24000000))]
check(all(ins and ins.mnemonic == "add" and ins.operands[2].imm == t for _, _, t, ins in enc),
      "the pause's add takes every clamped value: " + ", ".join(f"{ns} ns at {hz} Hz -> {plural(t, 'tick')} ({ins and ins.op_str})"
                                                                for ns, hz, t, ins in enc))

# negative controls: each must fail, in the family named
print("negative controls (1 GHz), each must fail:")
HZ, TICKS = 1000000000, c_ticks(DEFAULT_NS, 1000000000)
good = c_patch(TICKS)
ven = {off: dis(w, off) for off, w in good.items() if VENEER <= off < VENEER_END}
first = min(ven)
mrs_at = sorted(off for off, ins in ven.items() if ins and ins.mnemonic == "mrs")
lo_at = [off for off, ins in ven.items() if ins and ins.mnemonic == "b.lo"]
exits = [off for off, ins in ven.items() if ins and ins.mnemonic == "b" and ins.operands[0].imm == EXIT]
again = [off for off, ins in ven.items() if ins and ins.mnemonic == "b" and ins.operands[0].imm == LOOP]
shape = bool(ven[first] and ven[first].mnemonic == "cbz" and len(lo_at) == len(exits) == len(again) == 1 and mrs_at)
check(shape, "the veneer has what the negative controls change: the test (cbz), the exit, the counter reads, the b.lo wait, the branch back")
NEGATIVE = [] if not shape else [
    ("(a) the veneer's test inverted (cbnz)", {first: good[first] | 0x01000000}, "equal"),
    ("(b) the wait removed", {off: NOP for off in ven if off not in (first, exits[0], again[0])}, "pause"),
    ("(c) the site branching into the wait", {SITE: branch_to(SITE, exits[0] + 4)}, "pause"),
    ("(d) the veneer entered before the unlock", {lay.unlock: branch_to(lay.unlock, VENEER)}, "lock"),
    ("(e) no isb before the first counter read", {mrs_at[0] - 4: NOP}, "isb"),
    ("(f) the wait's comparison inverted (b.hs)", {lo_at[0]: good[lo_at[0]] & ~0xf | 0x2}, "pause"),
    ("(g) polling again at LOOP + 4 (skips the x20 test)", {again[0]: branch_to(again[0], LOOP + 4)}, "equal"),
]
for what, change, family in NEGATIVE:
    results = battery({**good, **change}, HZ, TICKS, what)
    failed = {}
    for fam, ok, line in results:
        if not ok:
            failed.setdefault(fam, []).append(line.split(": ", 2)[1].split(":")[0] if fam == "equal" else fam)
    check(family in failed, f"negative control {what}: fails as expected in '{family}'"
          + (f" ({', '.join(failed[family])})" if family == "equal" and family in failed else "")
          + f"; families failing: {', '.join(sorted(failed)) or 'none'}")

if failures:
    raise SystemExit(f"{len(failures)} check(s) failed")
print("job poll pause (1.9.0): all checks passed")

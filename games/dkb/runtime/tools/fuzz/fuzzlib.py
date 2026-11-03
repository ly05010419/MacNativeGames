"""The shared runner of the file-parser fuzz harnesses (dm/tools/fuzz/fuzz_*.py), and the replay of their regression corpus (dm/tools/tests/test_fuzz_corpus.py).

The parsers read files a player supplies (an NSP's RomFS, ExeFS and control data), so a malformed file must end in the parser's own rejection, never in another exception,
a hang or a huge allocation. Each harness builds its seeds in code (synthetic files, never anything from a game) and defines check(data): run the parser on the bytes, return
on a result or on its declared rejection, and let everything else out. run() here turns what comes out into a finding:
  - any exception check lets out (the parser's undeclared failures, and assertions a harness makes on a result);
  - Oversized: an allocation larger than allowed() for the input, caught before it is made (lz4.block.decompress's uncompressed_size, zlib.decompress's output and bytearray(n)
    in the modules guard() is given, inert outside run(); a MemoryError counts too, which the address-space limit set on Linux raises for the rest);
  - Timeout: one input running longer than TIMEOUT seconds (libFuzzer's -timeout under atheris; SIGALRM under the random engine and in the replay).
A finding's signature is <exception>@<file>:<caller>><function> of the innermost parser frames. dm/tools/fuzz/known.tsv lists the findings already recorded, one row per cause
(a signature pattern, its corpus inputs, its issue): fuzzing skips what the patterns match so it can find other causes, the replay runs their inputs and expects them to fail
until the parser is fixed and the row removed.

Usage: python3 dm/tools/fuzz/fuzz_<parser>.py [--time S] [--engine atheris|random] [--findings DIR] [libFuzzer options]
  atheris (pip install -r .github/requirements/fuzz.txt; on macOS it builds from source with a Clang that ships libFuzzer) guides the mutations by coverage; without it the
  random engine mutates the seeds blindly (no dependency). A finding is written to <findings>/crash-<sha1> and the run exits 1.
"""
import argparse
import atexit
import fnmatch
import hashlib
import importlib
import os
from pathlib import Path
import random
import shutil
import signal
import struct
import sys
import tempfile
import time
import traceback
import zlib

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
CORPUS = HERE / 'corpus'
KNOWN = HERE / 'known.tsv'
TIMEOUT = 10   # seconds for one input; the seeds take milliseconds
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools')]


class Oversized(BaseException):   # a BaseException, so a parser's own `except Exception` (lz4_shaders has one) cannot swallow it
    pass


class Timeout(BaseException):
    pass


class Finding(Exception):
    def __init__(self, signature, text):
        super().__init__(f'{signature}\n{text}'); self.signature, self.text = signature, text


_limit = [1 << 62]


def allowed(n):
    """The most a parser may allocate for an n-byte input: 16 MiB, or 256 bytes per input byte (LZ4's largest ratio is about 255) when that is more. zlib.decompress's output
    is held to it too, though deflate reaches about 1032: a parser inflates no further than the size its format declares"""
    return max(16 << 20, 256 * n)


def _check_size(n, what):
    if n > _limit[0]: raise Oversized(f'{what} of {n:#x} bytes for an input that allows {_limit[0]:#x}')


def _guarded_bytearray(*args, **kwargs):
    if args and isinstance(args[0], int): _check_size(args[0], 'bytearray')
    return bytearray(*args, **kwargs)


def guard(*modules):
    """Catch the allocations a malformed size can drive before they are made: bytearray(n) in these modules, every lz4.block.decompress (its uncompressed_size is allocated up
    front) and every zlib.decompress (its output, inflated first to one byte past the limit)"""
    for m in modules: m.bytearray = _guarded_bytearray
    lz4_block = importlib.import_module('lz4.block')
    if not getattr(lz4_block.decompress, 'fuzz_guarded', False):
        real = lz4_block.decompress
        def decompress(source, uncompressed_size=-1, **kwargs):
            _check_size(uncompressed_size, 'lz4.block.decompress')
            return real(source, uncompressed_size=uncompressed_size, **kwargs)
        decompress.fuzz_guarded = True
        lz4_block.__dict__['decompress'] = decompress   # (an attribute assignment pyright does not know the module has)
    if not getattr(zlib.decompress, 'fuzz_guarded', False):
        real_zlib = zlib.decompress
        def zlib_decompress(data, wbits=zlib.MAX_WBITS, bufsize=zlib.DEF_BUF_SIZE):   # inflated first to one byte past the limit; then the real call, for its result and errors
            try: _check_size(len(zlib.decompressobj(wbits).decompress(data, _limit[0] + 1)), 'zlib.decompress')
            except (zlib.error, ValueError): pass   # (a bad wbits too: the real call raises its own error)
            return real_zlib(data, wbits, bufsize)
        zlib_decompress.fuzz_guarded = True
        zlib.__dict__['decompress'] = zlib_decompress


class InputFile:
    """A path holding the current input, for parsers that take a path (rewritten for every input; its folder is removed at exit)"""
    def __init__(self, name): self.name, self.path = name, None

    def write(self, data):
        if self.path is None:
            d = tempfile.mkdtemp(prefix='nxfuzz-'); atexit.register(shutil.rmtree, d, True); self.path = os.path.join(d, self.name)
        with open(self.path, 'wb') as f: f.write(data)
        return self.path


def import_shader_split():
    """shader_split loads a game profile at import: any profile does (its readers do not depend on it); DM_GAME is put back after, as other tests expect no default game"""
    had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = had or '010015100B514000'
    try: return importlib.import_module('shader_split')
    finally:
        if had is None: del os.environ['DM_GAME']


class Broken(AssertionError):
    """A harness's assertion on a parser's result: where = the parser's <file>:<function>, which the signature takes"""
    def __init__(self, where, msg): super().__init__(msg); self.where = where


def broken(where, msg): return Broken(where, msg)


def signature(e):
    """<exception>@<file>:<caller>><function> of the two innermost frames in the parser's file (the repository outside this folder), else of the innermost frame"""
    if isinstance(e, Broken): return f'AssertionError@{e.where}'
    frames = traceback.extract_tb(e.__traceback__)
    inside = [f for f in frames if f.filename.startswith(str(REPO / 'dm')) and not f.filename.startswith(str(HERE))]
    last = (inside or frames)[-2:]   # the function and its caller in the parser: a shared helper (sph_stage, R.take) fails differently for each reader that calls it
    if len(last) == 2 and last[0].filename != last[1].filename: last = last[1:]
    where = f'{os.path.basename(last[-1].filename)}:' + '>'.join(f.name for f in last) if last else '?'
    t = type(e); name = t.__qualname__ if t.__module__ in ('builtins', __name__) else f'{t.__module__.lstrip("_")}.{t.__qualname__}'   # struct.error, not error
    return f'{name}@{where}'


def known(harness):
    """{signature pattern: [corpus files]} of the recorded findings of a harness (known.tsv: harness, signature pattern, corpus file, issue, what). A pattern (fnmatch) covers one
    cause: struct.error@uepak.py:*>take is every read of R past the end of its buffer, whichever table it reads"""
    out = {}
    for line in KNOWN.read_text().splitlines():
        if not line.strip() or line.startswith('#'): continue
        h, sig, name = line.split('\t')[:3]
        if h == harness: out[sig] = name.split(',')
    return out


def matches(sig, pattern):
    """A signature against a known.tsv pattern (alternatives separated by |); a pattern that leaves the exception open (*@...) never covers a hang or an oversized allocation"""
    if '|' in pattern: return any(matches(sig, p) for p in pattern.split('|'))
    if sig.startswith(('Oversized@', 'Timeout@')) and not pattern.startswith(sig.split('@')[0] + '@'): return False
    return fnmatch.fnmatchcase(sig, pattern)


def _alarm(signum, frame): raise Timeout(f'one input ran longer than {TIMEOUT} s')


def run(check, data, skip=(), alarm=False):
    """check(data) under the allocation limit; → None when it ended cleanly or in a known finding, else raises Finding"""
    _limit[0] = allowed(len(data))
    if alarm: old = signal.signal(signal.SIGALRM, _alarm); signal.alarm(TIMEOUT)
    try: check(data)
    except (Exception, SystemExit, Oversized, Timeout) as e:
        if isinstance(e, MemoryError): e = Oversized(f'MemoryError: {e}').with_traceback(e.__traceback__)
        sig = signature(e)
        if any(matches(sig, k) for k in skip): return
        raise Finding(sig, ''.join(traceback.format_exception(e))) from None
    finally:
        _limit[0] = 1 << 62
        if alarm: signal.alarm(0); signal.signal(signal.SIGALRM, old)


def replay(harness, check, data):
    """One corpus input, strictly (known findings fail too): raises Finding"""
    run(check, data, alarm=True)


# ---- the random engine: blind mutations of the seeds (no coverage), for machines without atheris
INTERESTING = (0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff)


def mutate(d, rng, pool):
    d = bytearray(d)
    for _ in range(rng.choice((1, 1, 2, 4, 8))):
        op, n = rng.randrange(7), len(d)
        if op == 0 and n: d[rng.randrange(n)] ^= 1 << rng.randrange(8)
        elif op == 1 and n: d[rng.randrange(n)] = rng.choice((0, 0xff, 0x7f, 0x80, rng.randrange(256)))
        elif op == 2 and n >= 4:   # a u32 field set to an interesting value, at an aligned offset (headers are word-aligned)
            o = rng.randrange(n // 4) * 4; v = rng.choice(INTERESTING + (n, n - 1, n + 1, rng.randrange(1 << 32)))
            d[o:o + 4] = struct.pack('<I', v & 0xffffffff)
        elif op == 3 and n: del d[rng.randrange(n):]   # cut short
        elif op == 4: d += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64)))
        elif op == 5 and n: a = rng.randrange(n); b = rng.randrange(a, min(n, a + 256) + 1); d[a:a] = d[a:b]
        elif op == 6:   # splice another input in
            o = rng.choice(pool); a = rng.randrange(len(o) + 1); b = rng.randrange(a, len(o) + 1); at = rng.randrange(n + 1); d[at:at + (b - a)] = o[a:b]
    return bytes(d)


def random_engine(check, pool, seconds, findings, skip, seed):
    rng, start, runs = random.Random(seed), time.monotonic(), 0
    while time.monotonic() - start < seconds:
        data = mutate(rng.choice(pool), rng, pool)
        try: run(check, data, skip, alarm=True)
        except Finding as f:
            path = save(findings, data)
            print(f.text, file=sys.stderr); print(f'==finding== {f.signature}: input written to {path}', file=sys.stderr)
            return 1
        runs += 1
    t = time.monotonic() - start
    print(f'Done {runs} runs in {t:.0f} second(s): {runs / max(t, 1e-9):.0f} exec/s (random engine)')
    return 0


def save(findings, data):
    os.makedirs(findings, exist_ok=True)
    path = os.path.join(findings, 'crash-' + hashlib.sha1(data).hexdigest())
    with open(path, 'wb') as f: f.write(data)
    return path


def main(harness, check, seeds):
    """The command line of a harness: fuzz check from the seeds and the harness's corpus"""
    ap = argparse.ArgumentParser(description=f'Fuzz the {harness} parser (dm/tools/fuzz/fuzzlib.py)')
    ap.add_argument('--time', type=int, default=60, help='seconds to fuzz (default 60)')
    ap.add_argument('--engine', choices=('auto', 'atheris', 'random'), default='auto', help='atheris when installed (auto), or the blind random engine')
    ap.add_argument('--findings', default=f'fuzz-findings/{harness}', help='folder for crashing inputs (default fuzz-findings/<parser>)')
    ap.add_argument('--seed', type=int, default=None, help='random engine: the RNG seed (default: time)')
    args, rest = ap.parse_known_args()
    pool = list(seeds().values())
    corpus = CORPUS / harness
    if corpus.is_dir(): pool += [p.read_bytes() for p in sorted(corpus.iterdir()) if p.is_file()]
    skip = set(known(harness))
    engine = args.engine
    if engine == 'auto':
        try: importlib.import_module('atheris'); engine = 'atheris'
        except ImportError: engine = 'random'
    print(f'{harness}: {len(pool)} seeds and corpus inputs, engine {engine}, {args.time} s, {len(skip)} known findings skipped', flush=True)
    if sys.platform == 'linux':   # the backstop for allocations guard() does not see: a MemoryError (macOS does not enforce RLIMIT_AS)
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (4 << 30, resource.getrlimit(resource.RLIMIT_AS)[1]))
    if engine == 'random':
        return random_engine(check, pool, args.time, args.findings, skip, args.seed if args.seed is not None else time.time_ns())
    atheris = importlib.import_module('atheris')
    work = tempfile.mkdtemp(prefix=f'nxfuzz-{harness}-')   # libFuzzer adds the inputs it finds to its first corpus folder: a scratch copy, never the repository
    for i, d in enumerate(pool): Path(work, f'seed-{i:03d}').write_bytes(d)
    os.makedirs(args.findings, exist_ok=True)
    argv = [sys.argv[0], f'-max_total_time={args.time}', f'-timeout={TIMEOUT}', '-rss_limit_mb=2048', f'-max_len={max(4096, 2 * max(map(len, pool)))}',
            f'-artifact_prefix={args.findings.rstrip("/")}/', *rest, work]
    atheris.instrument_all()
    def one(data):
        try: run(check, data, skip)
        except Finding as f:
            print(f'==finding== {f.signature}', file=sys.stderr); raise
    try:
        atheris.Setup(argv, one); atheris.Fuzz()
    finally: shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(__doc__)

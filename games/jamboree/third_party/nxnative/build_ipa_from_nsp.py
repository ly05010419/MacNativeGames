#!/usr/bin/env python3
# Unsigned IPA or a device-ready Xcode project straight from the NSPs: identify the game (title ID → game profile dm/games/<id>/) → extract (nx_extract) → rewrite the code modules (patch_nso.py)
#   → (a game that compiles shaders at run time: capture them, by booting it in the Mac host for a few seconds; an Unreal game: decode its shaders with its own Oodle, the same way)
#   → split out the shaders and check them against the profile's tables (shader_split.py) (a Vulkan game without SPIR-V files, Super Mario Galaxy, boots the same way to capture the SPIR-V shaders it generates;
#   a game with Trinity packs, Pokémon Legends: Z-A, has them decoded for the split by its own Oodle decoder, run in the Mac host's tool mode)
#   → iOS shader libraries (dmsh_build.py) → build_ipa.sh. A Vulkan game (main imports Vulkan, not NVN: Hades) skips the NVN shader steps: vk_shaders.py translates its SPIR-V shaders instead.
#   Which of these steps a game needs follows from its files (dm/games/profile_guess.py shader_sources), not from its profile. A game with no matching profile is surveyed (<NSP folder>/<title ID>_survey/survey.md), then gets a provisional profile guessed from
#   the survey (<NSP folder>/<title ID>_build/profile/<title ID>/, outside the repository, found through DM_GAMES_DIR) and a best-effort IPA that may not boot; SURVEY_ONLY / --survey-only stops after the survey.
# PLATFORM = 'mac' / --platform mac is a development option (the Mac is the development host, not a platform for players): the same steps up to the shaders, but only the Mac's
#   (the shader build without IOSOUT, a Vulkan or GL game's libraries without --ios), then the Mac host for the profile and the button-icon font; no iOS shaders, Xcode project or IPA.
#   The extracted files are kept whatever KEEP_EXTRACTED says (the game runs from them), and the last line is the command that runs the game in the Mac host, its save in <id>_build/run/save.
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below; an empty NSP folder or prod.keys is asked for at run time);
#   or in a terminal: python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> [options] (values given on the command line override "Settings", -h lists every option;
#   without keyboard input a missing value is not asked for but reported as an error; exit code 0 on success, 1 on error, 130 when interrupted). Command-line options entered in Spyder's run configuration work the same way.
# Uses only three things: this repository, a folder holding one game's NSPs (the base game, with or without an update; DLC NSPs, when present, are packed too and unlock the add-on content already in the game), and prod.keys.
#   The newest version in the folder is packed. A version the profile registers (with every version in its shader_versions in the folder) is built from the profile as it is; any other
#   version (the base game alone, another update) from a profile derived from it in <id>_build/profile: the same settings, and the version's own shaders (derive_profile).
# Every intermediate file is under <id>_build/ in the NSP folder: extract/ (the extracted game files, kept for the next build unless KEEP_EXTRACTED / --no-keep-extracted), aoc/ (each DLC's RomFS by index), tools/ (nx_extract, and the Mac host for a run-time shader capture), patched_*/, shader/ (shader package and caches, kept so the next build is fast), logs/ (each step's full output), tmp/, ipa/. The finished IPA also goes to the NSP folder by default.
# The DLC, the rewrite, the shader split and the iOS (or Vulkan) shaders are reused when all their inputs are as at their last successful run (the game files, the tools, the profile, Xcode and spirv-cross: fingerprint() keys beside their output);
#   the iOS shaders' inputs are the shader package's content and the chain's own tools, so a split that runs again and writes the same package leaves them reused.
#   The chain version header dm/gfx/nvn/dmsh_chain.h (one version for every game) is written again when a chain change was committed without it (chain_header: it follows from the source alone),
#   so a run may leave it changed. The c1 / uniform extent tables (c1_full.tsv, cb_extent.tsv) are not in the repository: the iOS shader step's dmsh_build.py makes them in the profile folder from
#   this build's shaders, where git ignores them, and a reused step makes them again when they do not name its libraries' stamp (chain_tables).
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin (the build whose fingerprints match dmsh_chain.lock; a rebuilt dll has different fingerprints and dmsh_build refuses it; its launcher is universal, so an Intel Mac runs it too). The Metal compiler and spirv-cross may be other than the lock's: the iOS shaders
#   are built through dm/gfx/tools/own_compilers.py, which says so in one line and leaves the lock and the chain version as they are.
# The Python running this file needs lz4, capstone and fonttools (and backports.zstd before Python 3.14); also needed: Xcode, xcodegen, .NET SDK 10, zstd, spirv-cross. A first run takes 2 (Metroid Dread) to 10 minutes (Breath of the Wild: 5 of them the shader split), longer when shader libraries must be compiled; a repeat run with unchanged inputs takes about 20 s (Metroid Dread), mostly packing the IPA. Key values are never printed.
# The console shows one line per stage ([k/n] stage, with a bar, done/total, percent, elapsed time and ETA where the work can be counted); the tools' own output goes only to logs/ (PROGRESS / --progress). --progress json writes newline-delimited JSON events to stdout.
import ast, collections, ctypes, getpass, hashlib, json, os, re, shlex, shutil, struct, subprocess, sys, tempfile, threading, time, types, zlib
from pathlib import Path

# ---- Settings ----
NSP_DIR = ''                            # folder holding the NSPs (empty = ask at run time)
PROD_KEYS = ''                          # path of prod.keys (empty = ask at run time)
PACKAGE = ''                            # version packed into the IPA (empty = the newest in the folder; Mario 'v327680' i.e. 1.2.1, or 'v0')
OUT_IPA = ''                            # output IPA (empty = <the profile's ipa> in the NSP folder)
APP_NAME = ''                           # app name on the home screen (empty = the game name in the base NSP)
BUNDLE_ID = ''                          # the app's bundle ID (empty = saved per-game Xcode ID, else org.nxnative.<base game title ID in lowercase>)
PREPARE_XCODE = False                   # prepare a per-game Xcode project and add it to the workspace instead of packing an IPA; requires KEEP_EXTRACTED
PLATFORM = 'ios'                        # 'ios' = the IPA (or the Xcode project); 'mac' = development: build only what running the game on this Mac needs (Mac shaders, the Mac host), no iOS shaders,
                                        #   Xcode project or IPA; keeps the extracted files and prints the command that runs the game (needs Apple silicon)
TEAM_ID = ''                            # optional Apple team ID; empty = DM_TEAM_ID environment, saved Xcode selection, or select in Xcode
XCODE_WORKSPACE = ''                    # shared .xcworkspace (empty = NxNative.xcworkspace beside the game's NSP folder)
LANGUAGE = ''                           # game language, a code or a name: 'ja', 'fr', 'de', 'es', 'zh-Hans', 'zh-Hant', 'ko', 'Japanese', ... (empty = English); it must be one the game supports (the console line lists them);
                                        #   DM_LANG=<code> in the app's Documents/env.txt changes it without a rebuild
KEEP_EXTRACTED = True                   # keep the extracted game files after the IPA is built (saves extraction next time; as big as the game files); False deletes them
DM_ENV_FILE = ''                        # optional: environment switches for a diagnostic build (one KEY=VALUE per line)
SHOW_FPS = True                         # True shows the game's frame rate on the iPhone (frames on screen per second, top left); DM_SHOW_FPS=0 / 1 in the app's Documents/env.txt changes it without a rebuild
SURVEY_ONLY = False                     # a game with no profile in dm/games: only write the survey, no provisional profile and no IPA
PROFILE_OUT = ''                        # where a provisional profile goes (empty = <NSP folder>/<title ID>_build/profile; the profile is <this>/<title ID>/)
ALLOW_DAMAGED = False                   # a damaged NSP (files failing the NCA's hash check: an incomplete copy or a broken download) stops the build; True writes those files as read and goes on
PROVISIONAL = False                     # build from a provisional profile even when dm/games has one for the game (to test the guesses; the profile goes to PROFILE_OUT / <id>_build/profile)
HEAP_CHECK = True                       # a provisional profile whose game has virtual address memory and sets a heap: boot it on the Mac until its heap request (a few seconds) to size the heap region
HEAP_CHECK_SECS = 30                    # the longest heap check (it stops at the first heap request, or HEAP_CHECK_NNMAIN_SECS into nnMain without one)
HEAP_CHECK_NNMAIN_SECS = 5

PROGRESS = 'auto'                       # stage lines on the console: 'bar' (one line per stage, redrawn in place), 'lines' (a plain line every BEAT seconds), 'off' (only each stage's start and end); 'auto' = bar in a terminal or Spyder, lines when the output goes to a file; 'json' = one JSON event per line, for a program that shows the build
BEAT = 30                               # seconds between progress lines in 'lines' mode
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet', os.path.expanduser('~/.dotnet')]   # GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def clock():
    return time.strftime('%H:%M:%S')


def say(msg):
    PROG.say(f'[{clock()}] {msg}')


def hms(s):   # seconds → m:ss, or h:mm:ss from an hour on
    s = int(s); h, m = divmod(s // 60, 60)
    return f'{h}:{m:02d}:{s % 60:02d}' if h else f'{m}:{s % 60:02d}'


def size_unit(v):   # (unit, divisor) for a byte count
    return next(((u, d) for u, d in (('GB', 2**30), ('MB', 2**20)) if v >= d), ('KB', 2**10))


def amount(v, unit=''):   # a count, or a byte size when unit is 'B'
    if unit == 'B':
        u, d = size_unit(v); return f'{v / d:.1f} {u}'
    return f'{v:,}' + (f' {unit}' if unit else '')


# ---- Progress on the console ----
class Progress:
    """The console view of the build: one line per stage, "[k/n] stage · step", with a bar, done/total, percent, elapsed time and ETA when the work can be counted, else a spinner and the elapsed time.
    Modes: bar = the line is redrawn in place (a terminal, Spyder's console) and replaced by a ✓ line when the stage ends; lines = a plain line when a stage or step starts, every BEAT seconds and at the end
    (for output piped to a file); off = only each stage's start and end lines. Counters come from set(), or from a poll function that runs in a thread of its own (it may walk a big folder) and returns set()'s arguments.
    Messages go through say(), which prints them above the live line. json (for a program that shows the build) = one JSON object per stdout line instead: the events stage, step, progress, stage_end and say;
    a count change goes out at most every JSON_INTERVAL seconds (the ticker sends one held back), any other change at once"""
    SPIN, BLOCKS, POLL, JSON_INTERVAL = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏', ' ▏▎▍▌▋▊▉', 1.0, 0.5

    def __init__(self, mode='off'):
        self.mode, self.lock, self.k, self.n, self.cur, self.shown, self.ticker, self.tstop = mode, threading.RLock(), 0, 0, None, 0, None, None

    def plan(self, n):   # the next n stages are numbered 1..n
        with self.lock: self.k, self.n = 0, n

    def add(self, n):   # n more stages than planned (one that turns out to be needed: a provisional profile's heap check)
        with self.lock: self.n += n

    def stage(self, name, poll=None, step='', **kw):
        """Start the next stage, ending the current one. kw: set()'s arguments"""
        with self.lock:
            self.end()
            self.k += 1; now = time.time()
            self.cur = types.SimpleNamespace(name=name, step=step, unit='', total=None, done=None, est=False, note='', t0=now, hist=collections.deque(), beat=now, pstop=None, jlast=None, jstate=None)
            if self.mode == 'json':
                self._json({'ev': 'stage', 'k': self.k, 'n': self.n, 'name': name, 'step': step, 't': round(now, 3)})
            elif self.mode != 'bar': self._out(f'[{clock()}] [{self.k}/{self.n}] {name}' + (f' · {step}' if step else '') + ' …')
            self.set(**kw)
            if self.mode != 'off' and not self.ticker:
                self.tstop = stop = threading.Event()
                self.ticker = threading.Thread(target=self._tick, args=(stop,), daemon=True); self.ticker.start()
        if poll: self.watch(poll)

    def watch(self, poll):
        """Call poll() over and over until the stage ends or watch() is called again (None: stop); poll returns set()'s arguments, or None to leave the line as it is"""
        with self.lock:
            st = self.cur
            if not st: return
            if st.pstop: st.pstop.set()
            st.pstop = stop = threading.Event()
        if poll: threading.Thread(target=self._poll, args=(st, poll, stop), daemon=True).start()

    def set(self, done=None, total=None, step=None, unit=None, est=None, note=None):
        """done / total: the count of the current step (unit 'B' = bytes); est: total is an estimate (shown with ~); note: shown after the count;
        step: the step within the stage (a new step starts with no count and restarts the ETA)"""
        with self.lock:
            st = self.cur
            if not st: return
            if step is not None and step != st.step:
                st.step, st.done, st.total, st.unit, st.est, st.note, st.beat = step, None, None, '', False, '', time.time(); st.hist.clear()
                if self.mode == 'lines': self._out(f'[{clock()}] [{self.k}/{self.n}] {st.name} · {step} …')
                if self.mode == 'json': self._json({'ev': 'step', 'k': self.k, 'name': st.name, 'step': step})
            if unit is not None: st.unit = unit
            if total is not None: st.total = total
            if est is not None: st.est = est
            if note is not None: st.note = note
            if done is not None:
                st.done = done; now = time.time(); st.hist.append((now, done))
                while len(st.hist) > 2 and st.hist[1][0] < now - 60: st.hist.popleft()   # the ETA follows the rate of the last minute
            if self.mode == 'json': self._json_progress(st)

    def _json_progress(self, st, force=False):   # a progress event when anything changed; a count change alone waits JSON_INTERVAL unless forced (the caller holds the lock)
        state = (st.step, st.total, st.unit, st.est, st.note, st.done)
        if state == st.jstate: return
        now = time.monotonic()
        if not force and st.jstate is not None and state[:-1] == st.jstate[:-1] and now - st.jlast < self.JSON_INTERVAL: return
        st.jlast, st.jstate = now, state
        self._json({'ev': 'progress', 'k': self.k, 'name': st.name, 'step': st.step, 'done': st.done, 'total': st.total,
                    'unit': st.unit, 'est': st.est, 'note': st.note,
                    'frac': min(st.done / st.total, 0.99) if st.total and st.done is not None else None,
                    'eta': self._eta(st), 'elapsed': round(time.time() - st.t0, 3)})

    def _json(self, obj):   # one JSON object per stdout line (a message's newlines are escaped)
        obj.setdefault('t', round(time.time(), 3))
        print(json.dumps(obj, ensure_ascii=False, allow_nan=False), flush=True)

    def end(self, summary='', ok=True):
        """End the current stage with a ✓ (or ✗) line: its elapsed time and a summary"""
        with self.lock:
            st, self.cur = self.cur, None
            if not st: return
            if st.pstop: st.pstop.set()
            if self.mode == 'json':
                self._json_progress(st, force=True)   # the last count, if it was held back
                self._json({'ev': 'stage_end', 'k': self.k, 'name': st.name, 'ok': ok, 'summary': summary, 'elapsed': round(time.time() - st.t0, 3)})
                return
            self._out(f'[{clock()}] {"✓" if ok else "✗"} [{self.k}/{self.n}] {st.name} ({hms(time.time() - st.t0)})' + (f': {summary}' if summary else ''))

    def say(self, msg):
        with self.lock:
            if self.mode == 'json': self._json({'ev': 'say', 'msg': msg})
            else: self._out(msg)

    def close(self, ok=True):   # end the current stage and stop the ticker (the mode stays: a message after it is still JSON in json mode)
        with self.lock:
            self.end(ok=ok)
            if self.tstop: self.tstop.set()
            self.ticker = self.tstop = None

    def _count(self, st):   # "1.9/~5.0 GB", "1,203/19,592 shaders", "2,301 files" (no total); '' without a count
        if st.done is None: return ''
        if not st.total: return amount(st.done, st.unit)
        t = '~' if st.est else ''
        if st.unit == 'B':
            u, d = size_unit(st.total); return f'{st.done / d:.1f}/{t}{st.total / d:.1f} {u}'
        return f'{st.done:,}/{t}{st.total:,}' + (f' {st.unit}' if st.unit else '')

    def _eta(self, st):   # seconds left at the rate of the last minute; None while unknown
        if not st.total or len(st.hist) < 2: return None
        (t1, d1), (t2, d2) = st.hist[0], st.hist[-1]
        if t2 - t1 < 3 or d2 <= d1: return None
        return max(st.total - st.done, 0) * (t2 - t1) / (d2 - d1)

    def _fields(self, st):   # (head, fraction done or None, count, note, times)
        head = f'[{self.k}/{self.n}] {st.name}' + (f' · {st.step}' if st.step else '')
        frac = min(st.done / st.total, 0.99) if st.total and st.done is not None else None   # 100 % only when the stage ends
        eta = self._eta(st)
        return head, frac, self._count(st), st.note, hms(time.time() - st.t0) + (f' · ETA {hms(eta)}' if eta is not None else '')

    def _line(self, st, width):
        head, frac, count, note, times = self._fields(st)
        head = f'{self.SPIN[int(time.time() * 8) % len(self.SPIN)]} {head}'
        pct = f'{frac * 100:.0f}%' if frac is not None else ''
        for note in (note, ''):   # a narrow console loses the note first, then the bar
            rest = '  '.join(x for x in (' · '.join(x for x in (count, note) if x), times) if x)
            barw = min(20, width - len(f'{head}  {pct:>4}  {rest}') - 3)   # the bar takes what the console's width leaves, up to 20 cells; none below 8
            if frac is None or barw >= 8: break
        if frac is not None and barw >= 8:
            cells = frac * barw; full = int(cells); part = int((cells - full) * 8)
            pct = ('█' * full + (self.BLOCKS[part] if part else '')).ljust(barw, '░') + f' {pct:>4}'
        return '  '.join(x for x in (head, pct, rest) if x)[:width - 1]

    def _out(self, text):   # a whole line above the live one (the caller holds the lock)
        if self.shown: sys.stdout.write('\r' + ' ' * self.shown + '\r'); self.shown = 0
        print(text, flush=True)
        if self.mode == 'bar' and self.cur: self._draw()

    def _draw(self):
        line = self._line(self.cur, shutil.get_terminal_size((80, 24)).columns)   # Spyder has no size to ask, and a line that wraps there cannot be redrawn: 80 columns
        sys.stdout.write('\r' + line + ' ' * max(self.shown - len(line), 0)); sys.stdout.flush()
        self.shown = len(line)

    def _tick(self, stop):
        while not stop.wait(0.25 if self.mode == 'bar' and not in_ipython() else 1):   # Spyder's console gets fewer redraws
            with self.lock:
                if stop.is_set(): return   # close() stopped this ticker while it waited for the lock
                st = self.cur
                if not st: continue
                try:
                    if self.mode == 'bar': self._draw()
                    elif self.mode == 'json': self._json_progress(st)   # a count that set() held back
                    elif self.mode == 'lines' and time.time() - st.beat >= BEAT:
                        st.beat = time.time(); head, frac, count, note, times = self._fields(st)
                        self._out(f'[{clock()}] {head}: ' + ', '.join(x for x in (f'{frac * 100:.0f}%' if frac is not None else '', count, note, times) if x))
                except Exception: pass   # progress never stops the build, and one bad line does not stop the next

    def _poll(self, st, poll, stop):
        while not stop.is_set():
            t, step = time.time(), st.step
            try: r = poll()
            except Exception: r = None   # progress never stops the build
            with self.lock:   # a count read for a step that ended meanwhile is dropped
                if r and self.cur is st and not stop.is_set() and ('step' in r or st.step == step): self.set(**r)
            stop.wait(max(self.POLL, (time.time() - t) * 5))   # a slow poll (a walk over tens of thousands of files) runs less often


PROG = Progress()


def progress_mode(want):   # auto: redraw in a terminal or in Spyder (IPython handles \r), plain lines when the output goes to a file or a pipe
    if want in ('bar', 'lines', 'off', 'json'): return want
    if want != 'auto': raise BuildError(f'PROGRESS / --progress must be auto, bar, lines, json or off: {want}')
    return 'bar' if in_ipython() or (sys.stdout and sys.stdout.isatty()) else 'lines'


def tree_size(d):   # (files, bytes) under d; (0, 0) when it does not exist yet
    n = b = 0; todo = [str(d)]
    while todo:
        try: it = os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                try:
                    if e.is_dir(follow_symlinks=False): todo.append(e.path)
                    else: n += 1; b += e.stat(follow_symlinks=False).st_size
                except OSError: pass
    return n, b


def in_ipython():   # Spyder (and Jupyter) run this file inside IPython: input works there, and sys.exit must not be called (it piles an error onto the console)
    import builtins
    return hasattr(builtins, '__IPYTHON__')


LANG_NEAR = {'en-US': 'en-GB', 'en-GB': 'en-US', 'fr': 'fr-CA', 'fr-CA': 'fr', 'es': 'es-419', 'es-419': 'es', 'pt': 'pt-BR', 'pt-BR': 'pt'}   # the same language for another region


def language_code(s):
    """A game language as typed (a code such as ja / zh-Hans, any case, _ for -, or its name such as Japanese) → its language code (nacp.LANGUAGES); None when it is none of them"""
    k = s.strip().lower().replace('_', '-')
    for c, n in nacp.LANGUAGES:
        if k in (c.lower(), n.lower()): return c
    return {'en': 'en-US', 'english': 'en-US', 'zh-cn': 'zh-Hans', 'zh-tw': 'zh-Hant'}.get(k)   # the settings' older codes for Chinese


def game_language(want, asked, have):
    """The language code to pack: want when the game supports it, else the same language for another region (en-GB for en-US, es for es-419, ...); have: the game's languages (empty: unknown, want as is).
    A language the game lacks stops the build when it was asked for; English, the default, then falls back to the game's first language"""
    if not have or want in have: return want
    if LANG_NEAR.get(want) in have: return LANG_NEAR[want]
    if asked: raise BuildError(f'the game has no {want} ({dict(nacp.LANGUAGES)[want]}); its languages: {language_list(have)} (--language / LANGUAGE)')
    return have[0]


def language_list(codes):
    names = dict(nacp.LANGUAGES)
    return ', '.join(f'{c} ({names[c]})' for c in codes)


def ask(prompt, what):   # what: what is missing, for the error message
    if PROG.mode == 'json': raise BuildError(f'{what} not given: JSON mode does not prompt; give it on the command line (-h for usage)')
    if not in_ipython() and not (sys.stdin and sys.stdin.isatty()):
        raise BuildError(f'{what} not given and there is no keyboard input to ask for it: give it on the command line (-h for usage)')
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, on_line=None):
    """Run a subprocess with all its output written to log (the console shows the stage line instead: the shader build alone emits tens of thousands of compiler warnings);
    stop on a non-zero exit, printing the log's last lines. on_line: called with every output line (build_ipa.sh's own step lines move its stages on)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail = collections.deque(maxlen=40)
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
    try:
        with open(log, 'w') as f:
            for line in p.stdout:
                f.write(line); tail.append(line)
                if on_line:
                    try: on_line(line.rstrip('\n'))
                    except Exception: pass   # progress never stops the build
        p.wait()
    except KeyboardInterrupt:
        p.terminate(); p.wait(); raise
    finally:
        p.stdout.close()
    if p.returncode:
        PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(tail).rstrip('\n'))
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        raise BuildError(f'{tool} exit code {p.returncode} (full output: {log})')


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def rmtree(p):
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p)


def build_nx_extract(repo, work, logs, env):
    """Build nx_extract into <work>/tools/nx_extract/. It is built in a temporary folder and copied over: with an apostrophe in the output path (e.g. "Bowser's Fury") MSBuild's conditions break,
    LibHac.dll and the executable are not copied, and the build still reports success"""
    nx = work / 'tools/nx_extract/nx_extract'
    feed = Path(env.get('DM_NUGET_SOURCE', ''))
    if not env.get('DM_NUGET_SOURCE') or not (feed / 'libhac.0.19.0.nupkg').is_file():
        raise BuildError('offline LibHac package missing: use build_jamboree.py --libhac-package (see README.md)')
    project = repo / 'dm/tools/nx_extract'
    with tempfile.TemporaryDirectory(prefix='nx_extract_') as t:
        run(['dotnet', 'restore', project, '--configfile', project / 'NuGet.Config',
             '--source', feed, '-p:NuGetAudit=false', '-p:RestoreIgnoreFailedSources=false',
             '--force', '-v', 'minimal', '-nologo'], logs / 'nx_extract_restore.log', env=env)
        run(['dotnet', 'build', project, '--no-restore', '-c', 'Release', '-o', t, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
        if not (Path(t) / 'nx_extract').exists(): raise BuildError(f'nx_extract built without an executable (see {logs / "nx_extract_build.log"})')
        rmtree(nx.parent); shutil.copytree(t, nx.parent)
    return nx


def swap_in(tmp, dst):   # swap the new output in as a whole (the old one is untouched on failure)
    rmtree(dst); tmp.rename(dst)


def free_gb(p):
    while not p.exists(): p = p.parent
    return shutil.disk_usage(p).free / 2**30


# ---- NSP identification ----
def pfs0_names(path):   # NSP = PFS0: list the file names inside
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != b'PFS0': return []
        n, st = struct.unpack_from('<II', h, 4)
        ent, strs = f.read(n * 0x18), f.read(st)
    return [strs[o:strs.index(b'\0', o)].decode(errors='replace') for o in (struct.unpack_from('<I', ent, i * 0x18 + 0x10)[0] for i in range(n))]


def title_of(path):   # the ticket name (the first 16 digits of the rights ID = title ID), else [title ID] in the file name
    for n in pfs0_names(path):   # the ticket first: a file name's tag can be wrong (FFT's base NSP is tagged …0001, its ticket is …0000)
        if re.match(r'[0-9A-Fa-f]{32}\.tik$', n): return int(n[:16], 16)
    m = re.search(r'\[([0-9A-Fa-f]{16})\]', path.name)
    return int(m.group(1), 16) if m else None


def version_of(path):
    m = re.search(r'\[v(\d+)\]', path.name)
    return int(m.group(1)) if m else None


def scan_nsps(nsp_dir):
    """Group by game: {application title ID: {'base': path, 'updates': [(version, path)], 'dlc': [(title ID, version, path)]}}"""
    fams, unknown = {}, []
    for p in sorted([*nsp_dir.glob('*.nsp'), *nsp_dir.glob('*.xci')]):   # a base game may be a cartridge dump (.xci: Monster Hunter Rise); nx_extract reads its secure partition
        if p.stat().st_size == 0:   # an empty file left by an incomplete download (the Zelda BotW folder holds a 0-byte DLC NSP next to the complete (1).nsp)
            say(f'skipping an empty file: {p.name}'); continue
        tid = title_of(p)
        if tid is None: unknown.append(p); continue
        low = tid & 0xFFF   # base …000, update …800, DLC = base + 0x1000 + index
        app = tid if low == 0 else tid & ~0xFFF if low == 0x800 else (tid & ~0xFFF) - 0x1000
        f = fams.setdefault(app, {'base': None, 'updates': [], 'dlc': []})
        if low == 0: f['base'] = p
        elif low == 0x800: f['updates'].append((version_of(p), p))
        else: f['dlc'].append((tid, version_of(p), p))
    if len(fams) == 1 and len(unknown) == 1 and next(iter(fams.values()))['base'] is None:   # a base game converted from a cartridge may have neither tags nor a ticket; build IDs are checked afterwards
        next(iter(fams.values()))['base'] = unknown[0]
    if not fams: raise BuildError(f'no NSP with a readable title ID in {nsp_dir}')
    if len(fams) > 1: raise BuildError('more than one game in the folder, keep one game per folder: ' + ', '.join(f'{t:016X}' for t in fams))
    app, fam = next(iter(fams.items()))
    if fam['base'] is None: raise BuildError(f'no base game NSP found in {nsp_dir} (title ID {app:016X})')
    return app, fam


def vnum(ver):   # 'v655360' → 655360
    return int(ver[1:])


def nsp_versions(fam, builds):
    """The versions the folder holds: {version: its update NSP, or None for v0 (the base game alone)}. An update is known by the [v<N>] tag in its file name; a single update without one
    is taken for the newest update the profile registers (its build IDs are checked once extracted), and is left out when the profile registers none or other updates are there"""
    out = {'v0': None}
    for v, p in fam['updates']:
        if v is not None: out[f'v{v}'] = p
    loose = [p for v, p in fam['updates'] if v is None]
    reg = sorted((b for b in builds if b != 'v0'), key=vnum)
    if loose and len(fam['updates']) == 1 and reg: out[reg[-1]] = loose[0]
    elif loose: say(f'[RED] leaving out update NSPs without a version tag ([v<N>]) in their name: {", ".join(p.name for p in loose)}')
    return out


def pick_nsps(fam, have, versions):
    """For each version to extract, (base game, update or None): v0 = the base game alone, v<N> = the base game + the update with version number N"""
    say(f'base: {fam["base"].name}')
    out = {}
    for ver in versions:
        if ver not in have: raise BuildError(f'no NSP for update {ver} (the game profile needs these versions: {", ".join(versions)})')
        if have[ver]: say(f'update {ver}: {have[ver].name}')
        out[ver] = (fam['base'], have[ver])
    return out


def can_clone(d):   # whether this volume supports APFS clones (nx_extract uses them to save space; without them it writes full copies)
    a, b = d / '.clone_test', d / '.clone_test2'
    try:
        a.write_bytes(b'x'); rmtree(b)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(a).encode(), str(b).encode(), 0) == 0
    finally:
        rmtree(a); rmtree(b)


# ---- Survey of a new game (when no game profile matches) ----
def survey(nsp_dir, app, fam, keys, repo, py, env):
    work = nsp_dir / f'{app:016X}_survey'; logs, ex = work / 'logs', work / 'extract'
    say(f'no game profile for {app:016X}: surveying first')
    PROG.plan(4)   # nx_extract, the manifest, the code modules (reading them, then the rewrite-site census), the imports
    PROG.stage('survey: build nx_extract')
    nx = build_nx_extract(repo, work, logs, env)
    ups = sorted(fam['updates'], key=lambda u: u[0] or 0)
    upd = ups[-1][1] if ups else None
    tmp = work / 'extract.partial'; rmtree(tmp)
    def listed():   # files in the manifest so far
        try: return {'done': (tmp / 'romfs_manifest.tsv').read_bytes().count(b'\n'), 'unit': 'files'}
        except OSError: return None
    PROG.stage('survey: RomFS manifest', poll=listed)
    say(f'extracting the code modules and a RomFS manifest (no game files written) → {ex}')
    run([nx, keys, fam['base'], upd or '-', tmp, '--manifest'] + (['--allow-damaged'] if ALLOW_DAMAGED else []), logs / 'extract.log')
    swap_in(tmp, ex)
    PROG.stage('survey: code modules', step='reading')
    xlog = (logs / 'extract.log').read_text(errors='replace')
    dv = re.search(r'display version (\S+)', xlog)
    F = pg.facts_of(ex / 'ExeFS', ex / 'romfs_manifest.tsv'); mods, rom = F['mods'], F['romfs']   # dm/games/profile_guess.py: what the code modules and the RomFS show
    R = [f'# Survey: {nacp.name(ex / "control.nacp") or "?"} ({app:016X})', '', f'- display version: {dv.group(1) if dv else "?"}', f'- base: `{fam["base"].name}` ({fam["base"].stat().st_size / 2**30:.2f} GB)']
    R += [f'- update: `{p.name}` (v{v}, {p.stat().st_size / 2**30:.2f} GB){" (used for this survey)" if p == upd else ""}' for v, p in ups]
    R += [f'- DLC: `{p.name}` ({p.stat().st_size / 1024:.0f} KB{", licence only, no content" if p.stat().st_size < 1 << 20 else ""})' for _, _, p in fam['dlc']]

    # Code modules
    R += ['', '## Code modules', '', '| module | what | build ID | text / ro / data (MB) | bss (MB) |', '|---|---|---|---|---|']
    R += [f'| {n} | {m["kind"]} | `{m["bid"]}` | ' + ' / '.join(f'{z / 2**20:.1f}' for z in m['sizes']) + f' | {m["bss"] / 2**20:.1f} |' for n, m in mods.items()]
    if F['program'] is not None: R.append(f'\n- main.npdm program ID: `{F["program"]:016X}`')
    if (ex / 'ExeFS/main.npdm').exists(): R.append(f'- system resource: {F["sysres"] / 2**20:.0f} MB')
    R.append(f'- SDK: {", ".join(sorted(F["sdkver"])) or "?"} (Mario: 16_2_0 / 16_2_12)')
    R.append('- SDK components: ' + (', '.join(sorted(F['comps'])) or '—'))
    if F['unity']: R.append('- Unity: ' + ', '.join(sorted(F['unity'])))
    R.append('- engine and API markers: ' + ('; '.join(f'{k} ({", ".join(v)})' for k, v in F['markers'].items()) or '—'))
    extra = [n for n in mods if n not in ('main', 'sdk', 'rtld')]

    # RomFS manifest
    R += ['', f'## RomFS: {rom["n"]} files, {rom["bytes"] / 2**30:.2f} GB', '', '| top level | files | GB |', '|---|---|---|']
    R += [f'| {t} | {n} | {rom["topb"][t] / 2**30:.2f} |' for t, n in rom['tops'].most_common(20)]
    R += ['', '| header magic | files | GB |', '|---|---|---|'] + [f'| {m} | {n} | {rom["magb"][m] / 2**30:.2f} |' for m, n in rom['mags'].most_common()]
    R += ['', 'extensions: ' + ', '.join(f'`{e}` {n}' for e, n in rom['exts'].most_common(20))]

    # Rewrite-site census and SDK surface
    R += ['', '## Rewrite-site census (insn_census.py)', '', '```']
    census = [ex / 'ExeFS' / n for n in mods if n != 'rtld']   # rtld is replaced by the host
    for i, m in enumerate(census):
        PROG.set(step=f'rewrite sites in {m.name}', done=i, total=len(census), unit='modules')
        run([py, repo / 'dm/tools/insn_census.py', m], logs / f'census_{m.name}.log', env=env)
        R += (logs / f'census_{m.name}.log').read_text(errors='replace').rstrip().splitlines()
    R += ['```', '', '## Imports vs host hooks (nso_syms.py)', '', '```']
    PROG.stage('survey: imports vs host hooks')
    run([py, repo / 'dm/tools/nso_syms.py', repo / 'dm/host', '-', ex / 'ExeFS'], logs / 'nso_syms.log', env=env)
    PROG.end()
    R += (logs / 'nso_syms.log').read_text(errors='replace').rstrip().splitlines() + ['```']

    # What it means for a port
    G = pg.guess(F)   # the provisional profile's keys (write_profile writes the same)
    keep = G['modules'][0].split()   # how the build will get the shaders (decided again from the extraction at build time; the survey has only a RomFS manifest, so Unreal paks are guessed from main's markers)
    S = pg.shader_sources(mods['main']['imports'], F['glsl_src'], [m for m in keep if 'glslcCompile' in mods[m]['exports']], rom['mags']['SPIR-V'],
                          rom['exts'].get('.pak', 0) if F.get('main_markers', set()) & {'Unreal', 'Unreal 4'} else 0, rom['mags']['Trinity pack'], gl_names=F.get('gl_names', 0),
                          gl_src=F.get('gl_src', 0), gl_files=sum(rom['exts'].get(e, 0) for e in pg.GL_FILES), oodle_pkz=rom['mags']['PlatinumGames PKZ'])   # the manifest has no name tables: every PKZ counts
    vulkan, gl = S['graphics'][0] == 'vulkan', S['graphics'][0] == 'gl'
    notes = []
    if not vulkan and not gl and 'nvnBootstrapLoader' not in mods['main']['imports']: notes.append('main imports neither nvnBootstrapLoader nor Vulkan functions nor OpenGL through EGL: the host implements only NVN, Vulkan and OpenGL')
    if vulkan: notes.append(f'{S["graphics"][1]}; the NVN shader steps are skipped' + (', and its shaders are captured by booting it on the Mac (no SPIR-V file in the RomFS)' if S['vk_capture'][0] else ''))
    if gl: notes.append(f'{S["graphics"][1]}; the NVN shader steps are skipped: gl_shaders.py translates its GLSL sources (MVGL archives, main\'s strings'
                        + (', and the shaders it puts together at run time, captured by booting it on the Mac)' if S['gl_capture'][0] else ')'))
    notes += [S[k][1] for k in ('glslc_capture',) if S[k][0]]
    if S['ue_decode'][0]:   # the manifest has no pak footers, so both kinds are named; the build tells them apart by the footer's version (uepak.py)
        notes.append(f'{rom["exts"].get(".pak", 0)} Unreal pak(s): version 11 paks keep the shaders Oodle-compressed in shader code archives (Octopath Traveler 0), decoded by booting the game on the Mac; '
                     'legacy paks (version 4, DRAGON QUEST XI S) keep them inline in the cooked packages, which the shader split reads through the game\'s own decoder (LZ5) in the Mac host')
    if extra: notes.append('modules besides main / sdk: {}; the provisional profile loads {} (load order main, subsdk*, sdk)'.format(", ".join(f'{n} ({mods[n]["kind"]})' for n in extra), G["modules"][0]))
    if rom['mags']['UnityFS'] or 'Unity IL2CPP' in F['markers']: notes.append('Unity: the shaders are Shader objects in UnityFS bundles and data.unity3d, which shader_split.py reads through unityfs.py (written for Unity 2020.3\'s Switch format, also read for 2017.4, 2018.3, 2022.3 and 6000.0; check other Unity versions first)')
    if F['glslc']: notes.append(f'main has {F["glslc"]} GLSLC compiler output magics (0x19866891): shader_split.py takes those that are whole outputs (13 Sentinels and the KTGL games compile their shaders into main; Unity\'s player has 11 of its own)')
    if rom['mags']['Trinity pack']: notes.append('Trinity packs (Game Freak, arc/data.trpfs with its index data.trpfd): the shaders are BNSH inside Oodle Mermaid streams; shader_split.py decodes them with the game\'s own OodleLZ_Decompress in the Mac host, which dm/host/hle_oodle.c finds by the library\'s name string')
    elif rom['mags']['PlatinumGames PKZ']: notes.append('PlatinumGames PKZ archives (pkzl; Bayonetta 3): DAT archives inside, compressed per entry (OodleKraken, ZStandard or None); the shaders are shader objects (VSO / PSO / CSO …) in DATs, which shader_split.py reads (pkz_entries, pg_shader), Oodle Kraken through the game\'s own streaming decoder in the Mac host (dm/host/hle_oodle.c)')
    elif rom['mags']['MT Framework ARC']: notes.append('MT Framework ARC archives (Capcom, The Great Ace Attorney Chronicles): the shaders are in a shader package (SPK) inside one of them, which shader_split.py reads (mtarc_members, mtspk_shaders)')
    elif rom['mags']['Bezel Engine archive']: notes.append('NintendoWare Bezel Engine archives (.bea, magic SCNE; Big Brain Academy: Brain vs. Brain): every entry is a zstd frame, and the shaders are BNSH in their .bfsha entries, which shader_split.py reads (bea_members)')
    elif rom['mags']['XNB'] and 'MonoGame' in F['markers']: notes.append('MonoGame (XNB content): the shaders are MonoGame effects (MGFX version 8, NVN control sections and code areas) in the XNB files EffectReader reads, and SpriteBatch\'s in main, which shader_split.py reads (mgfx_effect; Celeste). A compressed XNB (LZX / LZ4) is not read')
    elif not (rom['mags']['BNSH'] or rom['mags']['BAHS']) and not rom['mags']['UnityFS'] and not vulkan and not gl and not F['glslc']:
        notes.append('no BNSH / BAHS at the top level: the shaders are inside compressed or other containers or in main, and must be located first')
    major = {v.split('_')[0] for v in F['sdkver']}
    if not major: notes.append('no SDK version string found')
    elif major != {'16'}: notes.append(f'SDK {"/".join(sorted(major))}, not Mario\'s 16: NVN object sizes, API version and service layouts differ by SDK (the host was converted from D2R\'s SDK 12 to SDK 16 sizes); check them one by one per docs/agents/03')
    if any(l.split()[:1] == ['nn::ro'] for l in R):
        nro, nrr = rom['exts']['.nro'], rom['exts']['.nrr']
        # NRR files register NROs by hash; with none loose the NROs are inside an archive (Super Smash Bros. Ultimate: 14 NRRs, 98 NROs in data.arc, whose exports main imports)
        notes.append(f'imports nn::ro (loads NRO modules at run time): the RomFS has {nro} .nro files and {nrr} .nrr files; registered NROs are collected and rewritten beside the other modules'
                     + (' (look inside archives for the registered modules)' if nrr and not nro else '') + ' and embedded in the iOS app (gen_embed.py)')
    if fam['dlc']: notes.append('DLC present: it is extracted into the IPA and the host reports the indices as owned and mounts them; check that the host hooks every aoc / fs add-on content call main imports')
    R += ['', '## What it means for a port', ''] + [f'- {n}' for n in notes or ['—']]
    R += ['', '## Provisional profile keys (dm/games/profile_guess.py)', '', 'What the game files decide; mem_used follows the rewrite, and a game with virtual address memory that sets a heap gets a heap check on the Mac (scripts/build_ipa_from_nsp.py heap_check).', '', '```']
    R += [f'{k}\t{v}' + (f'\t# {why}' if why else '') for k, (v, why) in G.items()] + ['```']
    R += ['', 'Next steps: docs/agents (02 survey, 03 bring-up).']
    (work / 'survey.md').write_text('\n'.join(R) + '\n')
    for n in notes: say(n)
    say(f'survey report: {work / "survey.md"}')
    upv = ups[-1][0] if ups else None
    return {'F': F, 'name': nacp.name(ex / 'control.nacp') or '', 'display': dv.group(1) if dv else '?', 'version': 'v0' if upd is None else f'v{upv or 1}'}


# ---- Provisional profile (a game with no profile in dm/games) ----
def ipa_name(name, app):   # "The Legend of Zelda: Link's Awakening" → TheLegendOfZeldaLinksAwakening.ipa: ASCII words joined, apostrophes dropped; the title ID when nothing ASCII is left
    words = re.findall(r'[A-Za-z0-9]+', name.replace("'", '').replace('’', ''))
    words = [w.capitalize() if w.isupper() and len(w) > 1 else w[:1].upper() + w[1:] for w in words]
    return (''.join(words)[:48] or f'{app:016X}') + '.ipa'


EMPTY_TABLES = {   # the shader tables before (or, for a Vulkan game, instead of) a shader split: the profile inputs' digest (dmsh_chain.py inputs) and build_ipa.sh read them
    'shader_map.tsv': '# kind: evidence  provisional profile: filled by the shader split\npackage\tindex\tstage\tdm_ctl_sha16\n',
    'shader_facts.tsv': '# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. Provisional profile: filled by the shader split\n',
    'shader_pairs.tsv': '# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n',
    'shader_map.tsv.conflicts.tsv': '# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n',
    'bf_want.tsv': '# generated\n',
    'pso_manifest.tsv': '# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key.\n'
                        '# Provisional profile: empty until recorded on the Mac (DM_PSO_MANIFEST=off DM_PSO_RECORD).\n'}


def write_profile(pdir, app, f):
    """A provisional game.tsv (+ the empty tables) from the survey, in pdir (outside the repository). An existing game.tsv is kept, so it can be edited by hand; delete it to regenerate.
    The keys the game files decide come from dm/games/profile_guess.py (each written under its reason): the modules to load, the memory partition, engine presets; the rest keeps game.py's
    defaults until the heap check (below) or a developer sets it. What the files show at every build is not written: the graphics API and the shader sources (shader_sources), the shared-font
    layout and the NVN API version (the host reads the SDK version)"""
    pdir.mkdir(parents=True, exist_ok=True)
    if (pdir / 'game.tsv').exists():
        say(f'reusing the provisional profile: {pdir / "game.tsv"} (delete it to regenerate from the survey)')
        return
    F = f['F']; G, R = pg.guess(F), F['romfs']
    groups, seen = [], {'main', 'other'}
    for d, _ in R['dirs'].most_common(24):   # shader package groups by RomFS top-level folder (this only names the package files; anything else goes to "other")
        g = re.sub(r'[^a-z0-9]', '', d.lower())[:16] or 'g'
        while g in seen: g += 'x'
        seen.add(g); groups.append(f'/{d}/={g}')
    # stored in the IPA uncompressed: file types that are mostly compressed already (Yaz0 / zstd / zip / media headers; Super Mario Odyssey's 3.8 GB of .szs took the zip 1 h 40 min to deflate again)
    # and big files (mostly compressed formats without a known header)
    store = sorted(e for e, b in R['extb'].items() if e and R['exts'][e] and (b / R['exts'][e] >= 1 << 20 or 2 * R['extc'][e] >= R['exts'][e]))
    name = f['name'] or f'{app:016X}'
    mods = G['modules'][0].split()
    T = '\t'
    def key(k, default=None):   # a key with the reason profile_guess.py gives for it above it; None when neither the guess nor a default has one
        v, why = G[k] if k in G else (default, '')
        return [] if v is None else ([f'# {why}'] if why else []) + [f'{k}{T}{v}']
    L = ['# Provisional game profile (generated by scripts/build_ipa_from_nsp.py from the survey with dm/games/profile_guess.py, not checked by hand)',
         f'# {name}, SDK {F["sdk"] or "?"}, survey {f["survey"]}', '',
         '# ---- Identity ----', f'title_id{T}{app:016X}', f'program_id{T}{(F["program"] or app):016X}', '',
         '# ---- App ----', f'name{T}{name}', f'ipa{T}{ipa_name(name, app)}', f'app_category{T}public.app-category.games', *key('pad_layout'), *key('op_mode'), '',
         '# ---- Versions ----', T.join(['build', f['version'], f['display']] + [f'{m}={F["mods"][m]["bid"]}' for m in mods]), f'package{T}{f["version"]}', f'shader_versions{T}{f["version"]}', '',
         '# ---- Files ----', *key('modules'), f'romfs_marker{T}{R["biggest"][1]}', f'zip_store{T}{":".join(store + [".metallib"])}', '',
         '# ---- Memory ----', *key('mem_reserve'), *key('mem_alias'), *key('mem_stack'), *key('app_pool'), *key('sys_resource'), '',
         '# ---- Shader split ----', *key('shader_groups', " ".join(groups) or "/=romfs"), f'shader_note{T}{name} shaders (provisional profile)']
    (pdir / 'game.tsv').write_text('\n'.join(L) + '\n', encoding='utf-8')
    for n, text in EMPTY_TABLES.items(): (pdir / n).write_text(text)
    say(f'wrote a provisional game profile: {pdir / "game.tsv"}')
    for k in ('modules', 'mem_reserve', 'op_mode'):
        if k in G and G[k][1]: say(f'  {k} {G[k][0]}: {G[k][1]}')


# ---- Derived profile (a version the game's profile was not brought up on) ----
DERIVED_SKIP = {'game.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'vk_capture.tsv', 'gl_capture.tsv', 'patches.py'}   # made again from the packed version's own files (patches.py: the rewriter's patches of the registered builds)


def derive_profile(g, pdir, ver, d, mods):
    """The game's profile for a version it was not brought up on (the folder has no update, another update, or files of other builds), in pdir (outside the repository, where a
    provisional profile goes): its keys as they are, with this version's build row, package and shader versions. The shader tables, the chain header and the run-time capture list are
    left out: the build makes them again from this version's own files, as for a provisional profile. The hand-written tables (the prewarm list, shader_facts_manual.tsv) are copied:
    they name shaders by content key. Returns the version's display version"""
    rmtree(pdir); pdir.mkdir(parents=True)
    for p in g.dir.iterdir():
        if p.is_file() and p.name not in DERIVED_SKIP: shutil.copyfile(p, pdir / p.name)
    T, disp = '\t', nacp.version(d / 'control.nacp') or '?'
    row = T.join(['build', ver, disp] + [f'{m}={build_id(d / "ExeFS" / m)}' for m in mods if (d / 'ExeFS' / m).exists()])
    out, put = [f'# Derived from game profile {g.dir} by scripts/build_ipa_from_nsp.py for {ver} ({disp}), a version that profile was not brought up on:',
                '# its keys as they are, this version\'s build row, and shader tables made from this version\'s own files'], False
    for line in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
        if line.split(T, 1)[0] in ('build', 'package', 'shader_versions'):
            if not put: out += [row, f'package{T}{ver}', f'shader_versions{T}{ver}']; put = True
            continue
        out.append(line)
    (pdir / 'game.tsv').write_text('\n'.join(out) + '\n', encoding='utf-8')
    return disp


# ---- Stage reuse: a stage whose inputs are all as at its last successful run keeps its output ----
def files_under(d):   # (relative path, stat) of every file under d, sorted; [] when it does not exist
    out, todo = [], [str(d)]
    while todo:
        try: it = os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                if e.is_dir(follow_symlinks=False): todo.append(e.path)
                else: out.append((os.path.relpath(e.path, d), e.stat(follow_symlinks=False)))
    return sorted(out, key=lambda x: x[0])


def fingerprint(*items, env=None):
    """sha256 over a stage's inputs. Items: ('files', paths) = names and content (a missing file counts too); ('tree', folder) = relative path and size of every file under it
    (a RomFS: check_build pins its version, so names and sizes stand for its content); ('stat', folder) = relative path, size and mtime of every file under it (the translator's binaries);
    ('cmd', argv) = a tool's version output (run with env); anything else = its repr"""
    h = hashlib.sha256()
    for it in items:
        kind, v = it if isinstance(it, tuple) and len(it) == 2 and it[0] in ('files', 'tree', 'stat', 'cmd') else ('repr', it)
        h.update(f'\0{kind}\0'.encode())
        if kind == 'files':
            for p in map(Path, v):
                h.update(p.name.encode() + b'\0')
                try: h.update(hashlib.sha256(p.read_bytes()).digest())
                except OSError: h.update(b'missing')
        elif kind in ('tree', 'stat'):
            for rel, st in files_under(v): h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns if kind == "stat" else ""}\n'.encode())
        elif kind == 'cmd':
            try: r = subprocess.run(v, env=env, capture_output=True, text=True, timeout=120); h.update((r.stdout + r.stderr).encode())
            except (OSError, subprocess.TimeoutExpired): h.update(b'missing')
        else: h.update(repr(v).encode())
    return h.hexdigest()


def reused(keyfile, key):   # the note the stage's last successful run wrote beside its key ('' when none), or None when the key differs or is missing
    try: k, _, note = keyfile.read_text().partition('\n')
    except OSError: return None
    return note.strip() if k == key else None


def tool_files(repo):   # what the shader stages run: dm/gfx/tools' scripts and the lock of the translator's fingerprints, the profile reader, the iOS compile convention, the NSO reader
    return sorted((repo / 'dm/gfx/tools').glob('*.py')) + [repo / p for p in ('dm/gfx/tools/dmsh_chain.lock', 'dm/games/game.py', 'dm/ios/tools/ios_metal.py')]


def chain_files(repo):   # what the iOS shaders run: dmsh_build.py and every repository module it imports, at any depth (dmsh_*.py, ios_metal.py, game.py), and the translator's lock; not the split's readers (shader_split.py, unityfs.py, uepak.py)
    dirs, todo, seen = [repo / d for d in ('dm/gfx/tools', 'dm/ios/tools', 'dm/games')], [repo / 'dm/gfx/tools/dmsh_build.py'], set()
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file(): continue
        seen.add(p)
        for n in ast.walk(ast.parse(p.read_bytes())):
            mods = [a.name for a in n.names] if isinstance(n, ast.Import) else [n.module] if isinstance(n, ast.ImportFrom) and n.module else []
            todo += [d / f'{m.split(".")[0]}.py' for m in mods for d in dirs]
    return sorted(seen) + [repo / 'dm/gfx/tools/dmsh_chain.lock']


def package_key(pkg):   # the shader package's content (file names and bytes): what the iOS shaders are built from
    fs = [rel for rel, _ in files_under(pkg)]
    return fingerprint(fs, ('files', [pkg / f for f in fs]))


def profile_files(g):   # every file of the game profile (game.tsv and its tables) but the c1 / uniform extent tables, which the iOS shader step makes (chain_tables): no step reads them,
    out = {name for name, _, _ in CHAIN_TABLES} | {'patches.py'}   # and the rewriter's patches, which only the rewrite step reads (patch_files)
    return sorted(p for p in g.dir.iterdir() if p.is_file() and p.name not in out)


# ---- Steps ----
def check_prereqs(env, xlate, mac=False):   # mac: a Mac-only build (PLATFORM 'mac'), which needs no xcodegen and compiles shaders only for macOS
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools') if not _importable(m)] + ([] if _importable('compression.zstd') or _importable('backports.zstd') else ['backports.zstd'])   # zstd: built in from Python 3.14
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 capstone fonttools backports.zstd; backports.zstd only before Python 3.14)')
    tools = {'xcodebuild': 'Xcode', 'xcodegen': 'brew install xcodegen', 'dotnet': '.NET SDK 10', 'zstd': 'brew install zstd', 'spirv-cross': 'brew install spirv-cross', 'make': 'Xcode'}
    if mac: del tools['xcodegen']
    gone = [f'{t} ({how})' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('tools not found: ' + ', '.join(gone))
    mtl = subprocess.run(['xcrun', '-sdk', 'macosx' if mac else 'iphoneos', 'metal', '--version'], capture_output=True, text=True, env=env)   # Xcode 26 and later install the Metal toolchain separately
    if mtl.returncode and env.get('DM_METAL_RUNTIME') != '1': raise BuildError(f'the Metal compiler does not run ({(mtl.stderr.strip().splitlines() or ["no output"])[-1]}): install it with xcodebuild -downloadComponent MetalToolchain')
    if not xlate.exists(): raise BuildError(f'the shader translator is missing from the repository: {xlate}')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def extracted(log):   # (files, bytes) from the summary lines nx_extract wrote to log ("<dir>: N files X MB, ..." for ExeFS + RomFS; older logs have "| N files X MB"); None without them
    try: rows = re.findall(r'[:|] (\d+) files ([\d.]+) MB', log.read_text(errors='replace'))
    except OSError: return None
    return (sum(int(n) for n, _ in rows), int(sum(float(mb) for _, mb in rows) * 2**20)) if rows else None


def extract(nx, keys, ex, name, base, upd, cmp_dir, logs, g):   # the build IDs are checked by the caller (build_mismatch)
    dst, marker, log = ex / name, g.get('romfs_marker'), logs / f'extract_{name}.log'
    if (dst / 'ExeFS' / 'main').exists() and (dst / 'RomFS' / marker).exists():
        PROG.stage(f'extract {name}')
        say(f'reusing the extracted {name}: {dst}')
        PROG.end('reused')
        return dst
    tmp = ex / (name + '.partial'); rmtree(tmp)
    last = extracted(log)   # the last extraction of this version (its log is about to be overwritten) has the exact size; without one, the NSPs' size is the estimate
    def poll():
        n, b = tree_size(tmp); return {'done': b, 'note': f'{n:,} files'}
    PROG.stage(f'extract {name}', poll=poll, unit='B', total=last[1] if last else base.stat().st_size + (upd.stat().st_size if upd else 0), est=not last)
    run([nx, keys, base, upd or '-', tmp] + ([cmp_dir] if cmp_dir else []) + (['--allow-damaged'] if ALLOW_DAMAGED else []), log)
    swap_in(tmp, dst)
    n, b = extracted(log) or (0, 0)
    PROG.end(f'{n:,} files, {amount(b, "B")}')
    return dst


def extract_aoc(nx, keys, repo, work, app, fam, logs):
    """DLC NSPs in the folder → <id>_build/aoc/<index>/ (the RomFS of each one's data NCA; index = title ID − base − 0x1000). The host reports these indices as owned and mounts them (DM_AOC_DIR).
    Returns None without DLC. Reused when the DLC NSPs (names, sizes and times) and nx_extract's sources are as at the last extraction (Monster Hunter Rise: 250 DLC, 30 s)"""
    if not fam['dlc']: return None
    tmp, dst = work / 'aoc.partial', work / 'aoc'; rmtree(tmp)
    best = {}   # when one DLC comes in several copies, take the highest version, and the largest of equal versions
    for tid, ver, p in fam['dlc']:
        k = (ver or 0, p.stat().st_size)
        if tid not in best or k > best[tid][0]: best[tid] = (k, p)
    PROG.stage('extract DLC')
    kf = work / 'aoc.key'
    key = fingerprint(sorted((tid, p.name, p.stat().st_size, p.stat().st_mtime_ns) for tid, (_, p) in best.items()), ('files', [repo / 'dm/tools/nx_extract' / f for f in ('Program.cs', 'nx_extract.csproj')]))
    if dst.is_dir() and (note := reused(kf, key)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return dst
    rmtree(kf)
    for i, (tid, (_, p)) in enumerate(sorted(best.items())):
        idx = tid - app - 0x1000
        PROG.set(step='extracting', done=i, total=len(best), unit='DLC')
        say(f'DLC index {idx}: {p.name}')
        run([nx, keys, '--aoc', p, tmp / str(idx)], logs / f'aoc_{idx}.log')
    swap_in(tmp, dst)
    kf.write_text(f'{key}\n{len(best)} DLC\n')
    PROG.end(f'{len(best)} DLC')
    return dst


def build_mismatch(d, name, g):   # the modules of an extraction whose build ID is not the one the profile's build row of that version registers ([] = the registered build)
    want = g.builds().get(name)
    if want is None: return [f'{name} has no build row']
    return [f'{m} {build_id(d / "ExeFS" / m)} (registered {bid})' for m, bid in want.items() if m != 'display' and build_id(d / 'ExeFS' / m) != bid]


def patch_files(repo, exe_dir, mods, profile_dir=None):
    """The patch files that name one of the modules' builds (dm/tools/sdk_patches.py, dm/games/*/patches.py, and a provisional / derived profile outside the repository):
    patch_nso.py loads them all but applies only those of a module's build ID, so another game's patches do not change this game's rewrite"""
    ids = {build_id(exe_dir / m) for m in mods}
    own = profile_dir / 'patches.py' if profile_dir else None
    candidates = [repo / 'dm/tools/sdk_patches.py'] + sorted((repo / 'dm/games').glob('*/patches.py')) + ([own] if own and own.is_file() else [])
    return [p for p in candidates if any(i in p.read_text(errors='replace') for i in ids)]


def collect_nros(py, repo, romfs, dst, env, logs):
    """The NROs a game registers in NRR files (RomFS *.nrr) and loads at run time with nn::ro, found by their SHA-256 as loose files or inside the RomFS's archives (dm/tools/nro_collect.py):
    {module name: NRO path}, {} for a game without NRRs. The host maps their rewritten copies at start and loads one when the game asks (loader.c "NRO modules"; Super Smash Bros. Ultimate:
    98 in data.arc). Reused while the NRRs, RomFS tree and collection tools are unchanged"""
    nrrs = sorted(p for p in romfs.rglob('*.nrr') if p.is_file())   # Super Smash Bros. Ultimate keeps them in a folder named .nrr
    kf = dst.with_name(dst.name + '.key')
    if not nrrs:
        rmtree(dst); rmtree(kf)   # an update can remove its NRRs: do not carry the previous manifest into the rewrite
        return {}
    key = fingerprint(('files', nrrs + [repo / f for f in ('dm/tools/nro_collect.py', 'dm/tools/nso_syms.py', 'dm/gfx/tools/smash_arc.py')]), ('tree', romfs))
    if not ((dst / 'nro.tsv').is_file() and reused(kf, key) is not None):
        tmp = dst.with_name(dst.name + '.partial'); rmtree(tmp)
        PROG.set(step=f'finding the NROs {len(nrrs)} NRR files register')
        run([py, repo / 'dm/tools/nro_collect.py', romfs, tmp], logs / 'nro_collect.log', env=env)
        rmtree(kf); swap_in(tmp, dst); kf.write_text(f'{key}\n')
    # rows named 'refused' are online SDKs the host never loads (Epic Online Services): listed for the host's log, not rewritten
    return {l.split('\t')[0]: dst / (l.split('\t')[0] + '.nro') for l in (dst / 'nro.tsv').read_text().splitlines() if l and not l.startswith('#') and not l.startswith('refused\t')}


def rewrite(py, repo, exe_dir, out, env, logs, g):
    """patch_nso.py on every code module (each writes its own <module>.* files, all the same build_src.sha256 line), each with its log, as many at once as the Mac has cores.
    The modules are the profile's and the NROs the game registers (collect_nros; nro.tsv beside them lists those). Reused when the modules, patch_nso.py (with nso_code.py, nso_asm.py
    and the patch files of their builds), its switch and capstone are as at the last successful rewrite (Monster Hunter Rise's main alone takes 5.5 min)"""
    import capstone
    mods = g.words('modules')
    PROG.stage('rewrite', step='checking the inputs')
    nro_dir = out.with_name('nro_' + out.name.removeprefix('patched_'))
    nros = collect_nros(py, repo, exe_dir.parent / 'RomFS', nro_dir, env, logs)
    manifest = nro_dir / 'nro.tsv'
    src = {m: exe_dir / m for m in mods} | nros
    names = list(src)
    kf = out.with_name(out.name + '.key')
    key = fingerprint(('files', [repo / 'dm/tools' / f for f in ('patch_nso.py', 'nso_code.py', 'nso_asm.py')] + patch_files(repo, exe_dir, mods, getattr(g, 'dir', None)) + list(src.values()) + ([manifest] if manifest.is_file() else [])), names, capstone.__version__,
                      {k: env.get(k) for k in ('DM_PATCH_FRONT',)})
    if all((out / f'{m}.patched.bin').is_file() for m in names) and reused(kf, key) is not None:
        PROG.end(f'{", ".join(mods)}{f" and {len(nros)} NROs" if nros else ""}, reused (inputs unchanged)'); return
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True); logs.mkdir(parents=True, exist_ok=True)
    if manifest.is_file(): shutil.copyfile(manifest, tmp / 'nro.tsv')   # refused-only manifests preserve the game's offline registration path too
    PROG.set(step=', '.join(mods) + (f' and {len(nros)} NROs' if nros else ''), done=0, total=len(names), unit='modules')
    procs, logf, todo, cores = {}, {}, list(names), max(2, os.cpu_count() or 4)
    try:
        while todo or procs:
            while todo and len(procs) < cores:
                m = todo.pop(0); logf[m] = open(logs / f'rewrite_{m}.log', 'w')
                procs[m] = subprocess.Popen([str(py), str(repo / 'dm/tools/patch_nso.py'), str(src[m]), str(tmp), m], env=env, stdout=logf[m], stderr=subprocess.STDOUT)
            for m in [m for m in procs if procs[m].poll() is not None]:
                p = procs.pop(m); logf[m].close(); PROG.set(done=len(names) - len(todo) - len(procs))
                if p.returncode:
                    log = logs / f'rewrite_{m}.log'
                    PROG.end(ok=False)
                    PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
                    raise BuildError(f'patch_nso.py exit code {p.returncode} on {m} (full output: {log})')
            time.sleep(0.2)
    finally:   # an error or an interrupt stops the other rewrites
        for p in procs.values():
            if p.poll() is None: p.terminate(); p.wait()
        for f in logf.values(): f.close()
    rmtree(kf); swap_in(tmp, out); kf.write_text(f'{key}\n')
    PROG.end(', '.join(mods) + (f' and {len(nros)} NROs' if nros else ''))


GLSLC_SECS = 30   # the longest capture run: Three Houses compiles its 9 shaders within 1.2 s of boot, and the run stops as soon as every file the profile names is there


def glslc_wanted(g):   # the capture files the profile's shader map was built from (column 5: /glslc_capture/<name>#GLSLC@…); None without any (a provisional profile)
    try: t = g.table('shader_map.tsv').read_text(errors='replace')
    except OSError: return None
    return set(re.findall(r'/glslc_capture/([0-9a-f]{16}\.glslc)#', t)) or None


def glslc_done(d):   # capture files written whole: a GLSLC output (magic 0x19866891) holds its own length at +0x44
    out = set()
    try: names = [n for n in os.listdir(d) if n.endswith('.glslc')]
    except OSError: return out
    for n in names:
        try: b = (d / n).read_bytes()
        except OSError: continue
        if len(b) >= 0x48 and b[:4] == b'\x91\x68\x86\x19' and struct.unpack_from('<I', b, 0x44)[0] == len(b): out.add(n)
    return out


def apple_silicon():   # hw.optional.arm64 is 1 on Apple silicon (for a Python under Rosetta too); an Intel Mac has no such key
    return subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() == '1'


HOST_DIRS = ('dm/host', 'dm/gfx', 'dm/gfx/nvn', 'dm/gfx/nvn/include', 'dm/gfx/tex', 'dm/gfx/vk', 'dm/gfx/gl')   # the folders the host's compile line takes its sources and headers from (dm/host/Makefile SRC, GFX, GFX_H)


def build_mac_host(repo, out, env, logs, g):
    """The Mac host for the profile, built into out: dm/host's own compile line (make -n) with the output moved, since make takes no target path with spaces and dm/host/dmhost may be another build's.
    Reused when the compile line and its inputs (the source files of HOST_DIRS, the chain header dm/gfx/nvn/dmsh_chain.h among them, the profile header make generates, libvpx and the third-party headers, the compiler
    and the macOS SDK) are as at the last build into out. The host runs the game's own arm64 code natively, so an Intel Mac stops here (GitHub issue #54)"""
    if not apple_silicon(): raise BuildError('this step boots the game in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    host = repo / 'dm/host'
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/game_profile.h', 'gen/vpx/libvpx.a'], logs / 'mac_host_make.log', env=env, cwd=host)
    dry = subprocess.run(['make', '-n', '-B', f'GAME={g.id}', 'dmhost'], env=env, cwd=host, capture_output=True, text=True)
    line = next((l for l in dry.stdout.splitlines() if ' -o dmhost ' in l), None)
    if not line: raise BuildError(f'make -n gave no compile line for the Mac host: {dry.stderr.strip()[-500:]}')
    cmd = shlex.split(line); cmd[cmd.index('-o') + 1] = out
    kf, tp = out.with_name(out.name + '.key'), repo / 'dm/third_party'
    srcs = [p for d in HOST_DIRS for p in sorted((repo / d).iterdir()) if p.is_file() and p.suffix in ('.c', '.m', '.h', '.S', '.inc')]
    key = fingerprint(cmd, ('files', srcs + [host / f'gen/{g.id}/game_profile.h', host / 'gen/vpx/libvpx.a', tp / 'stb_vorbis/stb_vorbis.c']),
                      ('tree', tp / 'vulkan-headers/include'), ('tree', tp / 'libvpx/vpx'), ('cmd', ['clang', '--version']), ('cmd', ['xcrun', '--show-sdk-build-version']), env=env)
    if out.is_file() and reused(kf, key) is not None: return out
    rmtree(kf); out.parent.mkdir(parents=True, exist_ok=True)
    run(cmd, logs / 'mac_host_build.log', env=env, cwd=host)
    kf.write_text(f'{key}\n')
    return out


def boot_mac(host, patched, henv, rd, log, secs, done=lambda: False):
    """Boot the game in the Mac host (headless and silent: no DM_WINDOW, no DM_AUDIO) with its output in log, until done() says so, the host exits, or secs pass;
    then SIGTERM (the host's graceful exit), a kill 5 s later. Returns the host's exit code, None when it was still running"""
    t0 = time.time()
    with open(log, 'w') as f:
        p = subprocess.Popen([str(host), str(patched)], env=henv, cwd=rd, stdout=f, stderr=subprocess.STDOUT)
        try:
            while not done() and p.poll() is None and time.time() - t0 < secs: time.sleep(0.25)
        finally:
            rc = p.poll()
            if rc is None:
                p.terminate()
                try: p.wait(5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
    return rc


def host_stopped(log, rc, secs):   # a capture run whose host stopped by itself before the capture was complete: the game crashed or gave up, and the IPA will too (a provisional profile's IPA is still built)
    PROG.say(f'[RED] the game stopped after {secs:.0f} s in the Mac host (exit {rc}): its shaders are incomplete and the IPA will not get further; ---- last lines of {log} ----\n'
             + ''.join(collections.deque(open(log, errors='replace'), maxlen=12)).rstrip('\n'))


def capture_glslc(repo, dirs, pkg_ver, patched, work, env, logs, g):
    """A game that compiles some shaders at run time with its own GLSLC (Fire Emblem: Three Houses; profile_guess.py shader_sources), so no game file holds them. The game boots in the Mac host
    (headless and silent) with DM_GLSLC_CAPTURE (dm/host/hle_glslc.c), which writes each output to <version dir>/glslc_capture, where shader_split.py reads them; the outputs are the same on every run.
    dirs: {shader version: its extraction}; a folder that already holds every file the profile's shader map names is reused"""
    want = glslc_wanted(g)
    todo = [v for v, d in dirs.items() if not (glslc_done(d / 'glslc_capture') >= want if want else glslc_done(d / 'glslc_capture'))]
    PROG.stage('GLSLC capture')
    if not todo:
        say(f'reusing the captured run-time shaders: {", ".join(str(d / "glslc_capture") for d in dirs.values())}')
        PROG.end('reused'); return
    if todo != [pkg_ver]: raise BuildError(f'the run-time shader capture runs the packed version {pkg_ver}\'s code; {", ".join(v for v in todo if v != pkg_ver)} needs a capture of its own')
    PROG.set(step='building the Mac host')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    d = dirs[pkg_ver]; cap, rd, log = d / 'glslc_capture.partial', work / 'tmp/glslc_run', logs / 'glslc_capture.log'
    rmtree(cap); rmtree(rd)
    for s in ('save', 'temp'): (rd / s).mkdir(parents=True)
    # everything the run writes stays in <id>_build (DM_ROOT is already there): a fresh save and temporary storage, no shader libraries yet, the profile's shader map (the host looks beside itself by default)
    henv = dict(env, DM_ROMFS=str(d / 'RomFS'), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NVN_SHMAP=str(g.table('shader_map.tsv')), DM_GLSLC_CAPTURE=str(cap))
    PROG.set(step='booting the game', done=0, total=len(want) if want else None, unit='shaders')
    say(f'booting the game in the Mac host to capture the shaders it compiles at run time → {d / "glslc_capture"}')
    t0 = time.time()
    def done():   # every file the profile names is there (Three Houses: within its first frames)
        got = glslc_done(cap); PROG.set(done=len(got & want if want else got))
        return bool(want and got >= want)
    rc = boot_mac(host, patched, henv, rd, log, GLSLC_SECS, done)
    got = glslc_done(cap)
    if rc is not None and not (want and got >= want): host_stopped(log, rc, time.time() - t0)
    if not got or (want and not got >= want):
        PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
        raise BuildError(f'captured only {len(got)} run-time shaders'
                         + (f', the profile needs {len(want)}' if want else '') + f' (host output: {log})')
    if want and got - want: say(f'[RED] captured files the profile\'s shader map does not name: {", ".join(sorted(got - want))}')
    swap_in(cap, d / 'glslc_capture'); rmtree(rd)
    PROG.end(f'{len(got)} shaders in {hms(time.time() - t0)}')


UE_DECODE_SECS = 300   # the longest Oodle decode run: Octopath Traveler 0's 10,596 shaders take about 10 s, the boot up to nnMain included


def ue_stamp(d):   # what an extraction's decoded shader archives were made from: its paks and IoStore TOCs (paths and sizes)
    return ''.join(f'{p.relative_to(d / "RomFS")}\t{p.stat().st_size}\n' for p in sorted([*(d / 'RomFS').rglob('*.pak'), *(d / 'RomFS').rglob('*.utoc')]))


def decode_ue_shaders(repo, dirs, pkg_ver, patched, work, env, logs, g):
    """A game with Unreal paks (Octopath Traveler 0): Unreal Engine keeps its shaders in the shader code archives of its paks, each shader Oodle-compressed, and only the game's own Oodle
    decodes them. dm/gfx/tools/uepak.py takes the archives out of the paks (the pak index is AES-encrypted with a key from main) and writes one Oodle job; the game's code decodes it in the Mac host
    (DM_OODLE_JOB, dm/host/hle_oodle.c: right before nnMain, then the host exits), and the decoded archives go to <version dir>/ue_shaders, where shader_split.py reads them.
    The job runs the packed version's code (Oodle's format does not change between builds). Unreal Engine 4.26 and later compress shaders with LZ4 by default (DRAGON QUEST VII Reimagined),
    which uepak.py decodes itself, so such a game needs no host run here. Unreal Engine 5.6 (FINAL FANTASY RESONANCE) keeps its shader libraries in the IoStore container instead, two layers
    of Oodle (container blocks, then each shader group): uepak.py reads them through the game's Oodle in the host's tool mode (uepak.OodleHost), with no boot. A folder whose stamp matches its paks is reused"""
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import uepak
    todo = {v: d for v, d in dirs.items() if not ((d / 'ue_shaders/stamp').exists() and (d / 'ue_shaders/stamp').read_text() == ue_stamp(d))}
    PROG.stage('UE shader decode')
    if not todo:
        say(f'reusing the decoded shader archives: {", ".join(str(d / "ue_shaders") for d in dirs.values())}')
        PROG.end('reused'); return
    t0, total, host = time.time(), 0, None
    for v, d in todo.items():
        jd, rd, log = work / f'tmp/ue_job_{v}', work / 'tmp/ue_run', logs / f'ue_decode_{v}.log'
        rmtree(jd); rmtree(rd)
        for s in ('save', 'temp'): (rd / s).mkdir(parents=True)
        dec = None
        if uepak.io_shader_tocs(str(d / 'RomFS')):
            if not host:
                PROG.set(step='building the Mac host')
                host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
            dec = uepak.OodleHost(host, patched, env)
        PROG.set(step=f'{v}: shader archives out of the paks' + (' and IoStore containers' if dec else ''))
        try: listing, n = uepak.job(str(d / 'RomFS'), str(d / 'ExeFS/main'), str(jd), decode=dec)
        finally:
            if dec: dec.close()
        if not listing: raise BuildError(f'the RomFS has Unreal paks, but none under {d / "RomFS"} holds a shader code archive: where the shaders are has to be found first')
        say(f'{v}: ' + '; '.join(f'{x["name"]} {x["shaders"]:,} shaders' + (f' ({x["compression"]})' if x.get('compression') else '') for x in listing)
            + (f'; decoding {n:,} with the game\'s Oodle in the Mac host' if n else ''))
        if n:   # Oodle-compressed shaders (Octopath Traveler 0); LZ4 ones (Unreal Engine 4.26 and later by default) are decoded by uepak.py already
            if not host:
                PROG.set(step='building the Mac host')
                host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
            PROG.set(step=f'{v}: decoding {n:,} shaders in the Mac host')
            henv = dict(env, DM_ROMFS=str(dirs[pkg_ver] / 'RomFS'), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_OODLE_JOB=str(jd / 'oodle.job'))
            rc = boot_mac(host, patched, henv, rd, log, UE_DECODE_SECS)
            if rc != 0 or not (jd / 'oodle.job.out').exists():
                PROG.end(ok=False)
                PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=20)).rstrip('\n'))
                raise BuildError(f'the Oodle decode run stopped with exit {rc} (host output: {log})')
        out = d / 'ue_shaders.partial'; rmtree(out)
        uepak.finish(str(jd), str(out)); (out / 'stamp').write_text(ue_stamp(d))
        swap_in(out, d / 'ue_shaders'); rmtree(jd); rmtree(rd); total += n + sum(x['shaders'] for x in listing if x['compression'] == 'IoStore, decoded')
    PROG.end(f'{total:,} shaders in {hms(time.time() - t0)}')


CAPTURE_SECS = 120   # the longest run-time shader capture: Super Mario Galaxy creates its 3,089 Vulkan modules within 6 s of its first one (about 15 s after boot on an M4 Pro)
# Run-time shader captures, by graphics API: the host's switch, the profile's table of the keys a capture must hold, and a capture file's name → its key (None: not a whole file;
# the host writes each under a temporary name and renames it). Vulkan: <16 hex>.spv (dm/gfx/vk/hle_vk.m); GL: <16 hex>.vert / .frag (dm/gfx/gl/hle_gl.m sh_capture)
CAPTURES = {'vk': dict(name='Vulkan', env='DM_VK_CAPTURE', table='vk_capture.tsv', what='SPIR-V shaders it generates', key=lambda n: n[:-4] if n.endswith('.spv') and len(n) == 20 else None),
            'gl': dict(name='GL', env='DM_GL_CAPTURE', table='gl_capture.tsv', what='GLSL shaders it puts together',
                       key=lambda n: n if re.fullmatch(r'[0-9a-f]{16}\.(vert|frag)', n) else None)}


def capture_wanted(g, api):   # the keys the profile's capture table lists; None without the table (a provisional profile)
    try: keys = {l.strip() for l in g.table(CAPTURES[api]['table']).read_text().splitlines() if l.strip() and not l.startswith('#')}
    except OSError: return None
    return keys or None


def capture_done(d, api):   # keys of the whole files in a capture folder
    try: names = os.listdir(d)
    except OSError: return set()
    return {k for k in map(CAPTURES[api]['key'], names) if k}


def capture_shaders(repo, d, patched, work, env, logs, g, api):
    """Shaders no game file holds, made at run time: a Vulkan game without SPIR-V files (Super Mario Galaxy: Hagi's GX shaders, from its pipeline database) or an OpenGL game whose GLSL is put
    together at run time (Balatro: LÖVE adds its header and its built-in code to each of the game's shader files, in Lua). The game boots in the Mac host (headless and silent, no shader
    library) with DM_VK_CAPTURE (dm/gfx/vk/hle_vk.m) or DM_GL_CAPTURE (dm/gfx/gl/hle_gl.m), which writes each shader to <version dir>/vk_capture or gl_capture, where vk_shaders.py /
    gl_shaders.py read them with the RomFS; the shaders are the same on every run. The run stops once every key the profile's capture table lists is there; without the table, once none has
    been added for 20 s. A folder that already holds every listed key is reused"""
    C = CAPTURES[api]; want, out = capture_wanted(g, api), d / f'{api}_capture'
    PROG.stage(f'{C["name"]} shader capture')
    if want and capture_done(out, api) >= want:
        say(f'reusing the captured run-time shaders: {out}'); PROG.end('reused'); return out
    PROG.set(step='building the Mac host')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    cap, rd, log = d / f'{api}_capture.partial', work / f'tmp/{api}_run', logs / f'{api}_capture.log'
    rmtree(cap); rmtree(rd); cap.mkdir(parents=True)
    for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
    # everything the run writes stays in <id>_build (DM_ROOT is already there): a fresh save and temporary storage, an empty shader library folder (pipelines fail, the game still creates every shader)
    henv = dict(env, DM_ROMFS=str(d / 'RomFS'), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), **{C['env']: str(cap)})
    PROG.set(step='booting the game', done=0, total=len(want) if want else None, unit='shaders')
    say(f'booting the game in the Mac host to capture the {C["what"]} at run time → {out}')
    t0 = time.time(); last = {'n': 0, 't': t0}
    def done():   # every listed key is there; without the table, none has been added for 20 s
        got = capture_done(cap, api); n = len(got & want if want else got); PROG.set(done=n)
        if n != last['n']: last['n'], last['t'] = n, time.time()
        return bool((want and got >= want) or (not want and n and time.time() - last['t'] > 20))
    rc = boot_mac(host, patched, henv, rd, log, CAPTURE_SECS, done)
    got = capture_done(cap, api)
    if rc is not None and not (want and got >= want): host_stopped(log, rc, time.time() - t0)
    if not got or (want and not got >= want):
        PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
        raise BuildError(f'captured only {len(got)} run-time shaders'
                         + (f', the profile needs {len(want)}' if want else '') + f' (host output: {log})')
    if want and got - want: say(f'[RED] captured shaders the profile\'s {C["table"]} does not list: {len(got - want)}')
    swap_in(cap, out); rmtree(rd)
    PROG.end(f'{len(got)} shaders in {hms(time.time() - t0)}')
    return out


# ---- A provisional profile after the rewrite: mem_used and the heap check ----
def set_keys(g, changes, section):
    """Write keys into a provisional game.tsv, each under its reason, in a section of their own at the end; earlier rows of the same keys (and the reason line right above one) go.
    Returns the profile reloaded"""
    p = g.dir / 'game.tsv'; old = p.read_text(encoding='utf-8').splitlines(); out = []
    for line in old:
        if line.split('\t', 1)[0] in changes:
            if out and out[-1].startswith('# ') and not out[-1].startswith('# ----'): out.pop()
            continue
        out.append(line)
    out += ['', f'# ---- {section} ----'] + [x for k, (v, why) in changes.items() for x in ([f'# {why}'] if why else []) + [f'{k}\t{v}']]
    p.write_text('\n'.join(out) + '\n', encoding='utf-8')
    return type(g)(g.dir)


def provisional_prep(py, repo, env, logs, g):
    """What a provisional profile needs before a Mac host can be built for it: the shader tables (empty until a shader split fills them; an older provisional profile may lack some),
    which the host and build_ipa.sh read"""
    for n, text in EMPTY_TABLES.items():
        if not g.table(n).exists(): g.table(n).write_text(text)


CHAIN_H = 'dm/gfx/nvn/dmsh_chain.h'   # the chain version the host is compiled with, one for every game


def chain_of(repo):   # the chain version dm/gfx/nvn/dmsh_chain.h holds (None when it has none)
    try: m = re.search(r'#define DMSH_CHAIN "(.*)"', (repo / CHAIN_H).read_text())
    except OSError: return None
    return m.group(1) if m else None


def chain_header(py, repo, env, logs):
    """The chain version header, written again when it no longer holds the version the source tree gives (a chain change committed without dmsh_chain.py --header): dmsh_build.py stops
    on a stale one, and the host and build_ipa.sh read it. The version follows from the source alone (dmsh_chain.py), one for every game (GitHub issue #78: Breath of the Wild after bb585cd)"""
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py')], env=env, capture_output=True, text=True)
    if r.returncode or not r.stdout.split(): raise BuildError(f'dmsh_chain.py exit code {r.returncode}: {(r.stderr.strip() or "no version")[-500:]}')
    want, have = r.stdout.split()[-1], chain_of(repo)
    if have == want: return
    run([py, repo / 'dm/gfx/tools/dmsh_chain.py', '--header'], logs / 'dmsh_chain.log', env=env)
    say(f'chain version header rewritten: {repo / CHAIN_H} held {have or "no version"}, the source tree gives {want}')


CHAIN_TABLES = (('c1_full.tsv', 'c1_guard.py', 'c1 guard'), ('cb_extent.tsv', 'cb_extent.py', 'uniform read extents'))   # made from the shader build's Mac outputs: the host takes each only when its header names its library's stamp


def lib_head(lib):
    """What c1_full.tsv / cb_extent.tsv start with to be taken beside the library directory lib (dmsh_chain.py table_head): its chain version and the profile inputs its stamp (inputs.tsv,
    written by a full dmsh_build.py run) names. None when it has no stamp"""
    try: stamp = dict(l.split('\t', 1) for l in (lib / 'inputs.tsv').read_text().splitlines() if '\t' in l and not l.startswith('#'))
    except OSError: return None
    return f'# chain {lib.name} inputs {stamp["inputs"]}' if stamp.get('inputs') else None


def chain_tables(py, repo, sh, env, logs, g, chain):
    """c1_full.tsv (c1_guard.py: shaders whose c1 reads are not proven bounded) and cb_extent.tsv (cb_extent.py: how far each shader reads into its uniform blocks) from the Mac outputs of the
    shader build (sh/cache/b-<chain>/), into the profile folder. They are not in the repository (git ignores them): they follow from the shaders this Mac built, so every pack makes its own.
    A full dmsh_build.py run makes both; this makes those missing or not naming the library's stamp again, which the host would ignore (every c1 its own 64 KiB, whole uniform bindings
    copied; GitHub issue #78). True when both name it; False when the library has no stamp (no full build's Mac outputs to make them from)"""
    lib = sh / 'cache' / f'b-{chain}'
    want = lib_head(lib)
    if not want: return False
    for name, tool, step in CHAIN_TABLES:
        p = g.table(name)
        try: head = p.read_text(errors='replace').partition('\n')[0]
        except OSError: head = ''
        if head.startswith(want + ' '): continue
        PROG.set(step=step)
        run([py, repo / 'dm/gfx/tools' / tool, lib, p], logs / f'{tool[:-3]}.log', env=env)
        say(f'{name} ' + (f'named {" ".join(head.split()[2:5])}' if head.startswith('# chain ') else 'names no library' if head else 'missing') + f': made for {want[2:]} ({p})')
    return True


def refine_after_rewrite(g, ex_dir, patched):
    """Keys of a provisional profile that need the rewritten modules: mem_used (dm/games/profile_guess.py), for a game without virtual address memory that sizes its heap as pool − used memory"""
    if any(k == 'mem_used' for k, _ in g.rows) or pg.npdm(ex_dir / 'ExeFS/main.npdm')[0]: return g
    used = pg.mem_used(patched, g.words('modules'))
    if not used: return g
    say(f'mem_used {used:#x}: the rewritten modules are larger than the 64 MB of used memory the game takes off the pool for its heap')
    return set_keys(g, {'mem_used': (f'{used:#x}', 'the rewritten modules are larger than the default 64 MB of used memory (Age of Calamity: 66.6 MB → 72 MB)')}, 'After the rewrite')


def heap_checked(g):   # the heap check's stamp in the profile ("# heap check: ..."): its result is kept, later runs skip it
    return any(l.startswith('# heap check: ') for l in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines())


def heap_check(repo, d, patched, work, env, logs, g):
    """The heap check of a provisional profile whose game has virtual address memory (an NPDM system resource) and sets a heap (dm/games/profile_guess.py heap_check_needed):
    its heap request decides the alias / heap split of the guest block, and only a run shows it (Wonder asks for 1.44 GB, Tears of the Kingdom for 2.09 GB, which Engage's split
    has no room for). The game boots in the Mac host (headless and silent, no shader libraries) until its first heap request is answered or fails, or until it has run
    HEAP_CHECK_NNMAIN_SECS in nnMain without one (Engage sets none): a few seconds. The keys go into the profile (profile_guess.py heap_signals), which is stamped"""
    PROG.stage('heap check')
    host, rd, log = work / 'tools/dmhost', work / 'tmp/heap_run', logs / 'heap_check.log'
    PROG.set(step='building the Mac host'); build_mac_host(repo, host, env, logs, g)
    rmtree(rd)
    for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
    # a fresh save and temporary storage, no shader libraries (only the heap request matters), the profile's shader map
    henv = dict(env, DM_ROMFS=str(d / 'RomFS'), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NVN_SHMAP=str(g.table('shader_map.tsv')))
    PROG.set(step='booting the game')
    t0, seen = time.time(), {}
    def done():   # the first heap request answered (or refused for good), or nnMain running for a while without one
        try: text = log.read_text(errors='replace')
        except OSError: return False
        if 'SetHeapSize OOM' in text or '[mem] heap ' in text: return True
        if '>>> nnMain' in text: seen.setdefault('nnmain', time.time())
        return time.time() - seen.get('nnmain', time.time()) > HEAP_CHECK_NNMAIN_SECS
    rc = boot_mac(host, patched, henv, rd, log, HEAP_CHECK_SECS, done)
    text = log.read_text(errors='replace'); rmtree(rd)
    sig = {k: v for k, v in pg.heap_signals(text, g).items() if g.get(k, '') != v[0]}
    for k, (v, why) in sig.items(): say(f'heap check: {k} {v}: {why}')
    m = re.search(r'\[mem\] heap ([0-9.]+ GB)', text)
    how = ('the heap request did not fit' if 'SetHeapSize OOM' in text else f'heap {m.group(1)}' if m else 'no heap set' if '>>> nnMain' in text
           else f'the host stopped before nnMain (exit {rc})' if rc is not None else 'no nnMain within the time limit')
    if sig: g = set_keys(g, sig, 'Heap check on the Mac')
    p = g.dir / 'game.tsv'; p.write_text(p.read_text(encoding='utf-8') + f'\n# heap check: {time.strftime("%Y-%m-%d")}, {how}' + (f'; set {", ".join(sig)}' if sig else '') + '\n', encoding='utf-8')
    if not m and '>>> nnMain' not in text and 'SetHeapSize OOM' not in text:   # stopped before any heap request: worth a look before the IPA is tried
        PROG.say(f'[RED] heap check: {how}; ---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=20)).rstrip('\n'))
    PROG.end(f'{how} in {hms(time.time() - t0)}' + (f'; set {", ".join(sig)}' if sig else ''))
    return type(g)(g.dir)


def data_rows(b):   # a table's data rows (without # comment lines: the header changes with the shader_split.py version, not with the game files)
    return [l for l in b.splitlines() if not l.startswith(b'#')]


def split_shaders(py, repo, roms, mains, pkg, env, logs, g, provisional=False, oodle=None):
    """The shader split into pkg, checked against the profile's tables. The package is reused when the tools, the profile and each version's main, RomFS (paths and sizes) and run-time capture
    are as at the last successful split. Returns the package's content key, the iOS shaders' input (None for a provisional profile or damaged files, which never reuse them): a split that runs
    again because a reader of another engine changed writes the same package, and the iOS shaders stay reused.
    oodle: for a game with Trinity packs (Pokémon Legends: Z-A; profile_guess.py shader_sources), a function that builds the Mac host and returns the split's DM_OODLE_HOST / DM_OODLE_MODULES; called only when the split runs"""
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    PROG.stage('shader split', step='checking the inputs')   # then a spinner: shader_split.py writes the package and the tables only at the end
    kf = pkg.with_name(pkg.name + '.key')
    key = None if provisional or ALLOW_DAMAGED else fingerprint(('files', tool_files(repo)), ('files', profile_files(g)),
        *[x for r, m in zip(roms, mains) for x in (('files', [m]), ('tree', r), ('files', sorted((r.parent / 'glslc_capture').glob('*.glslc'))),
                                                   ('files', sorted((r.parent / 'ue_shaders').glob('*.ushaderbytecode'))))])
    if key and pkg.is_dir() and (note := reused(kf, key)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return package_key(pkg)
    if oodle:   # the packs are decoded by the game's own Oodle in the Mac host (dm/host/hle_oodle.c)
        PROG.set(step='building the Mac host for the game\'s Oodle decoder'); env = dict(env, **oodle())
    PROG.set(step='splitting')
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names], logs / 'shader_split.log', env=env)
    m = re.search(r'shaders: (\d+) \(', (logs / 'shader_split.log').read_text(errors='replace'))
    if m and m.group(1) == '0':   # dmsh_build.py would make no library, and the iOS shader step stop with no word of why
        raise BuildError('the shader split found no shaders in main or the RomFS: the game keeps them in a form shader_split.py does not read; locate them (docs/agents) and add a reader')
    if provisional:   # a provisional profile takes the tables as they come
        for n in names: shutil.copyfile(tmp / 'tables' / n, g.table(n))
    else:
        # Data rows byte-identical to the profile's = the same game files the tables were built from; if not, stop (the profile's tables are left alone). Only data rows are compared: the header of Engage's profile tables was written by an older shader_split
        diff = [n for n in names if data_rows((tmp / 'tables' / n).read_bytes()) != data_rows(g.table(n).read_bytes())]
        if diff: raise BuildError(f'the split shader tables differ from game profile {g.id}\'s: {", ".join(diff)} (game files not the versions the profile registers?)')
    rmtree(kf); swap_in(tmp / 'pkg', pkg); rmtree(tmp)
    note = f'{int(m.group(1)):,} shaders' if m else ''
    if key: kf.write_text(f'{key}\n{note}\n')
    PROG.end(note)
    return key and package_key(pkg)


class ShaderWatch:
    """Progress of dmsh_build.py, read from the files it writes (it prints nothing until it ends). It works in three phases:
    the cache check (a hit restores <key>_b.json into the library directory b-<chain>/: the iOS one, or the Mac's for a Mac-only build; a miss writes <key>.bin into its temporary folder dmsh_* under WORK),
    the translation of the misses (<key>.spv in the temporary folder), and the compile, which takes the keys in a fixed order (sorted, the vertex shaders with pixel pairs last)
    and writes <key>_b.json again for every miss and every pixel shader: the furthest key written so far tells how far it got"""
    def __init__(self, g, out, tmp):   # out: the library directory b-<chain>/ (the iOS one, or the Mac's for a Mac-only build)
        keys = set()
        for ln in open(g.table('shader_map.tsv'), errors='replace'):   # the keys dmsh_build.py builds: column 4 of the rows from a shader package
            c = ln.rstrip('\n').split('\t')
            if len(c) > 4 and c[0].endswith('.bin'): keys.add(c[3])
        pairs = {ln.split()[0] for ln in open(g.table('shader_pairs.tsv'), errors='replace') if ln.strip() and not ln.startswith('#')}
        order = sorted(keys - pairs) + sorted(keys & pairs)
        self.pos, self.n = {k: i for i, k in enumerate(order)}, len(order)
        self.out, self.tmp, self.t0, self.t1, self.w = out, tmp, time.time(), None, None
        self.hits, self.fin, self.top = set(), set(), -1

    def written(self, keys, since):   # those of keys whose <key>_b.json in the output was written after since
        found = set()
        try:
            with os.scandir(self.out) as it:
                for e in it:
                    if e.name.endswith('_b.json') and e.name[:-7] in keys and e.stat().st_mtime > since: found.add(e.name[:-7])
        except OSError: pass
        return found

    def __call__(self):
        if not self.w:   # dmsh_build.py's temporary folder: the dmsh_* under WORK made since it started
            self.w = max((d for d in self.tmp.glob('dmsh_*') if d.stat().st_birthtime >= self.t0 - 2), key=lambda d: d.stat().st_birthtime, default=None)
        try: ext = collections.Counter(os.path.splitext(x)[1] for x in os.listdir(self.w)) if self.w else collections.Counter()
        except OSError: ext = collections.Counter()
        if self.t1 is None:
            self.hits |= self.written(self.pos.keys() - self.hits, self.t0 - 2)
            if len(self.hits) + ext['.bin'] < self.n and not ext['.spv'] and not ext['.metal']:
                return {'step': 'cache check', 'done': len(self.hits) + ext['.bin'], 'total': self.n, 'unit': 'shaders', 'note': f'{len(self.hits):,} cached'}
            self.t1 = time.time()   # the cache check is over: from now on a <key>_b.json is written by the compile
        if ext['.bin'] and not ext['.metal'] and ext['.spv'] < ext['.bin']:
            return {'step': 'translating', 'done': ext['.spv'], 'total': ext['.bin'], 'unit': 'shaders'}
        new = self.written(self.pos.keys() - self.fin, self.t1)
        self.fin |= new; self.top = max([self.top] + [self.pos[k] for k in new])
        return {'step': 'compiling' if ext['.bin'] else 'finishing', 'done': max(len(self.fin), self.top + 1), 'total': self.n, 'unit': 'shaders',
                'note': f'{ext[".bin"]:,} not cached' if ext['.bin'] else ''}


def toolchain():   # the compilers' versions, part of the shader stages' keys
    return ('cmd', ['xcodebuild', '-version']), ('cmd', ['spirv-cross', '--revision'])


def build_shaders(py, repo, sh, xlate, env, logs, g, pkg_key=None, mac=False):
    """The iOS shader libraries (dmsh_build.py, which builds the Mac's into sh/cache in the same run), or with mac only the Mac's (a Mac-only build: no IOSOUT). Returns the folder the host takes
    them from. Reused when the shader package (pkg_key, its content; None never reuses) and the chain's tools, translator binaries, profile and compilers are as at the last successful build:
    dmsh_build.py would only restore every shader from its cache (63,000 for Breath of the Wild: about 100 s). The key of the iOS libraries is sh/ios/b-<chain>.key, that of the Mac's
    sh/mac-b-<chain>.key (not in sh/cache, whose b-* entries dmsh_build.py reads as library folders), which an iOS build writes too, so a Mac-only build after it is reused; either is taken only
    while the Mac libraries carry a full build's stamp (inputs.tsv) for c1_full.tsv and cb_extent.tsv to name (chain_tables).
    An iOS build after a Mac-only one translates the shaders again (dmsh_build.py's per-shader cache keys include the iOS compile convention), but takes the Mac libraries from its
    Metal compile cache (dmsh_mtlcache.py), so only the iPhone's are compiled"""
    ios, log = sh / 'ios', logs / 'dmsh_build.log'
    PROG.stage('Mac shaders' if mac else 'iOS shaders')
    chain = chain_of(repo)
    out = sh / 'cache' if mac else ios
    libs, kf, kmac = out / f'b-{chain}', ios / f'b-{chain}.key', sh / f'mac-b-{chain}.key'
    pattern = '*_b.metal' if mac and env.get('DM_METAL_RUNTIME') == '1' else '*_b.metallib'
    if pkg_key:
        PROG.set(step='checking the inputs')
        key = fingerprint(pkg_key, ('files', chain_files(repo)), ('stat', xlate.parent), ('files', profile_files(g)), *toolchain(), str(xlate),
                          {k: env.get(k) for k in ('XLATE_DEBUG', 'XLATE_REDUCED')}, env=env)
        note = next((n for k in ((kmac, kf) if mac else (kf,)) if (n := reused(k, key)) is not None), None)
        # the libraries are the last build's, so are tables that name their stamp; a missing or stale one is made from that build's Mac outputs, and without them the step runs again
        if next(libs.glob(pattern), None) and note is not None and chain_tables(py, repo, sh, env, logs, g, chain):
            PROG.end(f'{note}, reused (inputs unchanged)'); return out
    else: key = None
    rmtree(kmac)
    if not mac: rmtree(kf)
    try: PROG.watch(ShaderWatch(g, libs, Path(env['WORK'])))
    except Exception: pass   # progress never stops the build (a missing table is dmsh_build.py's to report)
    benv = dict(env, XLATE=str(xlate), IOSOUT=str(ios))
    if mac: del benv['IOSOUT']   # dmsh_build.py builds the iOS libraries only with IOSOUT
    run([py, repo / 'dm/gfx/tools/own_compilers.py', sh / 'pkg', sh / 'cache'], log, env=benv)   # dmsh_build.py with this Mac's Metal compiler and spirv-cross
    PROG.watch(None)
    if not chain_tables(py, repo, sh, env, logs, g, chain): raise BuildError(f'no stamped Mac shader outputs in {sh / "cache" / f"b-{chain}"} (inputs.tsv): dmsh_build.py did not finish a full build')
    what = 'Mac' if mac else 'iOS'
    n = sum(1 for _ in libs.glob(pattern)) if libs.is_dir() else 0
    if n == 0: raise BuildError(f'no {what} shader libraries in {libs}')
    say(f'{n} {what} shader libraries ({libs})')
    t = log.read_text(errors='replace')
    if own := re.search(r'^note: (built with this Mac\'s compilers.*)$', t, re.M): say(own.group(1))
    m, c = re.search(r'shaders (\d+): ok (\d+)', t), re.search(r'shader cache: hit (\d+)', t)
    note = ''
    if m:
        total, ok = int(m.group(1)), int(m.group(2))
        failed = f'; {total - ok:,} failed, reasons at the end of {log}' if ok < total else ''
        note = f'{ok:,} of {total:,} shaders ready{failed}'
        PROG.end(f'{ok:,} of {total:,} shaders ready' + (f' ({int(c.group(1)):,} from the cache)' if c else '') + failed)
    else: PROG.end()
    if key:
        for k in (kmac,) if mac else (kf, kmac): k.write_text(f'{key}\n{note}\n')
    return out


def layer_shaders(py, repo, api, name, roots, sh, key, logs, env, mac):
    """What build_vk_shaders and build_gl_shaders share: dm/gfx/tools/<api>_shaders.py on the roots → sh/<api>/ for the Mac and, unless mac (a Mac-only build), sh/ios/<api>/ for the iPhone.
    Returns the folder the host (DM_MSL_DIR) or build_ipa.sh (MSL_IOS) takes the libraries from. key: the inputs' fingerprint (None never reuses). The iOS libraries' key is sh/ios/<api>.key,
    the Mac's sh/<api>.key, which an iOS build writes too (it builds both), so a Mac-only build is reused after either; an iOS build after a Mac-only one translates again.
    Otherwise the old output is deleted first, so a failed run leaves nothing for build_ipa.sh or the host to pick up; the translator's temporary files go to WORK (on the NSP folder's volume)"""
    ios = sh / 'ios'; out = sh if mac else ios
    kf, kmac, lib, log = ios / f'{api}.key', sh / f'{api}.key', out / api / f'{api}_shaders.metallib', logs / f'{api}_shaders.log'
    note = next((n for k in ((kmac, kf) if mac else (kf,)) if (n := reused(k, key)) is not None), None) if key else None
    if lib.is_file() and note is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return out
    PROG.set(step='translating')
    rmtree(kmac); rmtree(sh / api)
    if not mac: rmtree(kf); rmtree(ios / api)
    run([py, repo / f'dm/gfx/tools/{api}_shaders.py', *roots, sh] + ([] if mac else ['--ios', ios]), log, env=dict(env, TMPDIR=env['WORK']))
    if not lib.is_file(): raise BuildError(f'no {"Mac" if mac else "iOS"} {name} shader library in {lib.parent}')
    m = re.search(r'macosx: (\d+)' if mac else r'iphoneos: (\d+)', log.read_text(errors='replace'))
    note = f'{int(m.group(1)):,} shaders for the Mac' + ('' if mac else ' and iOS') if m else ''
    if key:
        for k in (kmac,) if mac else (kf, kmac): k.write_text(f'{key}\n{note}\n')
    PROG.end(note)
    return out


def build_vk_shaders(py, repo, roots, sh, env, logs, g, reuse=True, mac=False):
    """A Vulkan game's SPIR-V shaders (roots: the RomFS, and a run-time capture folder) → one Metal library each for the Mac (sh/vk/) and the iPhone (sh/ios/vk/, what build_ipa.sh takes from MSL_IOS;
    not with mac), replacing the NVN split and chain build (layer_shaders). Reused when the roots (paths and sizes), the tools, the profile and the compilers are as at the last successful run (reuse False: never)"""
    PROG.stage('Vulkan shaders', step='checking the inputs')
    key = fingerprint(*[('tree', r) for r in roots], ('files', tool_files(repo)), ('files', profile_files(g)), *toolchain(), env=env) if reuse else None
    return layer_shaders(py, repo, 'vk', 'Vulkan', roots, sh, key, logs, env, mac)


def build_gl_shaders(py, repo, roots, sh, env, logs, g, reuse=True, mac=False):
    """An OpenGL game's GLSL sources (roots: the RomFS, main, a run-time capture folder) → one Metal library each for the Mac (sh/gl/) and the iPhone (sh/ios/gl/, what build_ipa.sh takes from MSL_IOS;
    not with mac), replacing the NVN split and chain build, as build_vk_shaders does for a Vulkan game; glslang (glslangValidator) compiles the GLSL to SPIR-V for spirv-cross"""
    PROG.stage('GL shaders', step='checking the inputs')
    if not shutil.which('glslangValidator', path=env['PATH']): raise BuildError('glslangValidator not found (brew install glslang): it compiles an OpenGL game\'s GLSL shaders')
    key = fingerprint(*[('tree', r) if Path(r).is_dir() else ('files', [r]) for r in roots], ('files', tool_files(repo)), ('files', profile_files(g)), *toolchain(),
                      ('cmd', ['glslangValidator', '--version']), env=env) if reuse else None
    return layer_shaders(py, repo, 'gl', 'GL', roots, sh, key, logs, env, mac)


class IpaWatch:
    """Progress of build_ipa.sh, from its step lines ("[time] generate Gen/ …", "xcodebuild …", "pack IPA …", then pack_ipa.py's "pack: N entries, X bytes") and the files it writes:
    the CompileC lines in xcodebuild's build.log (one per source file: Gen/src, Gen/main/main.c, Gen/embed.S and the app's Sources), then the bytes of the archive pack_ipa.py is
    writing (<IPA>.partial, renamed at the end). Its two halves are the stages "app build" and "IPA pack" """
    XCODE = {'CompileC': 'compiling', 'Ld': 'linking', 'CpResource': 'copying resources', 'CompileAssetCatalogVariant': 'asset catalog', 'ProcessInfoPlistFile': 'Info.plist'}

    def __init__(self, repo, w, out_ipa):
        self.ios, self.o, self.part = repo / 'dm/ios', w / 'build/ios/pkg', out_ipa.with_name(out_ipa.name + '.partial')
        self.last = out_ipa.stat().st_size if out_ipa.is_file() else None   # the last IPA's size: the expected size of the new archive
        self.state, self.t, self.off, self.compiled, self.what, self.total, self.most, self.lock = None, 0, 0, 0, '', None, 0, threading.Lock()

    def line(self, line):   # a line of build_ipa.sh's output
        s = re.sub(r'^\[[\d:]+\] ', '', line)
        with self.lock:   # a poll sees a step change whole
            if s.startswith('generate Gen/'): self.state = 'gen'; PROG.set(step='generating sources')
            elif s.startswith('xcodebuild'):
                units = len(os.listdir(self.ios / 'Gen/src')) + 2 + sum(1 for p in (self.ios / 'Sources').iterdir() if p.suffix in ('.c', '.m'))   # + Gen/main/main.c and Gen/embed.S
                self.state, self.t = 'xcode', time.time(); PROG.set(step='xcodebuild', done=0, total=units, unit='files', est=True)   # an incremental build compiles fewer
            elif s.startswith('pack IPA'):
                try: self._poll()   # the rest of build.log (xcodebuild writes it in big blocks)
                except OSError: pass
                PROG.end(f'{self.compiled:,} file{"" if self.compiled == 1 else "s"} compiled')
                self.state, self.total, self.most = 'list', None, 0; PROG.stage('IPA pack', poll=self.poll, step='listing the files')
            elif self.state == 'list' and (m := re.match(r'pack: (\d+) entries, (\d+) bytes', s)):   # the files' bytes: the archive's size when nothing deflates, else the last IPA's size is closer
                self.state, self.total = 'pack', self.last or int(m.group(2)); PROG.set(step='packing', unit='B', note=f'{int(m.group(1)):,} entries')

    def poll(self):   # from the poll thread
        with self.lock: return self._poll()

    def _poll(self):
        state = self.state
        if state == 'xcode':
            log = self.o / 'build.log'
            if log.stat().st_mtime < self.t - 1: return None   # still the last build's log
            if log.stat().st_size < self.off: self.off = self.compiled = 0
            with open(log, 'rb') as f: f.seek(self.off); chunk = f.read()
            chunk = chunk[:chunk.rfind(b'\n') + 1]; self.off += len(chunk)
            for ln in chunk.split(b'\n'):
                w = ln.split(b' ', 1)[0].decode(errors='replace')
                if w == 'CompileC': self.compiled += 1
                self.what = self.XCODE.get(w, self.what)
            r = {'done': self.compiled, 'note': self.what}
        elif state == 'pack':
            try: self.most = max(self.most, self.part.stat().st_size)   # never backwards: the archive is renamed at the end
            except OSError: pass
            r = {'done': self.most, 'total': self.total, 'est': True}
        else: return None
        return r


def env_file_pairs(path):   # an environment file's switches (one KEY=VALUE per line, # starts a comment), read as the iOS app reads them (dm/ios/Sources/main.m)
    out = {}
    try: text = Path(os.path.expanduser(path)).read_text(errors='replace')
    except OSError as e: raise BuildError(f'cannot read the environment file (--env-file / DM_ENV_FILE): {e}') from e
    for ln in text.splitlines():
        k, eq, v = ln.strip().partition('=')
        if k.strip() and eq and not k.startswith('#'): out[k.strip()] = v
    return out


def mac_run(py, repo, work, game, patched, msl, aoc, gfx, lang, env, logs, g, env_file=''):
    """The last stage of a Mac-only build (PLATFORM 'mac'): the Mac host for the profile (build_mac_host, into <id>_build/tools/dmhost, where the run-time captures build it too; DM_GAMES_DIR /
    DM_GAMES_FIRST in env pick a provisional or derived profile) and the button-icon font the host gives a game for the Nintendo extension shared font (make_ext_font.py, into <id>_build/font,
    made again when the tool changed). Returns the command that runs the game in a window with sound (docs/development.md, Build by hand (Mac), step 5): its save and temporary storage in
    <id>_build/run (kept from run to run), the extracted RomFS, the Mac shader libraries (msl), the DLC, the profile's shader map (a host outside dm/host does not find it beside itself;
    c1_full.tsv and cb_extent.tsv are read beside it), the game language, and the switches of an environment file (DM_ENV_FILE / --env-file)"""
    PROG.stage('Mac host', step='building')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    PROG.set(step='button-icon font')
    font = work / 'font'; ttf, kf = font / 'nintendo_ext.ttf', font / 'nintendo_ext.key'
    key = fingerprint(('files', [repo / 'dm/tools/make_ext_font.py']))
    if not (ttf.is_file() and reused(kf, key) is not None):
        rmtree(kf); font.mkdir(parents=True, exist_ok=True)
        run([py, repo / 'dm/tools/make_ext_font.py', ttf], logs / 'make_ext_font.log', env=env)
        kf.write_text(f'{key}\n')
    rd = work / 'run'
    for d in ('save', 'temp'): (rd / d).mkdir(parents=True, exist_ok=True)
    renv = {'DM_ROOT': work, 'DM_ROMFS': game / 'RomFS', 'DM_SAVE': rd / 'save', 'DM_TEMP': rd / 'temp', 'DM_MSL_DIR': msl, 'DM_FONT_DIR': font}
    if aoc: renv['DM_AOC_DIR'] = aoc
    if gfx == 'nvn': renv['DM_NVN_SHMAP'] = g.table('shader_map.tsv')
    renv.update(DM_LANG=lang, DM_WINDOW=1, DM_AUDIO=1, DM_METAL_RUNTIME=1)
    if env_file: renv.update(env_file_pairs(env_file))
    PROG.end(str(host))
    q = lambda v: shlex.quote(str(v))
    return f'cd {q(rd)} && ' + ' '.join(f'{k}={q(v)}' for k, v in renv.items()) + f' {q(host)} {q(patched)}'


def find_repo(here):   # this file lives in the repository's scripts/; walk up to the level that has dm/tools/nx_extract/Program.cs
    for d in here.parents:
        if (d / 'dm/tools/nx_extract/Program.cs').exists(): return d
    raise BuildError(f'cannot find the repository root above {here} (no dm/tools/nx_extract)')


def parse_args(argv):
    """Command-line options: when given they override "Settings"; F5 in Spyder passes none, so "Settings" is used as is. -h shows the help"""
    import argparse
    ap = argparse.ArgumentParser(prog='build_ipa_from_nsp.py',
        description='Build an unsigned IPA or prepare an Xcode project from a game\'s NSPs; a game with no profile is surveyed and gets a provisional profile.',
        epilog='example: python3 scripts/build_ipa_from_nsp.py ~/nsp/"Super Mario Bros Wonder" -k ~/keys/prod.keys')
    ap.add_argument('nsp_dir', nargs='?', help='folder holding one game\'s NSPs (default: the NSP_DIR setting, else asked)')
    ap.add_argument('-k', '--keys', help='path of prod.keys (default: the PROD_KEYS setting, else asked)')
    ap.add_argument('-p', '--package', help='version packed into the IPA, e.g. v327680 / v0 (default: the newest in the folder)')
    ap.add_argument('-o', '--out', help='output IPA (default: <the profile\'s ipa> in the NSP folder)')
    ap.add_argument('--app-name', help='app name on the home screen (default: the game name in the base NSP)')
    ap.add_argument('--bundle-id', help='the app\'s bundle ID (default: saved Xcode ID when preparing again, else org.nxnative.<base game title ID in lowercase>)')
    ap.add_argument('--prepare-xcode', action='store_true', help='prepare a per-game Xcode project and add it to the shared workspace instead of packing an IPA (requires keeping extracted files)')
    ap.add_argument('--platform', choices=('ios', 'mac'), help='ios = the IPA (or the Xcode project); mac = for development: build only what running the game on this Mac needs (the Mac shaders and the Mac host, '
                    'no iOS shaders, Xcode project or IPA), keep the extracted files and print the command that runs the game (default: the PLATFORM setting, ios)')
    ap.add_argument('--team-id', help='Apple developer team ID (default: TEAM_ID setting, DM_TEAM_ID environment, or saved Xcode selection)')
    ap.add_argument('--xcode-workspace', help='workspace collecting prepared games (default: NxNative.xcworkspace beside the NSP folder; use with --prepare-xcode)')
    ap.add_argument('-l', '--language', help='game language, a code or a name: ja, fr, de, es, it, nl, pt, ru, ko, zh-Hans, zh-Hant, en-GB, fr-CA, es-419, pt-BR, Japanese, ... (default: the LANGUAGE setting, else English); '
                    'it must be one the game supports, which the console lists')
    ap.add_argument('--keep-extracted', action=argparse.BooleanOptionalAction, help='keep the extracted game files after the IPA is built, for the next build (default: the KEEP_EXTRACTED setting, on)')
    ap.add_argument('--env-file', help='environment switches for a diagnostic build (one KEY=VALUE per line)')
    ap.add_argument('--show-fps', action=argparse.BooleanOptionalAction, help='show the game\'s frame rate on the iPhone, frames on screen per second at the top left (default: the SHOW_FPS setting, on)')
    ap.add_argument('--survey-only', action='store_true', help='a game with no profile: only the survey, no provisional profile and no IPA')
    ap.add_argument('--allow-damaged', action='store_true', help='a damaged NSP (files failing the NCA hash check): write them as read and go on instead of stopping')
    ap.add_argument('--profile-out', help='where the provisional profile goes (default: <NSP folder>/<title ID>_build/profile)')
    ap.add_argument('--provisional', action='store_true', help='build from a provisional profile even when dm/games has one for the game (tests the guesses)')
    ap.add_argument('--no-heap-check', action='store_true', help='a provisional profile: skip the heap check on the Mac (the alias / heap split stays a guess)')
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), help='stage lines on the console: bar = one line per stage, redrawn in place; lines = a plain line every BEAT seconds; json = newline-delimited JSON events (no prompts); off = only each stage\'s start and end '
                    '(default: the PROGRESS setting; auto = bar in a terminal or Spyder, lines when the output goes to a file or a pipe)')
    return ap.parse_args(argv)


def report_profile_status(g, reg, pkg_ver, derived, provisional, artifact='IPA'):
    """Keep build-specific warnings visible for the IPA, the Xcode project and a Mac-only build"""
    disp = g.builds()[pkg_ver]['display'] if derived else ''
    if derived:   # the modules whose build no registered version has: the rewriter's cuts and the host's fixes made for a registered build's code do not reach them
        known = {(m, b) for row in reg.builds().values() for m, b in row.items() if m != 'display'}
        other = [m for m, b in g.builds()[pkg_ver].items() if m != 'display' and (m, b) not in known]
        registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in reg.builds().items())
        say(f'[RED] this {artifact} uses {pkg_ver} ({disp}), which game profile {reg.id} was not brought up on ({registered_builds}): '
            'its settings, with this version\'s own shaders' + (f'; {", ".join(other)} {"is a build" if len(other) == 1 else "are builds"} the profile does not register, so the fixes made for '
            'the registered builds\' code (patch_nso.py, the host) do not apply there and the game may not run as well' if other else '; every module is a registered build'))
    if provisional:
        say(f'[RED] this {artifact} was built from a provisional game profile and the game may not boot: {g.dir}')
        say(f'  bring-up on the Mac: DM_GAMES_DIR="{g.dir.parent}" make -C dm/host GAME={g.id} dmhost, see docs/agents')


def main(args=None):
    args = args or parse_args([])
    PROG.mode = progress_mode(args.progress or PROGRESS)
    global ALLOW_DAMAGED; ALLOW_DAMAGED = args.allow_damaged or ALLOW_DAMAGED
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    repo = find_repo(here)
    prepare_xcode = args.prepare_xcode or PREPARE_XCODE
    platform = args.platform or PLATFORM
    if platform not in ('ios', 'mac'): raise BuildError(f'PLATFORM / --platform must be ios or mac: {platform}')
    mac = platform == 'mac'   # a Mac-only build for development: no iOS shaders, Xcode project or IPA (mac_run)
    if mac and prepare_xcode: raise BuildError('--prepare-xcode makes an iPhone project: it cannot be combined with --platform mac')
    if mac and not apple_silicon(): raise BuildError('--platform mac runs the game in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    keep = KEEP_EXTRACTED if args.keep_extracted is None else args.keep_extracted
    if mac and not keep: say('keeping the extracted game files all the same: the game runs from them on the Mac'); keep = True
    if prepare_xcode and not keep: raise BuildError('--prepare-xcode requires --keep-extracted: Xcode builds from the extracted files')
    if args.xcode_workspace and not prepare_xcode: raise BuildError('--xcode-workspace requires --prepare-xcode')
    nsp_dir = Path(os.path.expanduser(args.nsp_dir or NSP_DIR or ask('NSP folder: ', 'the NSP folder'))).resolve()
    keys = Path(os.path.expanduser(args.keys or PROD_KEYS or ask('path of prod.keys: ', 'prod.keys (-k)'))).resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'no such file: {keys}')
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod   # game profiles
    global pg; import profile_guess as pg   # dm/games: a provisional profile's keys from the game files and from the heap check
    sys.path.insert(0, str(repo / 'dm/tools')); global nacp; import nacp   # the game name in the NSP's control data
    lang_in = args.language or LANGUAGE   # checked against the game's languages once its control data is extracted
    want_lang = language_code(lang_in) if lang_in else 'en-US'
    if not want_lang: raise BuildError(f'unknown game language {lang_in!r}; give one of {language_list([c for c, _ in nacp.LANGUAGES])}, by code or by name')
    env = dict(os.environ, PATH=os.pathsep.join([os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')] + TOOL_PATHS))
    env.setdefault('USER', getpass.getuser())   # xcodegen fails without USER (Couldn't find current username)
    env.setdefault('J', str(os.cpu_count() or 8))   # shader_split.py's and dmsh_build.py's workers (their default is 8)
    py = sys.executable
    app, fam = scan_nsps(nsp_dir)
    if app != 0x0100965017338000: raise BuildError('this target supports only Super Mario Party Jamboree (0100965017338000)')
    if args.package != 'v458752': raise BuildError('this target supports only 2.3.0 (v458752)')
    if fam['dlc']: raise BuildError('DLC is not validated by this target')
    force = args.provisional or PROVISIONAL   # a provisional profile even when dm/games has one (testing the guesses)
    g, provisional, shadowed = None if force else gamemod.by_title(app), False, False
    if g is None:   # no game profile: survey, then (unless --survey-only) a provisional profile outside the repository and a best-effort IPA
        missing = [m for m in ('lz4.block', 'capstone') if not _importable(m)]
        if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)}')
        if not shutil.which('dotnet', path=env['PATH']): raise BuildError('dotnet not found (.NET SDK 10)')
        pdir = Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve() if (args.profile_out or PROFILE_OUT) else nsp_dir / f'{app:016X}_build' / 'profile'
        if (pdir / f'{app:016X}' / 'game.tsv').exists() and not (args.survey_only or SURVEY_ONLY):
            say(f'no game profile for {app:016X}: reusing the provisional profile from the last run (delete it to survey again)')
        else:
            facts = survey(nsp_dir, app, fam, keys, repo, py, env)
            if args.survey_only or SURVEY_ONLY: return
            facts['survey'] = str(nsp_dir / f'{app:016X}_survey' / 'survey.md')
            write_profile(pdir / f'{app:016X}', app, facts)
        # the profile folder joins the search path of game.py, which the shader tools, make and build_ipa.sh all use (DM_GAMES_DIR)
        os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
        if force and gamemod.by_title(app):   # dm/games has the game too: every tool (game.py, make, build_ipa.sh) takes the provisional profile first (DM_GAMES_FIRST)
            os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'; shadowed = True
        g, provisional = gamemod.load(f'{app:016X}'), True
        say('[RED] provisional game profile: values guessed from the survey, the game may not boot; bring it up per docs/agents after packing')
    say(f'game profile: {g.id}{" (provisional: " + str(g.dir) + ")" if provisional else ""}')
    builds, need = g.builds(), g.words('shader_versions')
    have, derived = nsp_versions(fam, builds), False
    if provisional:   # the version the survey saw
        pkg_ver = args.package or PACKAGE or g.get('package')
        if pkg_ver not in builds: raise BuildError(f"the version to pack (-p / PACKAGE) must be one the game profile registers: {', '.join(builds)}")
    else:   # the newest version in the folder, or the one asked for. The profile is used as it is for a version it registers when every version its shader tables were made from is there;
        # any other version (no update, another update) is packed from a profile derived from it (derive_profile), with its own shaders
        pkg_ver = args.package or PACKAGE or max(have, key=vnum)
        if pkg_ver not in have: raise BuildError(f'the version to pack (-p / PACKAGE) is not in the folder, which holds {", ".join(sorted(have, key=vnum))}')
        if pkg_ver not in builds:
            registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in builds.items())
            derived = f'{pkg_ver} is not a version game profile {g.id} registers ({registered_builds})'
        elif not all(v in have for v in need): derived = f'the folder has no {", ".join(v for v in need if v not in have)}, whose files game profile {g.id}\'s shader tables were made from'
        if derived: say(f'{derived}: packing {pkg_ver} from a profile derived from it')
    versions = [pkg_ver] if derived else need + ([pkg_ver] if pkg_ver not in need else [])
    out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else nsp_dir / g.get('ipa')   # by default in the NSP folder, next to the NSPs and <id>_build/
    if shadowed and not (args.out or OUT_IPA): out_ipa = out_ipa.with_name(out_ipa.stem + '-provisional.ipa')   # never over the IPA of the game's real profile
    work = nsp_dir / f'{g.id}_build'
    project = work / 'xcode/DmIOS.xcodeproj'
    workspace = Path(os.path.expanduser(args.xcode_workspace or XCODE_WORKSPACE)).resolve() if (args.xcode_workspace or XCODE_WORKSPACE) else nsp_dir.parent / 'NxNative.xcworkspace'
    if prepare_xcode and workspace.suffix != '.xcworkspace': raise BuildError('--xcode-workspace must end in .xcworkspace')
    saved_signing = {}
    if prepare_xcode:
        sys.path.insert(0, str(repo / 'dm/ios/tools')); import xcode_projects
        try:
            saved_signing = xcode_projects.signing_settings(project)
            # Preserve the user's selections when migrating the earlier single generated project.
            legacy_config = repo / 'dm/ios/Gen/dm.xcconfig'
            if not saved_signing and legacy_config.exists() and f'/gen/{g.id}/' in legacy_config.read_text():
                saved_signing = xcode_projects.signing_settings(repo / 'dm/ios/DmIOS.xcodeproj')
        except ValueError as e: raise BuildError(str(e)) from e
    say('Mac-only build for development: the Mac shaders and the Mac host, no iOS shaders, Xcode project or IPA' if mac else f'Xcode project: {project}' if prepare_xcode else f'output IPA: {out_ipa}')
    say(f'work folder (extracted game files in extract/, {"kept for the next build" if keep else "deleted after the IPA is built"}): {work}')
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_GAME picks the profile; DM_ROOT / WORK point at <id>_build: the tools' default paths and temporary directories all stay inside it
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate, mac)
    picks = pick_nsps(fam, have, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = sum({p.stat().st_size for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 6   # extraction + shader cache + slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'less than {gb:.0f} GB free on the volume of {work}')
    heapck = provisional and HEAP_CHECK and not args.no_heap_check and not heap_checked(g)   # the heap check, when the game needs one (once per provisional profile: it stamps the profile)
    PROG.plan(1 + len(versions) + bool(fam['dlc']) + 4 - (prepare_xcode or mac))   # prepare Xcode (a Mac-only build: the Mac host) replaces app build + IPA pack with one stage
    PROG.stage('build nx_extract')
    nx = build_nx_extract(repo, work, logs, env)
    PROG.end()
    dirs, clone = {}, None
    for ver in versions:   # the base game is extracted alone first; later updates clone against its RomFS
        b, u = picks[ver]
        dirs[ver] = extract(nx, keys, ex, ver, b, u, clone, logs, g)
        if u is None and clone is None and can_clone(ex): clone = dirs[ver] / 'RomFS'
    bad = [] if derived else [f'{v}: {x}' for v in versions for x in build_mismatch(dirs[v], v, g)]   # every module's build ID must be the one the profile registers for its version
    if bad: raise BuildError('unsupported executable build: ' + '; '.join(bad))
    pins = json.loads((repo / 'assets/expected-2.3.0.json').read_text())
    for name, pin in pins['modules'].items():
        data = (dirs[pkg_ver] / 'ExeFS' / name).read_bytes()
        if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise BuildError(f'original ExeFS hash mismatch: {name}; use unmodified 2.3.0 inputs')
    if bad and provisional: raise BuildError(f'not the builds provisional game profile {g.id} registers: {"; ".join(bad)}')
    if bad:   # files of other builds under a registered version's tag: packed like any other version
        derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'
        say(f'{derived}: packing {pkg_ver} from a profile derived from it')
        versions, dirs = [pkg_ver], {pkg_ver: dirs[pkg_ver]}
    reg = g   # the profile as registered (a derived build reports what it lacks against it)
    if derived:   # the derived profile goes where a provisional one would, and every tool (game.py, make, build_ipa.sh) takes it first (DM_GAMES_FIRST)
        pdir = Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve() if (args.profile_out or PROFILE_OUT) else nsp_dir / f'{app:016X}_build' / 'profile'
        disp = derive_profile(g, pdir / g.id, pkg_ver, dirs[pkg_ver], g.words('modules'))
        os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
        os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'
        g = gamemod.load(g.id)
        say(f'derived game profile for {pkg_ver} ({disp}): {g.dir}')
    sv = versions if derived else need   # the versions whose files the shaders come from
    game = dirs[pkg_ver]
    # how the shaders are got follows from the packed version's files (profile_guess.py shader_sources): the graphics API, and the shaders made only at run time
    src = pg.build_sources(game / 'ExeFS', game / 'RomFS', g.words('modules'))
    gfx = src['graphics'][0]   # nvn, vulkan (Hades: the host's Vulkan layer, SPIR-V shaders) or gl (Digimon Story Cyber Sleuth: the host's GL layer, GLSL sources)
    glslc = gfx == 'nvn' and src['glslc_capture'][0]   # shaders the game compiles at run time, captured in the Mac host (capture_glslc)
    vkcap = gfx == 'vulkan' and src['vk_capture'][0]   # SPIR-V shaders the game generates at run time, captured in the Mac host (capture_shaders)
    glcap = gfx == 'gl' and src['gl_capture'][0]   # GLSL the game puts together at run time (LÖVE), captured in the Mac host (capture_shaders)
    uedec = gfx == 'nvn' and src['ue_decode'][0]   # Unreal Engine shaders, Oodle-compressed in the paks' shader code archives, decoded in the Mac host (decode_ue_shaders)
    if gfx == 'vulkan': say(f'Vulkan game ({src["graphics"][1]}): vk_shaders.py translates the shaders instead of the NVN shader chain')
    if gfx == 'gl': say(f'OpenGL game ({src["graphics"][1]}): gl_shaders.py translates the shaders instead of the NVN shader chain')
    for on, k in ((glslc, 'glslc_capture'), (vkcap, 'vk_capture'), (glcap, 'gl_capture'), (uedec, 'ue_decode'), (gfx == 'nvn' and src['oodle_packs'][0], 'oodle_packs'), (gfx == 'nvn' and src['ue_legacy'][0], 'ue_legacy')):
        if on: say(src[k][1])
    PROG.add(glslc + vkcap + glcap + uedec + (gfx == 'nvn'))
    aoc = extract_aoc(nx, keys, repo, work, app, fam, logs)
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])   # the folder the base game was extracted to alone
    # the app name and bundle ID both come from the base NSP: the game name in its control data, and a bundle ID from its title ID; a setting / the command line overrides them
    app_name = args.app_name or APP_NAME or nacp.name(base / 'control.nacp')
    if not app_name and not mac: raise BuildError(f'no game name in the base extraction ({base / "control.nacp"}; an older extraction did not save it: delete extract/ and run again, or give --app-name)')
    bundle_id = args.bundle_id or BUNDLE_ID or saved_signing.get('PRODUCT_BUNDLE_IDENTIFIER') or f'org.nxnative.{app:016x}'
    team_id = args.team_id or TEAM_ID or env.get('DM_TEAM_ID') or saved_signing.get('DEVELOPMENT_TEAM') or '0000000000'
    if prepare_xcode:
        try: xcode_projects.check_bundle_id(workspace, project, bundle_id)
        except ValueError as e: raise BuildError(str(e)) from e
    # the app version is the packed game's display version: the update's control data when an update is packed (nx_extract saves it next to the RomFS), else the base game's
    app_version = nacp.version(game / 'control.nacp') or nacp.version(base / 'control.nacp')
    if not mac: say(f'app name: {app_name}; version: {app_version or "1.0"}; bundle ID: {bundle_id}')
    # the game language: Horizon gives the game one of the languages its control data lists (the packed version's, which an update may extend), so the one packed must be among them
    have = nacp.languages(game / 'control.nacp') or nacp.languages(base / 'control.nacp')
    lang = game_language(want_lang, bool(lang_in), have)
    say(f'game language: {lang} ({dict(nacp.LANGUAGES)[lang]})' + (f', the game has no {want_lang}' if lang != want_lang else '')
        + (f'; the game\'s languages: {" ".join(have)}' if have else '; the control data lists no languages (not checked)') + (' (DM_LANG=<code> in the run command changes it)' if mac else ' (DM_LANG=<code> in the app\'s Documents/env.txt changes it)'))

    patched = work / f'patched_{pkg_ver}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs, g)
    chain_header(py, repo, env, logs)   # before any host is built
    if provisional or derived:   # the keys that need the rewritten modules, then (provisional) the heap check (both only change keys the host is built with, not the rewrite or the shader steps)
        provisional_prep(py, repo, env, logs, g)
        g = refine_after_rewrite(g, game, patched)
        if provisional and heapck and pg.heap_check_needed(game / 'ExeFS'):
            if apple_silicon(): PROG.add(1); g = heap_check(repo, game, patched, work, env, logs, g)
            else: say('no heap check: it boots the game in the Mac host, which needs a Mac with Apple silicon; the profile keeps its guessed heap keys')
    if glslc: capture_glslc(repo, {v: dirs[v] for v in sv}, pkg_ver, patched, work, env, logs, g)
    if uedec: decode_ue_shaders(repo, {v: dirs[v] for v in sv}, pkg_ver, patched, work, env, logs, g)
    sh = work / 'shader'
    if gfx == 'vulkan':   # the packed version's merged RomFS holds the SPIR-V files the game loads; a capture holds those it generates
        cap = capture_shaders(repo, game, patched, work, env, logs, g, 'vk') if vkcap else None
        ios = build_vk_shaders(py, repo, [game / 'RomFS'] + ([cap] if cap else []), sh, env, logs, g, reuse=not (provisional or derived or ALLOW_DAMAGED), mac=mac)
    elif gfx == 'gl':   # the GLSL sources in the packed version's archives and in its main (the game compiles them at run time; the host finds each by the hash of its text), and a capture of those it puts together
        cap = capture_shaders(repo, game, patched, work, env, logs, g, 'gl') if glcap else None
        ios = build_gl_shaders(py, repo, [game / 'RomFS', game / 'ExeFS/main'] + ([cap] if cap else []), sh, env, logs, g, reuse=not (provisional or derived or ALLOW_DAMAGED), mac=mac)
    else:
        # Trinity packs and legacy Unreal paks: the split decodes them with the packed version's own decoder (Oodle, LZ5), in the Mac host built for the profile
        oodle = (lambda: {'DM_OODLE_HOST': str(build_mac_host(repo, work / 'tools/dmhost', env, logs, g)), 'DM_OODLE_MODULES': str(patched)}) if src['oodle_packs'][0] or src['ue_legacy'][0] else None
        own = bool(provisional or derived)   # the shader tables come from this build's split (the profile's are checked otherwise)
        pkey = split_shaders(py, repo, [dirs[v] / 'RomFS' for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g, own, oodle)
        ios = build_shaders(py, repo, sh, xlate, env, logs, g, pkey, mac)
    if mac:   # a Mac-only build ends with the Mac host and the command that runs the game
        cmd = mac_run(py, repo, work, game, patched, ios, aoc, gfx, lang, env, logs, g, args.env_file or DM_ENV_FILE)
        rmtree(work / 'tmp')
        say(f'done: the game runs on the Mac with this command (its save in {work / "run/save"}, kept from run to run; DM_WINDOW=1 opens a window, DM_AUDIO=1 plays sound):')
        PROG.say(cmd)
        report_profile_status(g, reg, pkg_ver, derived, provisional, 'Mac build')
        return

    if not prepare_xcode and free_gb(out_ipa.parent) < 5: raise BuildError(f'less than 5 GB free on the volume of {out_ipa.parent}')
    say('preparing the Xcode project' if prepare_xcode else f'building the IPA → {out_ipa}')
    ipa_env = dict(env, GAME=g.id, W=str(work / 'ipa'), PATCHED=str(patched), ROMFS=str(game / 'RomFS'), MSL_IOS=str(ios), GFXAPI=gfx, FONT_PY=py, DM_DISPLAY_NAME=app_name, DM_BUNDLE_ID=bundle_id, AOC=str(aoc or ''), DM_XCODE_DERIVED_DATA=str(work / 'xcode-derived-data'), DM_TEAM_ID=team_id, DM_LANG=lang)
    if prepare_xcode: ipa_env['DM_XCODE_PROJECT_DIR'] = str(project.parent)
    if app_version: ipa_env['DM_APP_VERSION'] = app_version
    # app icon: the game icon nx_extract saved from the control data (English, icon.jpg); the packed version's first, then the base game's
    icon = next((d / 'icon.jpg' for d in (game, base) if (d / 'icon.jpg').exists()), None)
    if icon: ipa_env['DM_ICON'] = str(icon); say(f'app icon: {icon}')
    else: say('[RED] no game icon in the extraction (an older extraction did not save it: delete extract/ and run again); the home-screen icon will be blank')
    env_file = args.env_file or DM_ENV_FILE
    if env_file: ipa_env['DM_ENV_FILE'] = str(Path(os.path.expanduser(env_file)).resolve())
    show_fps = SHOW_FPS if args.show_fps is None else args.show_fps
    ipa_env['DM_SHOW_FPS'] = '1' if show_fps else '0'
    if show_fps: say('frame-rate counter: on (DM_SHOW_FPS=0 in the app\'s Documents/env.txt turns it off)')
    if prepare_xcode:
        PROG.stage('prepare Xcode')
        run(['bash', repo / 'dm/ios/tools/build_ipa.sh', '--prepare-xcode'], logs / 'prepare_xcode.log', env=ipa_env)
        try: xcode_projects.register_project(workspace, project, app_name, workspace.with_name(workspace.stem + '-derived-data'))
        except ValueError as e: raise BuildError(str(e)) from e
        PROG.end()
        rmtree(work / 'tmp')
        say(f'ready: {workspace}')
        say(f'game project: {project}')
        say(f'Xcode derived data: {work / "xcode-derived-data"}')
        say('choose the game scheme in Xcode, select your Team and iPhone or iPad, then Run (Release)')
        report_profile_status(g, reg, pkg_ver, derived, provisional, 'Xcode project')
        return
    watch = IpaWatch(repo, work / 'ipa', out_ipa)
    PROG.stage('app build', poll=watch.poll, step='starting')   # build_ipa.sh's "pack IPA" line starts the stage "IPA pack"
    run(['bash', repo / 'dm/ios/tools/build_ipa.sh', out_ipa], logs / 'build_ipa.log', env=ipa_env, on_line=watch.line)
    if not out_ipa.is_file(): raise BuildError(f'no IPA at {out_ipa}')
    PROG.end(f'{out_ipa.name}, {amount(out_ipa.stat().st_size, "B")}')
    rmtree(work / 'tmp')
    if not keep:
        say(f'deleting the extracted game files: {ex}'); rmtree(ex)
    say(f'done: {out_ipa} ({out_ipa.stat().st_size / 2**30:.1f} GB)')
    report_profile_status(g, reg, pkg_ver, derived, provisional)


def cli(argv):   # returns the exit code: 0 done, 1 error, 130 interrupted
    # main points DM_GAMES_DIR (and DM_GAMES_FIRST) at this run's provisional or derived profile for its tools. Spyder runs the next F5 in the same process, where the game lookup
    # (game.py by_title) then found that profile and took it for a registered one: ASTRAL CHAIN's second run skipped every provisional step and stopped with no dmsh_chain.h
    environ = dict(os.environ)
    try:
        main(parse_args(argv))
    except BuildError as e:
        PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt:
        PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    except Exception as e:   # an unexpected error: end the stage line before the traceback (which goes to stderr; json mode also sends the error as a message)
        PROG.close(ok=False)
        if PROG.mode == 'json': PROG.say(f'[RED] {type(e).__name__}: {e}')
        raise
    finally:
        PROG.close(ok=False)   # stop the ticker
        for k in os.environ.keys() - environ.keys(): del os.environ[k]
        os.environ.update(environ)
    return 0


if __name__ == '__main__':
    code = cli(sys.argv[1:])
    if not in_ipython(): sys.exit(code)   # give an exit code in a terminal; do not exit in Spyder (keeps the console clean)

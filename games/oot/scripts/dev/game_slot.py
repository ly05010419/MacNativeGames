#!/usr/bin/env python3
"""Game slot: one totk-native game (or GPU probe) at a time on this Mac.

One game takes about 6.6 GB of the 16 GB and the GPU; two at once exhaust memory (the 2026-10-12 kernel panics) and
make every frame time meaningless. runtime/game_slot.c enforces this in every `boot` / `probe-gpu-*` run; this
module is the same protocol for scripts, so a script waits for its turn *before* it starts its own timers, and
reruns a test that the user's game interrupted. Change both files together.

Files in the slot directory (TOTK_GAME_SLOT_DIR, default <native-port>/analysis/game-slot; analysis/ is shared by
every worktree, see scripts/dev/new_worktree.sh):
  lock               flock(2) target; the holder keeps it locked while its game runs (the kernel drops it on exit)
  holder             key=value lines about the current holder (stale once the lock is free)
  queue/<ns>-<pid>   waiters, first come first served
  user-request       the user's game while it waits; it goes first and stops an agent's game that holds the slot
  preempted/<token>  written just before a holder's game is stopped for the user
  history.log        one line per acquire / start / preempt / release / exit

Command line:
  game_slot.py status
  game_slot.py run [--owner NAME] [--purpose TEXT] [--expect-seconds N] [--no-retry] -- COMMAND ...
      Waits for the slot, runs COMMAND holding it (passed down as TOTK_GAME_SLOT_FD; the game checks it), and
      releases it when COMMAND ends. If the user's game stopped it, waits and reruns COMMAND (exit 75 with --no-retry).
In a Python script:
  with GameSlot(owner="codex", purpose="map survey 017", expect_seconds=120) as slot:
      game = slot.popen([binary, "rewritten/v0", "boot"], env=env, cwd=root, stdout=log, stderr=subprocess.STDOUT)
      ...
  if slot.preempted: ...run the same test again...
"""
import argparse
import ctypes
import fcntl
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

NATIVE_PORT = Path(__file__).resolve().parents[2]
EX_PREEMPTED = 75
_OWNER = re.compile(r"[a-z0-9_-]{1,32}")

_libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
_libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
_libproc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
_libproc.proc_listallpids.argtypes = [ctypes.c_void_p, ctypes.c_int]
_PROC_PIDTBSDINFO, _BSDINFO_SIZE, _SZOMB = 3, 136, 5   # struct proc_bsdinfo: status at 4, pbi_start_tvsec at 120


class Preempted(Exception):
    """The user's game wants the slot: do not start (or stop) this test and run it again later."""


def pid_start(pid):
    """Start time in seconds, or 0 if the process is gone or a zombie; with the pid it names one process exactly."""
    if pid <= 0:
        return 0
    buf = ctypes.create_string_buffer(_BSDINFO_SIZE)
    if _libproc.proc_pidinfo(pid, _PROC_PIDTBSDINFO, 0, buf, _BSDINFO_SIZE) != _BSDINFO_SIZE:
        return 0
    if int.from_bytes(buf.raw[4:8], "little") == _SZOMB:
        return 0
    return int.from_bytes(buf.raw[120:128], "little")


def pid_same(pid, start):
    return bool(start) and pid_start(pid) == start


def pid_path(pid):
    buf = ctypes.create_string_buffer(4096)
    return buf.value.decode(errors="replace") if _libproc.proc_pidpath(pid, buf, 4096) > 0 else ""


def is_game(pid):
    return os.path.basename(pid_path(pid)).startswith("totk-native")   # also renamed copies (totk-native.base)


def all_pids():
    n = _libproc.proc_listallpids(None, 0)
    cap = max(n, 0) + 64
    arr = (ctypes.c_int * cap)()
    n = _libproc.proc_listallpids(arr, ctypes.sizeof(arr))
    return [p for p in arr[:max(0, min(n, cap))] if p > 0]


def clean(value):
    return "".join(" " if ord(c) < 0x20 else c for c in str(value))


def kv_read(path):
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return {}
    out = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and key not in out:
            out[key] = value
    return out


def kv_write(path, data):
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as file:
        file.write("".join(f"{k}={clean(v)}\n" for k, v in data.items()))
    os.replace(tmp, path)


def num(kv, key):
    try:
        return int(kv.get(key) or 0)
    except ValueError:
        return 0


def slot_dir():
    env = os.environ.get("TOTK_GAME_SLOT_DIR")
    d = Path(env) if env else (NATIVE_PORT / "analysis").resolve() / "game-slot"
    for sub in ("queue", "preempted"):
        (d / sub).mkdir(parents=True, exist_ok=True)
    return d.resolve()


def test_ignores_legacy(d):
    """Tests run in their own slot directory and may ignore games outside it; never the shared directory."""
    if os.environ.get("TOTK_GAME_SLOT_TEST_IGNORE_LEGACY") != "1":
        return False
    if not os.environ.get("TOTK_GAME_SLOT_DIR") or "/analysis/game-slot" in str(d):
        raise SystemExit("TOTK_GAME_SLOT_TEST_IGNORE_LEGACY needs a test TOTK_GAME_SLOT_DIR")
    return True


def history(d, event, owner, purpose, extra=""):
    line = (f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {event} owner={owner} pid={os.getpid()} mode=wrapper "
            f"purpose={clean(purpose)}{' ' + extra if extra else ''}\n")
    with open(d / "history.log", "a") as file:
        file.write(line)


def queue_entries(d):
    """Live waiters, oldest first (entries of processes that are gone are removed)."""
    live = []
    for path in sorted((d / "queue").iterdir()):
        if path.name.startswith(".") or ".tmp" in path.name:
            continue
        kv = kv_read(path)
        if not pid_same(num(kv, "pid"), num(kv, "start")):
            try:
                path.unlink()
            except OSError:
                pass
            continue
        live.append((path.name, kv))
    return live


def user_request_pid(d):
    kv = kv_read(d / "user-request")
    pid = num(kv, "pid")
    return pid if pid_same(pid, num(kv, "start")) else 0


def legacy_games(d, entries=None, user_pid=None, exclude=()):
    """totk-native processes outside the protocol (games started by a binary from before the slot existed)."""
    known = {num(kv, "pid") for _, kv in (queue_entries(d) if entries is None else entries)} | set(exclude)
    known.add(user_request_pid(d) if user_pid is None else user_pid)
    return [p for p in all_pids() if p != os.getpid() and p not in known and is_game(p) and pid_start(p)]


def describe_holder(d):
    kv = kv_read(d / "holder")
    since, expect = num(kv, "since"), num(kv, "expect_s")
    held = int(time.time()) - since if since else 0
    left = f"，预计还要约 {(expect - held + 59) // 60} 分钟" if expect > held else ""
    return (f"{kv.get('owner', '?')}（{kv.get('purpose', '?')}）占用中，已 {held // 60} 分 {held % 60} 秒{left}，"
            f"游戏 pid {kv.get('game_pid') or '-'}")


def slot_held(d):
    fd = os.open(d / "lock", os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    finally:
        os.close(fd)   # closing also drops the probe lock
    return False


class GameSlot:
    def __init__(self, owner=None, purpose="", expect_seconds=0, log=None):
        self.owner = owner or os.environ.get("TOTK_SLOT_OWNER") or "agent"
        if not _OWNER.fullmatch(self.owner):
            raise ValueError("owner: lower-case letters, digits, - or _ (claude, codex ...)")
        if self.owner == "user":
            raise ValueError("the user's game takes the slot itself: TOTK_SLOT_OWNER=user build/totk-native ...")
        self.purpose = clean(purpose or "test")
        self.expect = int(expect_seconds or 0)
        self.log = log or (lambda message: print(message, file=sys.stderr, flush=True))
        self.dir = slot_dir()
        self.fd = self.token = self.since = None
        self._stopped_for_user = False

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, *exc):
        self.release()
        return False

    def acquire(self):
        d = self.dir
        ignore_legacy = test_ignores_legacy(d)
        self.fd = os.open(d / "lock", os.O_RDWR | os.O_CREAT, 0o644)   # not inherited unless passed with pass_fds
        me = d / "queue" / f"{time.time_ns():019d}-{os.getpid()}"
        kv_write(me, {"pid": os.getpid(), "start": pid_start(os.getpid()), "owner": self.owner,
                      "purpose": self.purpose, "since": int(time.time())})
        started, last, last_t = time.monotonic(), "", 0.0   # last: who blocked us (not the running time)
        try:
            while True:
                entries = queue_entries(d)
                upid = user_request_pid(d)
                if upid:
                    reason, key = f"用户的游戏（pid {upid}）正在等，让它先", f"user {upid}"
                elif entries and entries[0][0] != me.name:
                    head = entries[0][1]
                    reason = f"排队中，前面是 {head.get('owner', '?')}（{head.get('purpose', '?')}）"
                    key = f"queue {entries[0][0]}"
                else:
                    try:
                        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        reason, key = describe_holder(d), "holder " + kv_read(d / "holder").get("token", "")
                    else:
                        old = [] if ignore_legacy else legacy_games(d, entries, upid)
                        if not old:
                            break
                        fcntl.flock(self.fd, fcntl.LOCK_UN)
                        reason, key = f"pid {old[0]} 的游戏没有接入游戏锁（旧版程序？），等它退出", f"legacy {old[0]}"
                now = time.monotonic()
                if key != last or now - last_t >= 30:
                    self.log(f"[game-slot] 等待游戏位置：{reason}")
                    last, last_t = key, now
                time.sleep(1 + random.random() * 0.2)
        except BaseException:
            os.close(self.fd)
            self.fd = None
            raise
        finally:
            try:
                me.unlink()
            except OSError:
                pass
        self.token = f"{time.time_ns():x}-{os.getpid()}"
        self.since = int(time.time())
        kv_write(d / "holder", {"owner": self.owner, "purpose": self.purpose, "token": self.token, "game_pid": "",
                                "game_start": "", "wrapper_pid": os.getpid(), "since": self.since,
                                "expect_s": self.expect, "mode": "wrapper", "exe": sys.argv[0], "cwd": os.getcwd(),
                                "log": ""})
        waited = int(time.monotonic() - started)
        history(d, "acquire", self.owner, self.purpose, f"waited_s={waited}")
        if waited:
            self.log(f"[game-slot] 取得游戏位置（{self.owner}，{self.purpose}），等了 {waited} 秒")

    def release(self):
        if self.fd is None:
            return
        held = int(time.time()) - (self.since or int(time.time()))
        history(self.dir, "release", self.owner, self.purpose,
                f"held_s={held}" + (" preempted=1" if self.preempted else ""))
        os.close(self.fd)
        self.fd = None

    @property
    def preempted(self):
        return self._stopped_for_user or bool(self.token and (self.dir / "preempted" / self.token).exists())

    def check(self):
        """Raise Preempted if the user's game is waiting for (or has claimed) the slot."""
        if self.preempted or user_request_pid(self.dir):
            self._stopped_for_user = True
            raise Preempted("the user's game is waiting for the slot")

    def env(self, base=None):
        env = dict(os.environ if base is None else base)
        env.update({"TOTK_GAME_SLOT_FD": str(self.fd), "TOTK_GAME_SLOT_DIR": str(self.dir),
                    "TOTK_SLOT_OWNER": self.owner, "TOTK_SLOT_PURPOSE": self.purpose, "TOTK_SLOT_TOKEN": self.token,
                    "TOTK_SLOT_SINCE": str(self.since), "TOTK_SLOT_EXPECT_S": str(self.expect),
                    "TOTK_SLOT_WRAPPER_PID": str(os.getpid())})
        return env

    def popen(self, argv, env=None, **kwargs):
        """subprocess.Popen with the slot passed down; raises Preempted instead of starting if the user is waiting."""
        if self.fd is None:
            raise RuntimeError("GameSlot.popen outside the slot")
        self.check()
        kwargs["pass_fds"] = tuple(kwargs.get("pass_fds", ())) + (self.fd,)
        return subprocess.Popen(argv, env=self.env(env), **kwargs)


def cmd_status(_args):
    d = slot_dir()
    print(f"slot directory: {d}")
    held = slot_held(d)
    kv = kv_read(d / "holder") if held else {}
    if held:
        print(f"held: {describe_holder(d)}")
        for key in ("mode", "cwd", "log"):
            if kv.get(key):
                print(f"  {key}: {kv[key]}")
    else:
        print("free")
    upid = user_request_pid(d)
    if upid:
        print(f"user's game waiting: pid {upid}")
    for name, kv in queue_entries(d):
        waited = int(time.time()) - num(kv, "since")
        print(f"queued: {kv.get('owner', '?')} ({kv.get('purpose', '?')}), pid {kv.get('pid')}, waiting {waited} s")
    holder_game = num(kv, "game_pid") if pid_same(num(kv, "game_pid"), num(kv, "game_start")) else 0
    for pid in legacy_games(d, exclude=(holder_game,)):
        print(f"game outside the slot: pid {pid} {pid_path(pid)}")
    try:
        lines = (d / "history.log").read_text(errors="replace").splitlines()[-8:]
    except OSError:
        lines = []
    if lines:
        print("recent:")
        for line in lines:
            print("  " + line)
    return 0


def cmd_run(args):
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        raise SystemExit("game_slot.py run: no command")
    while True:
        slot = GameSlot(owner=args.owner, purpose=args.purpose or os.path.basename(command[0]),
                        expect_seconds=args.expect_seconds)
        code = 0
        with slot:
            try:
                process = slot.popen(command)
            except Preempted:
                process = None
            if process:
                code = process.wait()
        if slot.preempted:
            if args.no_retry:
                print("[game-slot] 被用户的游戏打断（退出 75）", file=sys.stderr, flush=True)
                return EX_PREEMPTED
            print("[game-slot] 被用户的游戏打断；等游戏位置空出来后重跑同一个测试", file=sys.stderr, flush=True)
            continue
        return code if code >= 0 else 128 - code


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("status", help="who holds the slot, who waits")
    run = sub.add_parser("run", help="run a command holding the slot")
    run.add_argument("--owner", help="claude, codex ... (default TOTK_SLOT_OWNER, else agent)")
    run.add_argument("--purpose", help="what the run is for, shown to whoever waits")
    run.add_argument("--expect-seconds", type=int, default=0, help="rough duration, shown to whoever waits")
    run.add_argument("--no-retry", action="store_true", help="exit 75 instead of rerunning after the user's game")
    run.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    return cmd_status(args) if args.action == "status" else cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())

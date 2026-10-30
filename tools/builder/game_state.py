"""What the builder can learn about a built game without starting it (S5.1, O1, O3, O4). The model: no Tk.

- app_version: the one entry of the app's Contents/Resources/totk/extracted other than firmware (build_app.py copies
  extracted/<version>/romfs there; make_mod_manifest.py reads it); None when there is not exactly one.
- game_running: the launcher's own test (app/launcher.m running_game) on the game's slot, which every copy of the app
  shares: the lock held by another process, and the holder a player's game whose pid is alive. Taking the lock for an
  instant is what the launcher does too.
- probe_metal4fx: whether this Mac's GPU has Metal 4 MetalFX frame interpolation, the check the game aborts on
  (totk_mfx.m); through `xcrun swift -e`, 15-21 s cold, so with a long timeout; 'yes', 'no' or 'unknown'."""
import fcntl
import os
from pathlib import Path

from tools.builder.ports import Outcome

PROBE_S = 120.0
PROBE = ('import Metal; import MetalFX; '
         'print(MTLFXFrameInterpolatorDescriptor.supportsMetal4FX(MTLCreateSystemDefaultDevice()!))')


def app_version(app):
    folder = Path(app) / 'Contents/Resources/totk/extracted'
    try:
        names = [p.name for p in folder.iterdir() if p.is_dir() and p.name != 'firmware']
    except OSError:
        return None
    return names[0] if len(names) == 1 else None


def _holder(path):
    values = {}
    try:
        for line in Path(path).read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep:
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True                 # it exists, owned by someone else
    return True


def game_running(data_dir):
    slot = Path(data_dir) / 'game-slot'
    try:
        fd = os.open(slot / 'lock', os.O_RDONLY | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except BlockingIOError:
        pass
    finally:
        os.close(fd)
    holder = _holder(slot / 'holder')
    pid = holder.get('game_pid', '')
    return holder.get('owner') == 'user' and pid.isdigit() and int(pid) > 0 and _alive(int(pid))


def probe_metal4fx(runner):
    result = runner.run(['/usr/bin/xcrun', 'swift', '-e', PROBE], timeout_s=PROBE_S)
    answer = result.stdout.strip().splitlines()[-1:] if result.outcome is Outcome.OK else []
    return {'true': 'yes', 'false': 'no'}.get(answer[0] if answer else '', 'unknown')

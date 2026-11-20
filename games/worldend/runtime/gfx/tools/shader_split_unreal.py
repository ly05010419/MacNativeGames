"""Unreal Engine paks found by their footer only (shader_split.py and profile_guess.py). Unreal Engine games are not supported: their
pak reader (uepak.py), which opened a pak's encrypted index with an AES key it found in the game's own code, was removed (user 2026-11-20:
"我们不需要 就去掉 删"). Only the footer's magic is read, to say so."""
from pathlib import Path

UNREAL_PAK_MAGIC = b'\xe1\x12\x6f\x5a'   # 0x5A6F12E1, in the last 0xdd (version 11) or 0x2d (version 4) bytes of a pak


def unreal_pak(path):
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2); n = f.tell(); f.seek(max(0, n - 0x100)); return UNREAL_PAK_MAGIC in f.read()
    except OSError:
        return False


def unreal_paks(root, **_):
    return [str(p) for p in sorted(Path(root).rglob('*.pak')) if unreal_pak(p)]

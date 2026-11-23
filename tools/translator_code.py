"""The shader translators this repository carries and their code identity (user 2026-11-18: "以后 改变翻译器 必须过测试
回答一个重要问题 为什么 翻译器改变 有没有必要 这种事情不允许在发生！").

A translator's identity is its code, not its file: MNG Launcher ships every translator signed with a Developer ID whose
signature carries a time stamp, so each release is another file of the same code (2026-11-18: seven Launcher sources of
11-16 to 11-18, seven files, one code). Named by the whole file, every code update made players translate all of
Ocarina of Time's 42,848 shaders again and refused its express update. code_sha256 is the same hash as
games/oot/scripts/prepare_ns2_seed.py code_sha256, games/totk/scripts/prepare_shader_seed.py code_digest and
totk_rt_translator.h totk_code_sha256 (tests/test_translator_code_identity.py).

tools/translator_changes.json records, for every translator, its code now and each change with why it changed and why
that was necessary; tests/test_translator_changes.py fails when a translator's code is not the recorded one."""
import hashlib
import struct
from pathlib import Path

# The programs that translate shaders, by file name, under games/<game>/prebuilt/: the Mac services, Pokémon's, the .NET
# translator of the Koei Tecmo games (its apphost and its code) and the iOS static library with its bootstrap.
TRANSLATOR_NAMES = ('totk_xlate', 'psv_xlate', 'dmsh_xlate', 'dmsh_xlate.dll', 'libtotk_xlate.a', 'libtotk_xlate_bootstrap.o')


def translator_files(root):
    """Repository-relative paths of every translator program, sorted."""
    root = Path(root)
    return sorted(str(p.relative_to(root)) for name in TRANSLATOR_NAMES for p in root.glob(f'games/*/prebuilt/**/{name}')
                  if p.is_file() and not p.is_symlink())


def code_sha256(path):
    """SHA-256 of a program's code, the same whatever signed it: a thin 64-bit Mach-O up to its code signature, with the
    three sizes the signature sets zeroed (__LINKEDIT's vmsize and filesize, LC_CODE_SIGNATURE's datasize). Anything
    else (no signature, an archive, a .NET assembly) is hashed whole."""
    data = bytearray(Path(path).read_bytes())
    whole = hashlib.sha256(bytes(data)).hexdigest()
    if len(data) < 32 or struct.unpack_from('<I', data, 0)[0] != 0xfeedfacf:
        return whole
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    at, end, sig = 32, 32 + sizeofcmds, None
    for _ in range(ncmds):
        if at + 8 > end:
            return whole
        cmd, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > end:
            return whole
        if cmd == 0x19 and data[at + 8:at + 24].rstrip(b'\0') == b'__LINKEDIT':   # LC_SEGMENT_64
            struct.pack_into('<Q', data, at + 32, 0)   # vmsize
            struct.pack_into('<Q', data, at + 48, 0)   # filesize
        elif cmd == 0x1d:   # LC_CODE_SIGNATURE
            sig = struct.unpack_from('<I', data, at + 8)[0]
            struct.pack_into('<I', data, at + 12, 0)   # datasize
        at += size
    if sig is None or sig > len(data):
        return whole
    return hashlib.sha256(bytes(data[:sig])).hexdigest()

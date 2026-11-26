"""The lowest macOS each Mach-O asks for (its LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX minos), for every program and
library a release carries (user 2026-11-26: "用测试保证 以后不准错 哪怕我用这个新机器打包 26依然支持"; #2395).

Launcher 1.1.5 was the first release made on a macOS 27 Mac. Nothing set a minimum, so clang took the SDK's version:
every program it carried (the game runtimes in the source's .prebuilt, the offline store's libraries, zstd, spirv-cross)
said minos 27.2, and macOS 26 refused to open a Breath of the Wild built with it, while LSMinimumSystemVersion said 26.0.

    python3 tools/macho_minos.py <file or folder> ... [--limit 26.0]   every Mach-O (each member of a static library,
                                                                         each file of a tar) newer than the limit;
                                                                         exit 1 when there is one
Standard library only, so the release tools and the Launcher's Python can run it."""
import struct
import sys
import tarfile
from pathlib import Path

LIMIT = '26.0'   # LSMinimumSystemVersion of every game app and of the Launcher
LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1
MH_MAGIC_64, MH_CIGAM_64, MH_MAGIC, MH_CIGAM = 0xfeedfacf, 0xcffaedfe, 0xfeedface, 0xcefaedfe
THIN = (MH_MAGIC_64, MH_CIGAM_64, MH_MAGIC, MH_CIGAM)
FAT_MAGIC, FAT_MAGIC_64 = 0xcafebabe, 0xcafebabf
AR_MAGIC = b'!<arch>\n'


def version_text(packed):
    """xxxx.yy.zz packed as in Mach-O (16.8.8 bits), as '27.2' or '26.0.1'."""
    major, minor, patch = packed >> 16, (packed >> 8) & 0xff, packed & 0xff
    return f'{major}.{minor}' + (f'.{patch}' if patch else '')


def version_tuple(text):
    parts = [int(p) for p in str(text).split('.')]
    return tuple(parts + [0] * (3 - len(parts)))


def _thin_minos(data):
    """The macOS minos of one thin Mach-O image, or None (not a Mach-O, or no macOS version command)."""
    if len(data) < 28:
        return None
    magic = struct.unpack_from('<I', data)[0]
    if magic not in THIN:
        return None
    end = '<' if magic in (MH_MAGIC_64, MH_MAGIC) else '>'
    offset = 32 if magic in (MH_MAGIC_64, MH_CIGAM_64) else 28
    for _ in range(struct.unpack_from(end + 'I', data, 16)[0]):
        if offset + 16 > len(data):
            return None
        cmd, size = struct.unpack_from(end + 'II', data, offset)
        if cmd == LC_BUILD_VERSION:
            platform, minos = struct.unpack_from(end + 'II', data, offset + 8)
            if platform == PLATFORM_MACOS:
                return version_text(minos)
        elif cmd == LC_VERSION_MIN_MACOSX:
            return version_text(struct.unpack_from(end + 'I', data, offset + 8)[0])
        if size < 8:
            return None
        offset += size
    return None


def _fat(data):
    """The slices of a universal binary, or None."""
    if len(data) < 8 or struct.unpack_from('>I', data)[0] not in (FAT_MAGIC, FAT_MAGIC_64):
        return None
    wide, count = struct.unpack_from('>I', data)[0] == FAT_MAGIC_64, struct.unpack_from('>I', data, 4)[0]
    if count > 64:   # a Java class file shares the magic; a universal binary has a few slices
        return None
    slices, at = [], 8
    for _ in range(count):
        if wide:
            _, _, off, size, _, _ = struct.unpack_from('>IIQQII', data, at)
            at += 32
        else:
            _, _, off, size, _ = struct.unpack_from('>IIIII', data, at)
            at += 20
        slices.append(data[off:off + size])
    return slices


def _archive(data):
    """(member name, bytes) of a static library (BSD long names #1/n)."""
    at = len(AR_MAGIC)
    while at + 60 <= len(data):
        header = data[at:at + 60]
        name = header[:16].decode('ascii', 'replace').strip()
        size = int(header[48:58].decode('ascii').strip() or 0)
        body = data[at + 60:at + 60 + size]
        if name.startswith('#1/'):
            n = int(name[3:])
            name, body = body[:n].rstrip(b'\0').decode('utf-8', 'replace'), body[n:]
        yield name, body
        at += 60 + size + (size & 1)


def minos_of(data, name=''):
    """[(what, minos)] for the bytes of one file: one per Mach-O image (each member of a static library, each slice of
    a universal binary). Empty for anything else."""
    if data[:8] == AR_MAGIC:
        return [(f'{name}({member})', m) for member, body in _archive(data) for _, m in minos_of(body)]
    slices = _fat(data)
    if slices is not None:
        return [(name, m) for s in slices for m in [_thin_minos(s)] if m]
    m = _thin_minos(data)
    return [(name, m)] if m else []


def _looks_binary(head):
    return head[:8] == AR_MAGIC or (len(head) >= 4 and (struct.unpack_from('<I', head)[0] in THIN
                                                        or struct.unpack_from('>I', head)[0] in (FAT_MAGIC, FAT_MAGIC_64)))


def scan(path):
    """[(what, minos)] for a file, every file under a folder, and every file inside a .tar / .tar.gz among them."""
    path = Path(path)
    files = [path] if path.is_file() else sorted(p for p in path.rglob('*') if p.is_file() and not p.is_symlink())
    found = []
    for f in files:
        if f.name.endswith(('.tar', '.tar.gz', '.tgz')):
            with tarfile.open(f) as t:
                for member in t:
                    if member.isfile():
                        data = t.extractfile(member).read()
                        if _looks_binary(data[:8]):
                            found += minos_of(data, f'{f}!{member.name}')
            continue
        with open(f, 'rb') as stream:
            head = stream.read(8)
            if _looks_binary(head):
                found += minos_of(head + stream.read(), str(f))
    return found


def too_new(paths, limit=LIMIT):
    """[(what, minos)] of everything under paths that asks for a macOS newer than limit."""
    return [(what, m) for p in paths for what, m in scan(p) if version_tuple(m) > version_tuple(limit)]


def main(argv):
    limit = LIMIT
    if '--limit' in argv:
        i = argv.index('--limit')
        limit, argv = argv[i + 1], argv[:i] + argv[i + 2:]
    if not argv:
        raise SystemExit(__doc__)
    bad = too_new(argv, limit)
    for what, m in bad:
        print(f'{m}  {what}')
    if bad:
        print(f'{len(bad)} Mach-O image(s) need a macOS newer than {limit}', file=sys.stderr)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

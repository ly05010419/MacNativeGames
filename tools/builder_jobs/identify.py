"""What a game file (XCI, NSP, NSZ, XCZ) holds, read with the player's keys: the content metas of its Meta NCAs, each a
title ID, a kind (base game, update or add-on) and a version, as nxnative reads every package (scripts/
build_ipa_from_nsp.py scan_nsps, after its issue #1629: "A package's title ID and version are what its content metas
say, never its file name"). A player's Pokémon card was taken for Tears of the Kingdom (user 2026-11-21: "经常装rom
缺识别成其他游戏，比如宝可梦的rom 识别出 塞尔达"): the card's game had come from its name, or not at all.

A Meta NCA's header carries its content meta's title ID (an update's Program NCA may keep the base game's: games/totk/
scripts/extract_exefs.py program_nca), readable with header_key alone; its section 0, a PFS0 with one .cnmt file, gives
the kind and the version and needs the key area key of the NCA's generation. Keys too old for the file still tell its
game; they are said as the problem.

Contract: contents(path, keys) returns (metas, problem) and never raises for a bad file or keys. metas: [Meta] (empty
when the file holds no base game, update or add-on), or None when the file could not be read at all (no cryptography
package in this Python: problem None; keys that cannot open it: problem says so). Nothing from the keys is returned or
printed."""
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path
import struct
import sys

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

SCRIPTS = repo.REPO / 'games/totk/scripts'
# content meta types (CNMT offset 0x0C): Application, Patch, AddOnContent; others (a delta update, system titles) are
# not game files
KINDS = {0x80: 'base', 0x81: 'update', 0x82: 'dlc'}
META = 1   # NCA content type of a Meta NCA (offset 0x205)


@dataclass(frozen=True)
class Meta:
    kind: str            # base | update | dlc
    title: str           # 16 hex digits, uppercase
    version: int | None  # None when the keys cannot open the content meta (too old for the file)


def _reader():
    if str(SCRIPTS) not in sys.path:
        sys.path.insert(0, str(SCRIPTS))
    try:
        import extract_exefs
    except ImportError:   # no cryptography in this Python
        return None
    return extract_exefs


def load_keys(keys):
    """The keys file's keys, or (None, problem)."""
    reader = _reader()
    if reader is None:
        return None, None
    try:
        loaded = reader.load_keys(Path(keys))
    except (OSError, ValueError) as e:
        return None, t(f'the keys file cannot be read: {e}', f'keys 文件读不了：{e}')
    if 'header_key' not in loaded:
        return None, t('the keys file has no header_key: not a complete keys file', 'keys 文件没有 header_key：不是完整的 keys 文件')
    return loaded, None


def contents(path, keys):
    reader = _reader()
    if reader is None:
        return None, None
    loaded, problem = load_keys(keys)
    if loaded is None:
        return None, problem
    path = Path(path)
    try:
        with path.open('rb') as f:
            _, entries = reader.container_entries(f, path.stat().st_size)
            records = reader.nca_inventory(f, entries, loaded)
            metas, generation = [], None
            for record, hdr in records:
                if record['content_type'] != META:
                    continue
                try:
                    kind, title, version = _content_meta(reader, f, record, hdr, loaded)
                except ValueError:   # keys without this NCA's key area key: its title from the header, no version
                    generation = _generation(hdr) if generation is None else max(generation, _generation(hdr))
                    title, version = record['title_id'], None
                    kind = repo.build_app().title_kind(title)[1]
                if kind:
                    metas.append(Meta(kind, title, version))
    except Exception as e:   # the reader raises its own errors (require) as well as OSError and struct errors
        return None, t(f'these keys cannot read {path.name} ({e}): they do not match these game files',
                       f'这份 keys 读不了 {path.name}（{e}）：和这份游戏文件对不上')
    missing = missing_generation(records, loaded)
    if missing is None and generation is not None:
        missing = generation
    if missing is not None:
        return metas, t(f'these keys are older than {path.name}: it needs key generation {missing:02x} '
                        f'(titlekek_{missing:02x}), which this keys file does not have',
                        f'这份 keys 比 {path.name} 旧：它要第 {missing:02x} 代密钥（titlekek_{missing:02x}），这份 keys 文件里没有')
    return metas, None


def _generation(hdr):
    gen = max(hdr[0x206], hdr[0x220])
    return gen - 1 if gen else 0


def _content_meta(reader, f, record, hdr, keys):
    """(kind or None, title ID, version) of the .cnmt file in a Meta NCA's section 0 (a hierarchical SHA-256 PFS0,
    AES-CTR, as extract_exefs reads an ExeFS). Raises ValueError when it cannot be read."""
    require = reader.require
    ctr_key = reader.nca_ctr_key(hdr, keys, {})   # a Meta NCA is never ticket-encrypted
    start, end = struct.unpack_from('<II', hdr, 0x240)
    require(start, 'Meta NCA without section 0')
    fs = hdr[0x400:0x600]
    require(hashlib.sha256(fs).digest() == hdr[0x280:0x2A0], 'FS header hash mismatch')
    require(fs[2:4] == b'\x01\x02' and fs[4] == 3, 'Meta NCA section is not an AES-CTR PFS0')
    pfsoff, pfssize = struct.unpack_from('<QQ', fs, 0x40)
    secstart, secsize = start * 512, (end - start) * 512
    require(0 <= pfsoff and 0 < pfssize <= 1 << 20 and pfsoff + pfssize <= secsize, 'Content meta out of bounds')
    absolute = secstart + pfsoff
    aligned = absolute & ~15
    iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
    from cryptography.hazmat.primitives.ciphers import modes
    pfs = reader.decrypt(ctr_key, modes.CTR(iv), reader.read_at(f, record['nca_offset'] + aligned, pfssize + absolute - aligned))
    pfs = pfs[absolute - aligned:]
    cnmt = [(off, size) for name, off, size in reader.partition_entries(io.BytesIO(pfs), 0, len(pfs)) if name.endswith('.cnmt')]
    require(len(cnmt) == 1 and cnmt[0][1] >= 0x20, 'Expected one content meta')
    off, _ = cnmt[0]
    title, version, kind = struct.unpack_from('<QIB', pfs, off)
    return KINDS.get(kind), f'{title:016X}', version


def missing_generation(records, keys):
    """The newest key generation (0-based) an NCA needs that the keys lack, or None. An NCA's generation is the larger
    of its two header fields (0x206, 0x220), 0 and 1 both meaning the first; a ticketed NCA needs titlekek_<gen>, a
    ticketless one key_area_key_application_<gen>. Either name is enough here: one dump holds both or neither."""
    need = {_generation(hdr) for _, hdr in records}
    for gen in sorted(need, reverse=True):
        if f'titlekek_{gen:02x}' not in keys and f'key_area_key_application_{gen:02x}' not in keys:
            return gen
    return None

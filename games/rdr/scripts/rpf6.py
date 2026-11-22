#!/usr/bin/env python3
"""Red Dead Redemption's shader programs from its RPF6 archive (shaders.rpf) and its main module, for the shader seed
(scripts/prepare_rdr_shader_seed.py), as Tears of the Kingdom's seed tool reads BNSH files (prepare_shader_seed.py
bnsh_shaders).

RPF6 (Rockstar's archive of this game, big-endian header): 'RPF6', the entry count, the debug area's offset / 8 and an
encryption word, then the table of entries, encrypted (the key is not taken here: nothing below needs the table). The
files follow as Zstandard frames, one per file, back to back on 8-byte boundaries up to the debug area: shaders.rpf
holds the effect list (a text file) and one RAGE effect per .fx ('rgxd'). Each frame is checked by decompressing it, and
the frames may not outnumber the entries.

An effect (and main, which carries a few RAGE programs of its own) keeps each NVN program as a record: the word
0x19866891, the code's size, the control section's size, the code (its 0x30-byte header first, magic 0x12345678), the
control section (magic 0x98761234) as the game hands them to NVN. The runtime keys a shader by the SHA-256 of the
control section's first ctl_key_len bytes and the code's first <control+0x704> bytes (nvn_mtl.m prog_get), which is
what programs() yields. Checked on the Mac (TOTK_CTL_DUMP, 22 November 2026): every one of the 560 shaders the runtime
bound from the start to Armadillo is one of these records (546 from shaders.rpf, 14 from main), byte for byte."""
import hashlib
import struct

MAGIC = b'RPF6'
ZSTD = b'\x28\xb5\x2f\xfd'
EFFECT = b'rgxd'
RECORD = struct.pack('<I', 0x19866891)
CODE_MAGIC = struct.pack('<I', 0x12345678)
CONTROL_MAGIC = struct.pack('<I', 0x98761234)
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}   # NVN stages at control+0x714
TOC_ENTRY = 20


class Rpf6Error(ValueError):
    pass


def header(data):
    """(entries, debug offset in bytes, encryption word) of an RPF6 archive."""
    if len(data) < 16 or data[:4] != MAGIC:
        raise Rpf6Error('not an RPF6 archive')
    entries, debug, crypt = struct.unpack_from('>III', data, 4)
    debug *= 8
    if not entries or 16 + entries * TOC_ENTRY > len(data) or debug > len(data) or (debug and debug < 16 + entries * TOC_ENTRY):
        raise Rpf6Error('RPF6 header out of range')
    return entries, debug or len(data), crypt


def files(data):
    """[(offset, compressed size, contents)] of an RPF6 archive whose files are Zstandard frames, in archive order."""
    import zstandard
    entries, end, _ = header(data)
    at, found = 16 + entries * TOC_ENTRY, []
    while True:
        at = data.find(ZSTD, at, end)
        if at < 0:
            break
        obj = zstandard.ZstdDecompressor().decompressobj()
        try:
            contents = obj.decompress(data[at:end])
        except zstandard.ZstdError as e:
            raise Rpf6Error(f'RPF6 file at {at:#x}: {e}') from None
        if not obj.eof:
            raise Rpf6Error(f'RPF6 file at {at:#x}: truncated Zstandard frame')
        size = end - at - len(obj.unused_data)
        found.append((at, size, contents))
        at = (at + size + 7) & ~7
    if len(found) > entries:
        raise Rpf6Error(f'{len(found)} files for {entries} entries')
    return found


def ctl_key_len(control):
    """How many bytes of a control section the runtime hashes (nvn_mtl.m ctl_key_len)."""
    size = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return size if 0x7cc <= size < 0x880 else 0x880


def programs(data):
    """Every NVN program record in data (an effect, or a module's image): (key, stage, control, code), control cut to
    what the runtime hashes and code to its length, as the seed tool's bnsh_shaders yields them."""
    at = data.find(RECORD)
    while at >= 0:
        record = parse(data, at)
        if record:
            yield record
        at = data.find(RECORD, at + 4)


def parse(data, at):
    """The record at at, or None when the words there are not one (the magic can occur in other data)."""
    if at + 12 > len(data):
        return None
    code_size, control_size = struct.unpack_from('<II', data, at + 4)
    code_at = at + 12
    control_at = code_at + code_size
    if not 0x7cc <= control_size <= 0x1000 or control_at + control_size > len(data) or code_size < 0x30:
        return None
    code, control = data[code_at:control_at], data[control_at:control_at + control_size]
    if control[:4] != CONTROL_MAGIC:
        return None
    key_len = ctl_key_len(control)
    if key_len > len(control):
        return None
    control = control[:key_len]
    c1, length = struct.unpack_from('<II', control, 0x700)
    stage = STAGES.get(struct.unpack_from('<I', control, 0x714)[0])
    if not stage or not 0 < length <= code_size or c1 > length or (stage != 'cs' and (code[:4] != CODE_MAGIC or c1 < 0x30)):
        return None
    code = code[:length]
    return hashlib.sha256(control + code).hexdigest(), stage, control, code


def effects(data):
    """[(offset, effect)] of shaders.rpf: its RAGE effects (the effect list, a text file, is left out)."""
    return [(at, contents) for at, _, contents in files(data) if contents[:4] == EFFECT]

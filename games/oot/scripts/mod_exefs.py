#!/usr/bin/env python3
"""Merge a player's main-module code patches into the build, before the code is rewritten for Apple silicon.

The Mac app loads RomFS mods while it runs (docs/mods.md); a mod's ExeFS side - an IPS / IPS32 / pchtxt patch for the
main module - cannot be applied while the game runs, because the code modules are rewritten for Apple silicon at
build time and bound to the app by hash. It can be merged at build time instead, which is what this script does:

    python3 build_app.py <game.xci> -k <prod.keys> --mod ~/Mods/SecondQuest

What is merged (v1): the code patches for **main** only, read from `<mod>/exefs/`:

  *.ips / *.ips32   an IPS / IPS32 patch, as emulators apply it to the NSO. Its offsets count the 0x100-byte NSO
                    header (IPS numbers are main addresses + 0x100); here the same convention is reversed. The file
                    names no module or build by itself, so it is read as a main patch only when it is named main, or
                    main's build id the way Atmosphere and the mod managers name patches (exefs/<build id>.ips); a
                    patch for another module, or named after another build of main, is left out with a note.
  *.pchtxt          the community text form, read as the runtime's own TOTK_PCHTXT experiment reads it (plus the two
                    switches it does not read yet - see the next line); the format is:
                    "@nsobid-<build id>" must name this version's main, then "<main address> <hex bytes>" lines
                    between "@enabled" and "@stop", with "@disabled" switching a section back off (a mod author who
                    keeps an edit around without applying it). "@flag offset_shift 0x100" must come before the first
                    patch line - it says the addresses leave the 0x100-byte NSO header out, which is how they are
                    applied; a file without it, or with another shift, is refused rather than put 0x100 bytes off.
                    "@flag print_values" (a display option of the patching tools) is accepted. The build id
                    may be the 16-byte one (32 hex digits, as the rewriter's tables use it) or the 20 bytes the
                    runtime compares.
  main.npdm         ignored with a note: the native loader never reads a module's npdm (the module metadata comes
                    from the tables built from the retail code).

What is not (v1): an extra code module (`subsdk9` and the like) is **not loaded** - the app loads the three modules
it was built from, so a module's own code never runs. The build says so, records it in the report, and still merges
the main patches: a mod such as The Second Quest ships a hooking module whose trampolines do not install here, while
its main patches and its RomFS do apply. A whole replacement main is refused instead: it would have to be verified
as this version's code, which is what a patch is for. The mod's `romfs/` folder is the runtime path (docs/mods.md)
and is not read here.

Every patch is checked before it is applied:

  * the input must be the verified retail main of the selected version (segment digests, NSO structure);
  * an edit must land inside main's text, rodata or data - never in the padding the build writes its own veneers
    into, and never outside the image;
  * an edit must not touch a site the runtime or the rewriter patches itself (scripts/patch_sites.py), so a mod
    cannot silently break the 60 FPS, real-time frame-step, render/output-resolution, ultrawide or cloth patches;
  * two patch files must not change the same bytes.

The result is a normal NSO again (segments uncompressed, header fields and digests recomputed), which the build
rewrites exactly like the retail module: `rewritten/<version>/main.patched.bin` carries the mod's changes, the app
is bound to that image's SHA-256, and the same code goes into an iPhone / iPad build.

usage: mod_exefs.py apply --exefs <extracted/<version>/exefs> --mod <mod folder> --version v121
                          --output <extracted/<version>/mod-exefs/main> [--report <json>]
       mod_exefs.py sites --version v121 [--build-id <32 or 40 hex digits>]
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patch_sites

HEADER_SIZE = 0x100
BUILD_ID_AT, BUILD_ID_SIZE = 0x40, 0x14     # 16 bytes the rewriter's tables use, the 20 the runtime compares
SEGMENT_NAMES = ('text', 'rodata', 'data')
SEGMENT_TABLE, COMPRESSED_SIZES, SEGMENT_DIGESTS, BSS_AT = 0x10, 0x60, 0xA0, 0x3c
MAX_SEGMENT = 256 * 1024 * 1024
PATCH_SUFFIXES = ('.ips', '.ips32', '.pchtxt')
IPS_HEADER = 0x100      # an IPS patch's offsets count the NSO header (the runtime's own note in game_patches.c)
MODULES = ('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'rtld')     # the module names a patch file can carry
MODULE_STEM = re.compile(r'subsdk\d*')     # ... and subsdk3..subsdk9, which mods ship as their hooking module


class ModError(Exception):
    pass


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        while chunk := handle.read(8 << 20):
            digest.update(chunk)
    return digest.hexdigest()


class Segment:
    __slots__ = ('name', 'file_offset', 'address', 'size', 'stored', 'compressed', 'data')

    def __init__(self, name, file_offset, address, size, stored, compressed, data):
        self.name, self.file_offset, self.address, self.size = name, file_offset, address, size
        self.stored, self.compressed, self.data = stored, compressed, data


class Nso:
    """A Switch NSO module read the way the runtime and the rewriter read it: the three segments decompressed into
    one image whose addresses are what the game's patches (and a mod's) are written in."""

    def __init__(self, raw, source='main'):
        if raw[:4] != b'NSO0' or len(raw) < HEADER_SIZE:
            raise ModError(f'{source}: not a Switch NSO module (no NSO0 header)')
        self.raw = raw
        self.source = source
        self.flags = struct.unpack_from('<I', raw, 0x0c)[0]
        self.bss = struct.unpack_from('<I', raw, BSS_AT)[0]
        self.compressed_sizes = struct.unpack_from('<III', raw, COMPRESSED_SIZES)
        self.build_id = raw[BUILD_ID_AT:BUILD_ID_AT + BUILD_ID_SIZE].hex()
        self.segments = []
        for index, name in enumerate(SEGMENT_NAMES):
            file_offset, address, size, extra = struct.unpack_from('<IIII', raw, SEGMENT_TABLE + 16 * index)
            compressed = bool(self.flags & (1 << index))
            stored = self.compressed_sizes[index] if compressed else size
            if size > MAX_SEGMENT or file_offset + stored > len(raw):
                raise ModError(f'{source}: invalid {name} segment')
            payload = raw[file_offset:file_offset + stored]
            if compressed:
                payload = self._decompress(payload, size, name)
            if len(payload) != size:
                raise ModError(f'{source}: {name} segment has {len(payload)} bytes, not {size}')
            self.segments.append(Segment(name, file_offset, address, size, stored, compressed, payload))
            if self.flags & (8 << index):       # the NSO's own digest of the segment, when it carries one
                want = raw[SEGMENT_DIGESTS + 32 * index:SEGMENT_DIGESTS + 32 * (index + 1)]
                if hashlib.sha256(payload).digest() != want:
                    raise ModError(f'{source}: {name} does not match the NSO header\'s own SHA-256 (not this version?)')
        self.image_size = max(segment.address + segment.size for segment in self.segments)
        self.image_size = max(self.image_size, self.segments[2].address + self.segments[2].size + self.bss)

    def _decompress(self, payload, size, name):
        try:
            import lz4.block
        except ImportError:
            raise ModError(f'{self.source}: the {name} segment is compressed and the lz4 Python module is missing '
                           f'(brew install python@3.14 and install games/totk/requirements.txt)')
        return lz4.block.decompress(payload, uncompressed_size=size)

    def image(self):
        """The module as the game sees it: main addresses are offsets into this."""
        image = bytearray(self.image_size)
        for segment in self.segments:
            image[segment.address:segment.address + segment.size] = segment.data
        return image

    def segment_of(self, address, size, where):
        """The segment an edit belongs to; an edit in a gap (the padding the build writes veneers into) is refused."""
        for segment in self.segments:
            if segment.address <= address and address + size <= segment.address + segment.size:
                return segment
        text = self.segments[0]
        following = [s.address for s in self.segments if s.address >= text.size]
        limit = min(following) if following else self.image_size
        if address < limit and address + size > text.size:
            raise ModError(f'{where}: main+{address:#x}..{address + size:#x} is in the padding after main\'s text '
                           f'(main+{text.size:#x}..{limit:#x}); the build writes its own patch veneers there')
        geometry = ', '.join(f'{s.name} {s.address:#x}..{s.address + s.size:#x}' for s in self.segments)
        raise ModError(f'{where}: main+{address:#x}..{address + size:#x} is outside main\'s segments ({geometry}); '
                       f'note that an IPS patch\'s offsets count the 0x100-byte NSO header (main address + 0x100) '
                       f'while a pchtxt\'s are main addresses already')

    def encode(self, image):
        """The module as an NSO file again: uncompressed segments, header fields and digests recomputed.

        The result is a normal NSO (the rewriter reads it exactly like the retail one); nothing but what the mod's
        patches changed differs in the image the game runs. The header keeps everything this script does not own -
        the build ID in particular, which selects the rewriter's registered cuts."""
        out = bytearray(self.raw[:HEADER_SIZE])
        struct.pack_into('<I', out, 0x0c, self.flags & ~0b111)          # no segment is compressed any more
        body, file_offset = bytearray(), HEADER_SIZE
        for index, segment in enumerate(self.segments):
            data = bytes(image[segment.address:segment.address + segment.size])
            struct.pack_into('<IIII', out, SEGMENT_TABLE + 16 * index, file_offset, segment.address, segment.size,
                             struct.unpack_from('<I', self.raw, SEGMENT_TABLE + 16 * index + 12)[0])
            struct.pack_into('<I', out, COMPRESSED_SIZES + 4 * index, segment.size)
            struct.pack_into('<32s', out, SEGMENT_DIGESTS + 32 * index, hashlib.sha256(data).digest())
            body += data
            file_offset += segment.size
        return bytes(out) + bytes(body)


# ---- patch formats ----
def _ips_records(data, start, offset_size, end_marker, path):
    """[(file offset, payload)] of an IPS / IPS32 patch, with the header's offsets (as emulators use them)."""
    records, position = [], start
    while True:
        if data[position:position + len(end_marker)] == end_marker:
            return records
        if position + offset_size + 2 > len(data):
            raise ModError(f'{path}: the patch ends without its {end_marker.decode()} marker')
        offset = int.from_bytes(data[position:position + offset_size], 'big')
        position += offset_size
        size = int.from_bytes(data[position:position + 2], 'big')
        position += 2
        if size == 0:                        # RLE: a repeat count and one byte
            if position + 3 > len(data):
                raise ModError(f'{path}: a run-length record is cut short')
            count, value = int.from_bytes(data[position:position + 2], 'big'), data[position + 2]
            position += 3
            payload = bytes([value]) * count
        else:
            payload = data[position:position + size]
            if len(payload) != size:
                raise ModError(f'{path}: a record needs {size} bytes, the file has {len(payload)}')
            position += size
        if not payload:
            raise ModError(f'{path}: an empty record')
        records.append((offset, payload))


def parse_ips(data, path):
    if data[:5] == b'PATCH':
        return _ips_records(data, 5, 3, b'EOF', path)
    if data[:5] == b'IPS32':
        try:
            return _ips_records(data, 5, 4, b'EEOF', path)
        except ModError:                     # some writers put a padding byte between the magic and the records
            return _ips_records(data, 6, 4, b'EEOF', path)
    raise ModError(f'{path}: not an IPS patch (it starts with {data[:5]!r}, not PATCH or IPS32)')


def parse_pchtxt(text, identity, path):
    """A community pchtxt patch for main: the same reading the runtime's TOTK_PCHTXT does, plus the format's own
    section switches.

    @enabled and @disabled switch the lines that follow on and off, so a file that turns a section off (the usual way a
    mod author keeps an edit around without applying it) applies only the sections it enabled; @stop ends the file.
    @flag offset_shift 0x100 is required before the first patch line: 0x100 is the shift an IPS form counts (the NSO
    header), which is what these addresses leave out; without it (or with another shift) the tools that read the file
    would put every edit elsewhere, so it is refused. @flag print_values only changes what those tools print: accepted.

    identity: the NSO header's build ID bytes (0x40, 20 bytes) as hex; the runtime compares 20 bytes and the
    rewriter's tables key on the first 16, and a mod may name either."""
    records, enabled, matched, shifted = [], False, False, False
    wanted, wanted16 = identity.lower(), identity.lower()[:32]
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith('@nsobid-'):
            named = line[8:].strip().lower()
            if named not in (wanted, wanted16):
                raise ModError(f'{path}:{number}: this patch is for another build of main (@nsobid-{line[8:].strip()}), '
                               f'not {wanted16}')
            matched = True
            continue
        if line.startswith('@enabled'):
            enabled = True
            continue
        if line.startswith('@disabled'):
            enabled = False
            continue
        if line.startswith('@flag'):
            shifted = _check_pchtxt_flag(line.split(), path, number) or shifted
            continue
        if line.startswith('@stop'):
            break
        if not enabled or line.startswith('@') or not line:
            continue
        fields = line.split()
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue                          # a comment or another line the game's own parser ignores too
        if len(fields) < 2:
            continue                          # ... and its sscanf() needs an address and one hex string
        try:
            payload = bytes.fromhex(fields[1])
        except ValueError:
            raise ModError(f'{path}:{number}: "{line}" is not "<main address> <hex bytes>"') from None
        if not payload:
            raise ModError(f'{path}:{number}: an empty patch line')
        if not matched:
            raise ModError(f'{path}:{number}: patches before the @nsobid line')
        if not shifted:
            raise ModError(f'{path}:{number}: no "@flag offset_shift 0x100" before the first patch line, so it is not '
                           f'certain the addresses leave the 0x100-byte NSO header out')
        records.append((address, payload))
    if not matched:
        raise ModError(f'{path}: no @nsobid line, so the patch cannot be checked against this build of main '
                       f'(it is {wanted16})')
    return records        # empty when every section is @disabled: nothing to apply, not a broken file


def _check_pchtxt_flag(fields, path, number):
    """One "@flag <name> <value>" line of a pchtxt: True for offset_shift 0x100, False for print_values (it changes only
    what the patching tools print), anything else is refused.

    These patches are applied at main addresses - the 0x100-byte NSO header is not counted, which is exactly what an
    IPS form's offsets leave out (IPS_HEADER) - so 0x100 is the one shift that describes what is done here. A file that
    asks for another shift is not applied: it would put every edit somewhere else."""
    name = fields[1] if len(fields) > 1 else ''
    value = fields[2] if len(fields) > 2 else ''
    if name == 'print_values':
        return False
    if name != 'offset_shift':
        raise ModError(f'{path}:{number}: @flag {name or "(none)"} is not supported (offset_shift 0x100 and print_values are)')
    if not value:
        raise ModError(f'{path}:{number}: @flag offset_shift needs a value (0x100)')
    try:
        shift = int(value, 16)
    except ValueError:
        raise ModError(f'{path}:{number}: @flag offset_shift {value} is not a hex number') from None
    if shift != HEADER_SIZE:
        raise ModError(f'{path}:{number}: @flag offset_shift {value} is not supported: these patches are applied at '
                       f'main addresses, so only 0x100 (the NSO header an IPS form counts) holds here')
    return True


# ---- the merge ----
def patch_records(path, nso):
    """[(main address, payload)] of one patch file, in the game's address space."""
    data = path.read_bytes()
    if path.suffix.lower() == '.pchtxt':
        return parse_pchtxt(data.decode('utf-8', 'replace'), nso.build_id, path)
    records = []
    for offset, payload in parse_ips(data, path):
        if offset < IPS_HEADER:
            raise ModError(f'{path}: the patch starts at {offset:#x}, inside the NSO header (an IPS offset is a main '
                           f'address + {IPS_HEADER:#x}, so it is never below {IPS_HEADER:#x})')
        records.append((offset - IPS_HEADER, payload))
    return records


def _is_build_id(text):
    """32 or 40 hex digits: the 16 bytes the rewriter's tables use, or the 20 the runtime compares."""
    return len(text) in (32, 40) and all(character in '0123456789abcdef' for character in text)


def patch_files(exefs, build_id):
    """(patch files, notes) of a mod's exefs folder.

    Only main can change at build time: the app loads the three modules it was built from, so an extra code module a
    mod ships is not loaded and its code never runs. That is noted loudly and recorded in the report rather than
    silently ignored - the mod's main patches and its RomFS still apply, and the player can see what stayed out.
    A whole replacement module is refused: it would have to be verified as this version's code, which is the job of
    the patches (and of games/totk/assets/expected-*.json).

    A .pchtxt says which build of main it was made for (@nsobid-); an IPS / IPS32 carries no module or build id of its
    own, so it is read as a main patch only when its name says so: main, or main's build id the way Atmosphere and the
    mod managers name patches (exefs/<build id>.ips). Anything else is left out with a note, so a patch for sdk or
    subsdk9, or one named after another build of main, can never be applied to this main by accident."""
    files = sorted(p for p in exefs.rglob('*') if p.is_file()
                   and not any(part.startswith('.') for part in p.relative_to(exefs).parts))
    build = (build_id or '').lower()
    main_names = {'main'} | ({build[:32], build} if build else set())
    patches, notes = [], []
    for path in files:
        name = path.relative_to(exefs.parent).as_posix()
        suffix, stem = path.suffix.lower(), path.stem.lower()
        if suffix == '.pchtxt':
            patches.append(path)        # its @nsobid- line is checked against this main's build id
            continue
        if suffix in PATCH_SUFFIXES:
            if stem in main_names:
                patches.append(path)
            elif stem in MODULES or MODULE_STEM.fullmatch(stem):
                notes.append(f'{name}: not merged (a patch for {stem}, and the app loads only the modules it was built '
                             f'from - main, sdk and subsdk0 - so the code it changes never runs; the mod\'s main patch '
                             f'and its RomFS still apply)')
            elif _is_build_id(stem):
                notes.append(f'{name}: not merged (the build id in its name is not this build\'s main '
                             f'({build[:32] or "unknown"}): a patch for another module, or for another version of main '
                             f'- build the app for that version instead)')
            else:
                notes.append(f'{name}: not merged (an IPS patch does not name the module or build it changes, so only '
                             f'main.ips / main.ips32 and <this build\'s main build id>.ips are read as main patches; '
                             f'rename this one, or ship it as main.pchtxt, whose @nsobid- line is checked)')
            continue
        if path.name == 'main.npdm':
            notes.append(f'{name}: not merged (the native loader never reads a module\'s npdm)')
        elif path.name == 'main':
            raise ModError(f'{name}: a whole replacement module cannot be merged; ship a patch for the retail main '
                           f'({" ".join(PATCH_SUFFIXES)}) instead, so the build can check it against this version')
        else:
            notes.append(f'{name}: not loaded (the app loads the three modules it was built from - main, sdk and '
                         f'subsdk0 - so an extra code module\'s own code never runs; the mod\'s main patches and its '
                         f'RomFS still apply)')
    return patches, notes


def skipped(args, mod, nso, retail_sha, notes, why):
    """A mod with nothing to merge into main is not a build failure - its romfs/ half still goes in. The report says
    "skipped", so the build merges nothing, says so, and carries on."""
    report = dict(format=1, status='skipped', folder=str(mod), version=args.version, build_id=nso.build_id[:32],
                  retail_main_sha256=retail_sha, patches=[], edit_bytes=0, notes=[f'{mod}/exefs: {why}'] + notes)
    report_path = (args.report.expanduser().resolve() if args.report else
                   args.output.expanduser().resolve().with_name('mod-exefs.json'))
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    for note in report['notes']:
        print(f'mod: {note}')
    print(f'mod: nothing merged into main; {args.version} is built from the retail module')
    return report


def apply(args):
    mod = args.mod.expanduser().resolve()
    exefs = mod / 'exefs'
    if not exefs.is_dir():
        raise ModError(f'{mod}: a mod folder holds its files under exefs/ (code patches) and romfs/ (data); '
                       f'this one has no exefs/')
    source = args.exefs.expanduser().resolve()
    main = source / 'main'
    if not main.is_file():
        raise ModError(f'{main}: the extracted retail ExeFS has no main module')
    raw = main.read_bytes()
    nso = Nso(raw)
    retail_sha = sha256_bytes(raw)

    patches, notes = patch_files(exefs, nso.build_id)
    if (mod / 'romfs').is_dir():
        notes.append('romfs/: not merged (data mods are loaded while the game runs, from '
                     '~/Library/Application Support/MacNativeGames/TOTK/mods/<version>; see docs/mods.md)')
    if not patches:
        return skipped(args, mod, nso, retail_sha, notes,
                       f'no code patch for main ({", ".join(PATCH_SUFFIXES)}), so nothing is merged into it')

    reserved = patch_sites.sites(args.version, nso.build_id)
    image, applied, taken = nso.image(), [], {}
    total_bytes = 0
    for path in patches:
        name = path.relative_to(mod).as_posix()
        records = patch_records(path, nso)
        if not records:
            notes.append(f'{name}: not merged (every section is @disabled, so the file applies nothing)')
            continue
        edits = []
        for address, payload in records:
            where = f'{name} (main+{address:#x})'
            nso.segment_of(address, len(payload), where)
            hits = patch_sites.overlaps([(address, len(payload))], reserved)
            if hits:
                site = hits[0][2]
                raise ModError(f'{name}: main+{address:#x}..{address + len(payload):#x} overlaps a site the build or '
                               f'the runtime patches itself ({site.label} at {site.offset:#x}); a mod patch cannot '
                               f'change it (leave that feature to the app: TOTK_FPS60, TOTK_REALTIME, TOTK_ASPECT, '
                               f'TOTK_OUTPUT_RES, TOTK_RENDER_RES)')
            for start in range(address, address + len(payload)):
                if start in taken and taken[start] != name:
                    raise ModError(f'{name} and {taken[start]} both change main+{start:#x}')
                taken[start] = name
            image[address:address + len(payload)] = payload
            edits.append(dict(address=f'{address:#x}', size=len(payload)))
            total_bytes += len(payload)
        applied.append(dict(file=name, sha256=sha256_file(path),
                            format=path.suffix.lower().lstrip('.'),
                            edits=edits, edit_count=len(edits), bytes=sum(e['size'] for e in edits)))

    if not applied:                 # patches that apply no record at all say nothing was merged, like an empty exefs/
        return skipped(args, mod, nso, retail_sha, notes, 'no patch applies a byte of main, so nothing is merged into it')
    if bytes(image) == bytes(nso.image()):
        raise ModError(f'{mod}: the patches change no byte of main (are they for another version, or already merged?)')

    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    encoded = nso.encode(image)
    output.write_bytes(encoded)
    back = Nso(output.read_bytes())
    if bytes(back.image()) != bytes(image):
        raise ModError(f'{output}: the merged module does not read back as the patched image')

    folder_sha = hashlib.sha256(''.join(f'{p.relative_to(mod).as_posix()}\t{sha256_file(p)}\n'
                                        for p in patches).encode()).hexdigest()
    report = dict(format=1, status='pass', folder=str(mod), folder_sha256=folder_sha, version=args.version,
                  build_id=nso.build_id[:32], retail_main_sha256=retail_sha, patched_main_sha256=sha256_bytes(encoded),
                  rewritten_main_sha256=None, compressions=sum(s.compressed for s in nso.segments),
                  segments=[dict(name=s.name, address=f'{s.address:#x}', size=f'{s.size:#x}') for s in nso.segments],
                  patches=applied, edit_bytes=total_bytes, reserved_sites=len(reserved), notes=notes)
    report_path = (args.report.expanduser().resolve() if args.report else output.with_name('mod-exefs.json'))
    report_path.write_text(json.dumps(report, indent=2) + '\n')

    print(f'mod: {len(patches)} code patch(es), {sum(p["edit_count"] for p in applied)} edits, {total_bytes} bytes '
          f'of main ({report["patched_main_sha256"][:16]}...)')
    print(f'mod: {args.version}, main build {report["build_id"]}; {len(reserved)} reserved sites untouched')
    for note in notes:
        print(f'mod: {note}')
    print(f'mod: merged main written to {output}')
    return report


def sites(args):
    found = patch_sites.sites(args.version, args.build_id)
    print(f'{len(found)} sites the build or the runtime patches itself in {args.version} main')
    for site in found:
        print(f'{site.offset:#010x}  {site.size:2d}  {site.label}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    merge = commands.add_parser('apply', help='merge a mod\'s main code patches into an extracted retail main')
    merge.add_argument('--exefs', type=Path, required=True, help='the extracted retail ExeFS (extracted/<version>/exefs)')
    merge.add_argument('--mod', type=Path, required=True, help='the mod folder prepared before the build')
    merge.add_argument('--version', choices=patch_sites.VERSIONS, required=True, help='the game version being built')
    merge.add_argument('--output', type=Path, required=True, help='where to write the merged main module')
    merge.add_argument('--report', type=Path, help='the JSON report (default: mod-exefs.json next to the output)')
    merge.set_defaults(run=apply)
    listed = commands.add_parser('sites', help='list the sites a mod patch must not touch')
    listed.add_argument('--version', choices=patch_sites.VERSIONS, required=True)
    listed.add_argument('--build-id', help='16 or 20 bytes of build ID as hex digits (adds that build\'s cuts)')
    listed.set_defaults(run=sites)
    args = parser.parse_args()
    try:
        args.run(args)
    except ModError as error:
        sys.exit(f'ERROR: {error}')
    except OSError as error:
        sys.exit(f'ERROR: {error}')


if __name__ == '__main__':
    main()

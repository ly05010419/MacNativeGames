#!/usr/bin/env python3
"""The shared shader list (games/totk/assets/shader_list.tsv): the one list for the Mac, iPhone and iPad, kept by everyone.

It holds no game data, only identifiers; the build turns them into shaders from the player's own game files.
  V <shader key> <deps>         a shader (SHA-256 of its NVN control section and code, as the runtime computes it) and the
                                bind state it was translated for (the translator's deps: texture, image, attribute and
                                fragment output types). The runtime picks a variant by its deps (totk_shader_rt.m
                                deps_match), so an entry is the shader plus its deps, never a variant name.
  A <suffix> <answers>          a bind state warm rows name: a variant is "<shader key>-<suffix>_b", the suffix the first 12
                                hex digits of the SHA-256 of the answers it was translated for (prepare_shader_seed.py
                                suffix, totk_shader_contrib.h totk_contrib_listed_name). The list keeps one for every
                                suffix its warm rows name but the default one (no answers), also when prune drops the V
                                rows that brought it: packaging resolves each name through it to the variant the seed
                                serves that bind state with (resolve), as the runtime picks one, so a row stays right
                                whatever names the seed gives. A warm row naming a bind state the list does not know is
                                not merged (2026-10-30: 6,855 rows named bind states whose V rows prune had dropped).
  L <shader key> <source> <control> <code> <length>
                                where a shader that is no BNSH file sits in the player's game files: <source> is a RomFS
                                path (a package member after a '/', .zs decompressed) or module:<name> (a game module's
                                image), the rest hex offsets and the code length. agl keeps its shaders in .sharcb files
                                and the game module holds a few; the build reads them there and checks them against
                                the key, so bytes of another game version are never used.
  S <variant>_b <stage> <flags> a shader library the warm-up loads,
  C <variant>_b <flags>         a compute pipeline, and
  R <pipeline key> <flags>      a render pipeline it builds;
  B <pipeline key> <vertex> <fragment> <flags>
                                the same pipeline with the border colours its samplers had (nvn_mtl.m border_parse): per
                                stage "<argument id>:<axes>:<r>,<g>,<b>,<a>;" for each sampler, colours as C %a floats,
                                or - for none; the warm-up builds the shader variants that emulate them and that pipeline.
  T <pipeline key> <tile> <flags>
                                the same pipeline with the fragment variant that reads the draw's own colour attachments
                                from the tile (nvn_mtl.m att_fb_take / warm_tileread): "<argument id>:<attachment>;" per
                                read. Recorded only in pso_user.tsv before, so every new install built these in play
                                (58 of the 58 a 5-minute route built in the world with the list, 2026-10-26).
                                Flags t: the title screen's (the first frame waits for these only), m: the rest of the
                                warm-up. Warm rows keep the order they were recorded in.
  A 544-byte pipeline key is kept packed in the TOTK and BotW lists (pack_key): "<vertex variant>:<fragment variant>:<the
  other 384 bytes in hex, each run of 4 or more zero bytes as z<count in hex>.>", about 280 characters instead of 1,088
  (the TOTK list was 79 MB, 71 MB of it render pipeline keys, 2026-10-28). Rows read either form; exports write hex.

  shader_list.py merge [--title] <list> <source>...  add a translation cache folder (its *.variants: shaders only; a cache's
                                                     pso_collected.tsv holds every pipeline ever built, tens of thousands,
                                                     so pipelines join only from a file named here), or a pso_user.tsv /
                                                     pso_collected.tsv / older list file
  shader_list.py resolve <seed folder> <list> <pso_manifest.tsv> <pso_title.tsv>
                                                     the two files the runtime reads (nvn_mtl.m warm_start) for the game
                                                     version of that seed: rows of other versions' shaders left out, each
                                                     variant named as the seed serves its bind state, rows it cannot
                                                     build (a shader or bind state it failed to translate) left out
  shader_list.py export <list> <pso_manifest.tsv> <pso_title.tsv>   the two files as listed, every game version's
                                                     rows under the names the list keeps (games/totk/Makefile, for
                                                     development runs; an app gets resolve's)
  shader_list.py export <list> <pso_manifest.tsv>   one file with every warm row as listed, title rows first, for a
                                                     runtime without a title list (the original port, native-port)
  shader_list.py prune <list> <seed folder>          drop the V rows the seed's built-in variants serve (prepare_shader_seed.py
                                                     builtin_serves): the list keeps only non-default variants
  shader_list.py stats <list>                        check the list and count it
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

HEADER = ('# TOTK shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md). The one list for every platform.\n'
          '# Identifiers and pipeline descriptors only, no game data. Translation chain totk-native-v8-unverified.\n')
HEADERS = {'botw': ('# Breath of the Wild shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md). The one list for every platform.\n'
                    '# Identifiers and pipeline descriptors only, no game data. Translation chain totk-native-v8-unverified.\n')}
EXPORT = '# Generated from {} by scripts/shader_list.py; edit that list, not this file.\n'
KEY = re.compile(r'^[0-9a-f]{64}$')
VARIANT = re.compile(r'^[0-9a-f]{64}-[0-9a-f]{12}_b$')
PIPELINE = re.compile(r'^(?:[0-9a-f]{2})+$')
HEX = re.compile(r'^[0-9a-f]{1,10}$')
_HEXFLOAT = r'-?0x[0-9a-f]+(?:\.[0-9a-f]*)?p[-+]\d+'
# nvn_mtl.m border_key: per sampler "<argument id>:<axes>:<r>,<g>,<b>,<a>", a filtering sampler adding
# ":f<ws>,<wt>,<wr>,<magf>,<minf>,<aniso>,<lod min>,<lod max>" before ';' (the runtime has written it so for a while: without
# it every B row a player sent was dropped, 51 of 51 on 2026-10-22)
_FILTER = rf':f\d+,\d+,\d+,\d+,\d+,{_HEXFLOAT},{_HEXFLOAT},(?:{_HEXFLOAT}|inf)'
BORDER = re.compile(rf'^(?:-|(?:\d+:[0-7]:{_HEXFLOAT},{_HEXFLOAT},{_HEXFLOAT},{_HEXFLOAT}(?:{_FILTER})?;){{1,8}})$')
TILE = re.compile(r'^(?:\d+:[0-7];){1,8}$')   # nvn_mtl.m g_pso_fbrec: "<argument id>:<colour attachment>;" per tile read
SOURCE = re.compile(r'^(?:module:[a-z0-9]+|[A-Za-z0-9_.()+-]+(?:/[A-Za-z0-9_.()+-]+)*)$')
DEPS_FIELDS = {'tex', 'img', 'attr', 'fout'}
SUFFIX_RE = re.compile(r'^[0-9a-f]{12}$')


def answers_suffix(answers):
    """The suffix a variant translated for these answers (compact sorted JSON) is named with: prepare_shader_seed.py suffix."""
    return hashlib.sha256(answers.encode()).hexdigest()[:12]


DEFAULT_SUFFIX = answers_suffix('{"attr":{},"fout":{},"img":{},"tex":{}}')   # no answers: prepare_shader_seed.py SUFFIX


def listed_answers(deps):
    """The answers a build translates a V row's deps (compact JSON) with, as compact JSON: prepare_shader_seed.py
    answers_for, so its variant is named "<key>-" + answers_suffix of them (and so is a pipeline row's, by the runtime's
    totk_contrib_listed_name)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # also when run with python -I
    from prepare_shader_seed import answers_for   # it imports this module
    return json.dumps(answers_for(json.loads(deps)), sort_keys=True, separators=(',', ':'))


def canonical(deps):
    """The deps as one compact, sorted JSON text, or None when they are not the translator's deps."""
    if not isinstance(deps, dict) or not set(deps) <= DEPS_FIELDS:
        return None
    def slot(v):   # a type name, or for an image slot {"type": ..., "fmt": ...}
        return isinstance(v, str) or isinstance(v, dict) and all(isinstance(k, str) and isinstance(x, str) for k, x in v.items())
    for value in deps.values():
        if not isinstance(value, dict) or not all(isinstance(k, str) and slot(v) for k, v in value.items()):
            return None
    return json.dumps(deps, sort_keys=True, separators=(',', ':'))


def locator(fields):
    """('L', key, source, control, code, length) as (key, source, control, code, length), or None."""
    if len(fields) != 6 or fields[0] != 'L' or not KEY.match(fields[1]) or not SOURCE.match(fields[2]):
        return None
    if any(part in ('.', '..') for part in fields[2].split('/')) or not all(HEX.match(f) for f in fields[3:]):
        return None
    return (fields[1], fields[2], int(fields[3], 16), int(fields[4], 16), int(fields[5], 16))


# nvn_pso_skey.h pso_skey as the runtime writes it (544 bytes): merged flag, colour formats, blend enables, write masks and
# one 8-byte blend state per colour target
SKEY_SIZE, SKEY_MERGED, SKEY_CFMT, SKEY_BLEND_EN, SKEY_CHMASK, SKEY_BLEND = 544, 162, 216, 260, 264, 272


def pipeline_key(key):
    """A render pipeline key as nvn_pso_skey.h pso_key_norm leaves it: blend factors of a target that does not blend, and
    outside a merged pass the write mask and blend bit of a missing colour target, cleared. Rows recorded before the runtime
    did so differ only there (9,057 render pipelines of the list were 5,611, 2026-10-22); keys of another size stay."""
    if len(key) != 2 * SKEY_SIZE:
        return key
    b = bytearray.fromhex(key)
    merged, cfmt = b[SKEY_MERGED], struct.unpack_from('<8I', b, SKEY_CFMT)
    (blend_en,), (chmask,) = struct.unpack_from('<I', b, SKEY_BLEND_EN), struct.unpack_from('<I', b, SKEY_CHMASK)
    for i in range(8):
        if not merged and not cfmt[i]:
            blend_en &= ~(1 << i); chmask &= ~(0xf << (4 * i))
        if not blend_en >> i & 1:
            b[SKEY_BLEND + 8 * i:SKEY_BLEND + 8 * i + 8] = bytes(8)
    struct.pack_into('<I', b, SKEY_BLEND_EN, blend_en); struct.pack_into('<I', b, SKEY_CHMASK, chmask)
    return b.hex()


PACKED = re.compile(r'^([0-9a-f_-]{0,80}):([0-9a-f_-]{0,80}):((?:[0-9a-f]{2}|z[0-9a-f]+\.)*)$')
ZERO_RUN = re.compile(rb'\0{4,}')


def unpack_key(text):
    """A pipeline key as hex: a packed one (pack_key) unpacked, anything else as it is."""
    m = PACKED.match(text)
    if not m:
        return text
    vs, fs, rest = m.groups()
    key = (vs.encode().hex().ljust(2 * NAME, '0') + fs.encode().hex().ljust(2 * NAME, '0')
           + re.sub(r'z([0-9a-f]+)\.', lambda z: '00' * int(z.group(1), 16), rest))
    return key if len(key) == 2 * SKEY_SIZE else text


def pack_key(key):
    """A 544-byte pipeline key as the TOTK and BotW lists keep it; a key that would not unpack to itself stays hex."""
    if len(key) != 2 * SKEY_SIZE or not PIPELINE.match(key):
        return key
    b = bytes.fromhex(key)
    vs, fs = b[:NAME].rstrip(b'\0'), b[NAME:2 * NAME].rstrip(b'\0')
    rest, at = [], 2 * NAME
    for m in ZERO_RUN.finditer(b, at):
        rest += [b[at:m.start()].hex(), f'z{m.end() - m.start():x}.']; at = m.end()
    try:
        text = f'{vs.decode()}:{fs.decode()}:' + ''.join(rest) + b[at:].hex()
    except UnicodeDecodeError:
        return key
    return text if unpack_key(text) == key else key


def warm_row(fields):
    """A warm row without flags ('S', variant, stage) / ('C', variant) / ('R', key) / ('B', key, vertex, fragment) /
    ('T', key, tile), or None when it is not one. Pipeline keys come back as hex, packed ones unpacked."""
    kind = fields[0] if fields else ''
    if kind in ('R', 'B', 'T') and len(fields) >= 2:
        fields = [kind, unpack_key(fields[1]), *fields[2:]]
    if kind == 'S' and len(fields) >= 3 and VARIANT.match(fields[1]) and fields[2] in ('0', '1', '5'):
        return tuple(fields[:3])
    if kind == 'C' and len(fields) >= 2 and VARIANT.match(fields[1]):
        return tuple(fields[:2])
    if kind == 'R' and len(fields) >= 2 and PIPELINE.match(fields[1]):
        return ('R', pipeline_key(fields[1]))
    if (kind == 'B' and len(fields) >= 4 and PIPELINE.match(fields[1]) and BORDER.match(fields[2]) and BORDER.match(fields[3])
            and fields[2:4] != ['-', '-']):
        return ('B', pipeline_key(fields[1]), fields[2], fields[3])
    if kind == 'T' and len(fields) >= 3 and PIPELINE.match(fields[1]) and TILE.match(fields[2]):
        return ('T', pipeline_key(fields[1]), fields[2])
    return None


def list_name(path):
    """How the export names its list: games/<game>/assets/<file> when it is kept there, else the file name."""
    parts = Path(path).resolve().parts
    return '/'.join(parts[-4:]) if len(parts) >= 4 and parts[-4] == 'games' and parts[-2] == 'assets' else Path(path).name


def list_game(path):
    """The game folder a list is kept in (games/<game>/assets/...), or None."""
    parts = Path(path).resolve().parts
    return parts[-3] if len(parts) >= 3 and parts[-2] == 'assets' else None


def header_for(path):
    """The list's header comment: by the game folder it is kept in (games/<game>/assets/...), Tears of the Kingdom's otherwise."""
    return HEADERS.get(list_game(path), HEADER)


class ShaderList:
    def __init__(self):
        self.variants = set()   # (key, deps)
        self.warm = {}          # warm row -> flags; dicts keep insertion order, the order rows were recorded in
        self.locators = set()   # (key, source, control offset, code offset, length)
        self.path = None
        self.answers = {}       # suffix -> answers (compact JSON): the bind states warm rows name (A rows)
        self.refused = 0        # warm rows merged from a source that named a bind state the list does not know

    @classmethod
    def load(cls, path):
        lst = cls()
        path = Path(path)
        lst.path = path
        if not path.exists():
            return lst
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            if fields[0] == 'V' and len(fields) == 3 and KEY.match(fields[1]):
                try:
                    if canonical(json.loads(fields[2])) == fields[2]:
                        lst.add_variant(fields[1], fields[2])
                        continue
                except ValueError:
                    pass
            if fields[0] == 'A' and len(fields) == 3 and SUFFIX_RE.match(fields[1]) and answers_suffix(fields[2]) == fields[1]:
                lst.answers[fields[1]] = fields[2]
                continue
            if fields[0] == 'L':
                where = locator(fields)
                if where:
                    lst.locators.add(where)
                    continue
                raise ValueError(f'{path}:{number}: not a shader list row')
            row = warm_row(fields[:-1])
            if row and len(fields) == len(row) + 1 and fields[-1] in ('t', 'm', 'tm'):
                lst.add_warm(row, fields[-1])   # rows that only differed in what pipeline_key clears become one
                continue
            raise ValueError(f'{path}:{number}: not a shader list row')
        return lst

    def save(self, path):
        lines = [f'V\t{key}\t{deps}' for key, deps in sorted(self.variants)]
        if list_game(path) in (None, 'totk', 'botw'):   # other games' own tools know no A rows
            named = {n[:-2].rpartition('-')[2] for row in self.warm for n in row_variants(row) if VARIANT.match(n)}
            lines += [f'A\t{suffix}\t{answers}' for suffix, answers in sorted(self.answers.items()) if suffix in named]
        lines += [f'L\t{k}\t{src}\t{c:x}\t{d:x}\t{n:x}' for k, src, c, d, n in sorted(self.locators)]
        packed = list_game(path) in (None, 'totk', 'botw')   # other games' own tools read their lists as hex
        lines += ['\t'.join((row[0], pack_key(row[1]), *row[2:]) if packed and row[0] in ('R', 'B', 'T') else row) + '\t' + flags
                  for row, flags in self.warm.items()]
        Path(path).write_text(header_for(path) + ''.join(line + '\n' for line in lines))

    def add_variant(self, key, deps):
        """A V row, and the bind state its variant's name stands for."""
        self.variants.add((key, deps))
        answers = listed_answers(deps)
        self.answers[answers_suffix(answers)] = answers

    def knows(self, row):
        """Whether every variant a warm row names is the default one or one of a bind state the list keeps."""
        return all(name[:-2].rpartition('-')[2] in self.answers or name[:-2].rpartition('-')[2] == DEFAULT_SUFFIX
                   for name in row_variants(row) if VARIANT.match(name))

    def add_warm(self, row, flag):
        old = self.warm.get(row, '')
        self.warm[row] = ''.join(f for f in 'tm' if f in old + flag)

    def add_cache(self, folder):
        for index in sorted(Path(folder).glob('*.variants')):
            if not KEY.match(index.stem):
                continue
            try:
                entries = json.loads(index.read_text())
            except (OSError, ValueError):
                continue
            for entry in entries if isinstance(entries, list) else []:
                deps = canonical(entry.get('deps')) if isinstance(entry, dict) else None
                if deps is not None:
                    self.add_variant(index.stem, deps)

    def add_rows(self, path, flag):
        """A contribution file, a pso_user.tsv / pso_collected.tsv or another copy of a list: its V, A and L rows first,
        then the warm rows whose bind states are known with them (the rest counted in refused)."""
        lines = [line.split('\t') for line in Path(path).read_text().splitlines() if line and not line.startswith('#')]
        for fields in lines:
            if fields[0] == 'V' and len(fields) == 3 and KEY.match(fields[1]):
                try:
                    deps = canonical(json.loads(fields[2]))
                except ValueError:
                    deps = None
                if deps is not None:
                    self.add_variant(fields[1], deps)
            elif fields[0] == 'A' and len(fields) == 3 and SUFFIX_RE.match(fields[1]):
                try:
                    answers = canonical(json.loads(fields[2]))
                except ValueError:
                    answers = None
                if answers is not None and answers_suffix(answers) == fields[1]:
                    self.answers[fields[1]] = answers
            elif fields[0] == 'L':
                where = locator(fields)
                if where:
                    self.locators.add(where)
        for fields in lines:
            row = warm_row(fields) if fields[0] in 'SCRBT' else None
            if not row:
                continue
            if not self.knows(row):
                self.refused += 1
                continue
            listed = fields[len(row)] if len(fields) > len(row) and fields[len(row)] in ('t', 'm', 'tm') else ''
            self.add_warm(row, listed or flag)

    def export(self, *flags, source='games/totk/assets/shader_list.tsv'):
        """The warm rows carrying one of these flags; rows of the first flag come first, each row once."""
        out, seen = [], set()
        for flag in flags:
            for row, have in self.warm.items():
                if flag in have and row not in seen:
                    out.append(row); seen.add(row)
        return EXPORT.format(source) + ''.join('\t'.join(row) + '\n' for row in out)

    def resolve(self, seed, flag, given=()):
        """The warm rows carrying this flag for the game version of a seed folder, each variant named as the seed serves
        its bind state, without those in given (rows resolved before) and those naming a shader of another version
        (neither in the seed's shader-keys.json nor in the seed) or one the seed cannot build. Returns rows and counts."""
        from prepare_shader_seed import deps_match   # it imports this module
        seed = Path(seed)
        keys = set(json.loads((seed/'shader-keys.json').read_text())['keys'])
        index, names = {}, {}
        def variants_of(key):   # the seed's variants of a shader, as totk_shader_rt.m variants_of
            if key not in index:
                try:
                    rows = json.loads((seed/f'{key}.variants').read_text())
                except (OSError, ValueError):
                    rows = []
                index[key] = [r for r in rows if isinstance(r, dict) and isinstance(r.get('variant'), str)]
            return index[key]
        def served(name):   # "<key>-<suffix>_b" -> the seed's name for it, '' another version's, None the seed lacks it
            if name not in names and not VARIANT.match(name):   # no variant name (a hand-made row): as it is
                names[name] = name
            if name not in names:
                key, _, suffix = name[:-2].partition('-')
                mine = variants_of(key)
                if not mine and key not in keys:
                    names[name] = ''
                elif any(r['variant'] == name[:-2] for r in mine):   # the seed carries it (its default, depth-bound or a listed one)
                    names[name] = name
                elif same := next((r['variant'] for r in mine if r['variant'].endswith('-' + suffix)), None):
                    names[name] = same + '_b'   # the same bind state of the shader translated for this one (prepare_shader_seed.py translation_input)
                elif suffix == DEFAULT_SUFFIX:
                    names[name] = None
                else:
                    answers = json.loads(self.answers[suffix])
                    hit = next((r['variant'] for r in mine if deps_match(r.get('deps') or {}, answers)), None)
                    names[name] = hit+'_b' if hit else None
            return names[name]
        out, seen, counts = [], set(given), dict(other=0, missing=0, renamed=0, merged=0)
        for row, have in self.warm.items():
            if flag not in have:
                continue
            old = row_variants(row)
            new = [served(name) for name in old]
            if '' in new:
                counts['other'] += 1; continue
            if None in new:
                counts['missing'] += 1; continue
            renamed = rename_warm(row, dict(zip(old, new)))
            counts['renamed'] += renamed != row
            if renamed in seen:
                counts['merged'] += 1; continue
            seen.add(renamed); out.append(renamed)
        return out, counts

    def counts(self):
        kinds = {k: sum(1 for row in self.warm if row[0] == k) for k in 'SCRBT'}
        title = sum(1 for flags in self.warm.values() if 't' in flags)
        return (f'{len(self.variants)} variants over {len({k for k, _ in self.variants})} shaders, '
                f'{len(self.answers)} bind states, {len(self.locators)} located; warm rows: '
                f'{kinds["S"]} S, {kinds["C"]} C, {kinds["R"]} R, {kinds["B"]} B, {kinds["T"]} T ({title} for the title screen)')


NAME = 80   # nvn_pso_skey.h pso_skey: char vs[80], fs[80] lead the pipeline key, each a NUL-padded "<variant>_b"


def row_variants(row):
    """The variant names ("<key>-<suffix>_b") a warm row names: S / C its variant, R / B / T the two in the key."""
    if row[0] in ('S', 'C'):
        return [row[1]]
    key = bytes.fromhex(row[1])
    return [n for n in (key[at:at+NAME].split(b'\0')[0].decode('ascii', 'replace') for at in (0, NAME)) if n]


def rename_warm(row, names):
    """A warm row with the variants it names replaced (names: "<variant>_b" -> "<variant>_b")."""
    if row[0] in ('S', 'C'):
        return (row[0], names.get(row[1], row[1]), *row[2:])
    key = bytearray.fromhex(row[1])
    for at in (0, NAME):
        old = bytes(key[at:at+NAME]).split(b'\0')[0].decode('ascii', 'replace')
        new = names.get(old, old)
        if new != old:
            if len(new) >= NAME:
                raise ValueError(f'variant name {new} does not fit a pipeline key')
            key[at:at+NAME] = new.encode().ljust(NAME, b'\0')
    return (row[0], key.hex(), *row[2:])


def main(argv):
    if argv[:1] == ['merge'] and len(argv) >= 3:
        title = argv[1] == '--title'
        args = argv[2:] if title else argv[1:]
        if len(args) < 2:
            return usage()
        lst = ShaderList.load(args[0])
        before = len(lst.variants) + len(lst.warm) + len(lst.locators)
        for source in args[1:]:
            if Path(source).is_dir():
                lst.add_cache(source)
            else:
                lst.add_rows(source, 't' if title else 'm')
        lst.save(args[0])
        refused = f'; {lst.refused} warm rows refused: they name bind states neither the list nor the source keeps' if lst.refused else ''
        print(f'{len(lst.variants) + len(lst.warm) + len(lst.locators) - before} new; {lst.counts()}{refused}')
    elif argv[:1] == ['resolve'] and len(argv) == 5:
        lst = ShaderList.load(argv[2])
        title, counts_t = lst.resolve(argv[1], 't')
        rest, counts_m = lst.resolve(argv[1], 'm', given=title)   # the runtime reads both: a row goes where it is first
        for path, rows, counts in ((argv[4], title, counts_t), (argv[3], rest, counts_m)):
            Path(path).unlink(missing_ok=True)   # often a clone of an older file: written anew, not through the shared blocks
            Path(path).write_text(EXPORT.format(list_name(argv[2])) + ''.join('\t'.join(row) + '\n' for row in rows))
            print(f'{path}: {len(rows)} rows; left out {counts["other"]} of other game versions\' shaders and '
                  f'{counts["missing"]} the seed cannot build; {counts["renamed"]} named as the seed serves them, '
                  f'{counts["merged"]} rows given before')
    elif argv[:1] == ['export'] and len(argv) == 4:
        lst = ShaderList.load(argv[1])
        Path(argv[2]).write_text(lst.export('m', source=list_name(argv[1])))
        Path(argv[3]).write_text(lst.export('t', source=list_name(argv[1])))
    elif argv[:1] == ['export'] and len(argv) == 3:
        Path(argv[2]).write_text(ShaderList.load(argv[1]).export('t', 'm', source=list_name(argv[1])))
    elif argv[:1] == ['prune'] and len(argv) == 3:
        sys.path.insert(0, str(Path(__file__).resolve().parent))   # also when run with python -I
        from prepare_shader_seed import builtin_serves   # it imports this module
        lst = ShaderList.load(argv[1])
        before = len(lst.variants)
        lst.variants = {(key, deps) for key, deps in lst.variants if not builtin_serves(argv[2], key, deps)}
        lst.save(argv[1])
        print(f'{before - len(lst.variants)} rows the seed serves dropped; {lst.counts()}')
    elif argv[:1] == ['stats'] and len(argv) == 2:
        lst = ShaderList.load(argv[1])
        lines = [l for l in Path(argv[1]).read_text().splitlines() if l and not l.startswith('#')]
        variant_lines = [l for l in lines if l.startswith('V\t')]
        unknown = sum(1 for row in lst.warm if not lst.knows(row))
        if unknown:
            raise ValueError(f'{argv[1]}: {unknown} warm rows name a bind state the list has no A row for')
        answer_lines = sum(1 for l in lines if l.startswith('A\t'))
        if variant_lines != sorted(variant_lines) or len(lines) != len(lst.variants) + answer_lines + len(lst.warm) + len(lst.locators):
            raise ValueError(f'{argv[1]}: variant rows must be sorted and no row repeated (merge rewrites it so)')
        print(lst.counts())
    else:
        return usage()
    return 0


def usage():
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (ValueError, OSError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)

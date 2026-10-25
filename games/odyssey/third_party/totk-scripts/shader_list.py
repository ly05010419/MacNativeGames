#!/usr/bin/env python3
"""The shared shader list (games/totk/assets/shader_list.tsv): the one list for the Mac, iPhone and iPad, kept by everyone.

It holds no game data, only identifiers; the build turns them into shaders from the player's own game files.
  V <shader key> <deps>         a shader (SHA-256 of its NVN control section and code, as the runtime computes it) and the
                                bind state it was translated for (the translator's deps: texture, image, attribute and
                                fragment output types). The runtime picks a variant by its deps (totk_shader_rt.m
                                deps_match), so an entry is the shader plus its deps, never a variant name.
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
                                Flags t: the title screen's (the first frame waits for these only), m: the rest of the
                                warm-up. Warm rows keep the order they were recorded in.

  shader_list.py merge [--title] <list> <source>...  add a translation cache folder (its *.variants: shaders only; a cache's
                                                     pso_collected.tsv holds every pipeline ever built, tens of thousands,
                                                     so pipelines join only from a file named here), or a pso_user.tsv /
                                                     pso_collected.tsv / older list file
  shader_list.py export <list> <pso_manifest.tsv> <pso_title.tsv>   the two files the runtime reads (nvn_mtl.m warm_start)
  shader_list.py export <list> <pso_manifest.tsv>   one file with every warm row, title rows first, for a runtime
                                                     without a title list (the original port, native-port)
  shader_list.py rename <seed>.aliases.tsv <file>... rename the variants exported warm rows name through the aliases the
                                                     seed's preparation wrote (listed variants another one serves)
  shader_list.py prune <list> <seed folder>          drop the V rows the seed's built-in variants serve (prepare_shader_seed.py
                                                     builtin_serves): the list keeps only non-default variants
  shader_list.py stats <list>                        check the list and count it
"""
import json
import re
import sys
from pathlib import Path

HEADER = ('# TOTK shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md). The one list for every platform.\n'
          '# Identifiers and pipeline descriptors only, no game data. Translation chain totk-native-v7-unverified.\n')
HEADERS = {'odyssey': ('# Super Mario Odyssey shader list v1 (third_party/totk-scripts/shader_list.py; docs/shader-list.md). Mac only.\n'
                       '# Identifiers and pipeline descriptors only, no game data. Translation chain totk-native-v4-unverified (games/odyssey).\n'),
           'botw': ('# Breath of the Wild shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md). The one list for every platform.\n'
                    '# Identifiers and pipeline descriptors only, no game data. Translation chain totk-native-v7-unverified.\n')}
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
SOURCE = re.compile(r'^(?:module:[a-z0-9]+|[A-Za-z0-9_.()+-]+(?:/[A-Za-z0-9_.()+-]+)*)$')
DEPS_FIELDS = {'tex', 'img', 'attr', 'fout'}


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


def warm_row(fields):
    """A warm row without flags ('S', variant, stage) / ('C', variant) / ('R', key) / ('B', key, vertex, fragment), or None
    when it is not one."""
    kind = fields[0] if fields else ''
    if kind == 'S' and len(fields) >= 3 and VARIANT.match(fields[1]) and fields[2] in ('0', '1', '5'):
        return tuple(fields[:3])
    if kind == 'C' and len(fields) >= 2 and VARIANT.match(fields[1]):
        return tuple(fields[:2])
    if kind == 'R' and len(fields) >= 2 and PIPELINE.match(fields[1]):
        return tuple(fields[:2])
    if (kind == 'B' and len(fields) >= 4 and PIPELINE.match(fields[1]) and BORDER.match(fields[2]) and BORDER.match(fields[3])
            and fields[2:4] != ['-', '-']):
        return tuple(fields[:4])
    return None


def list_name(path):
    """How the export names its list: games/<game>/assets/<file> when it is kept there, else the file name."""
    parts = Path(path).resolve().parts
    return '/'.join(parts[-4:]) if len(parts) >= 4 and parts[-4] == 'games' and parts[-2] == 'assets' else Path(path).name


def header_for(path):
    """The list's header comment: by the game folder it is kept in (games/<game>/assets/...), Tears of the Kingdom's otherwise."""
    parts = Path(path).resolve().parts
    game = parts[-3] if len(parts) >= 3 and parts[-2] == 'assets' else None
    return HEADERS.get(game, HEADER)


class ShaderList:
    def __init__(self):
        self.variants = set()   # (key, deps)
        self.warm = {}          # warm row -> flags; dicts keep insertion order, the order rows were recorded in
        self.locators = set()   # (key, source, control offset, code offset, length)

    @classmethod
    def load(cls, path):
        lst = cls()
        path = Path(path)
        if not path.exists():
            return lst
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            if fields[0] == 'V' and len(fields) == 3 and KEY.match(fields[1]):
                try:
                    if canonical(json.loads(fields[2])) == fields[2]:
                        lst.variants.add((fields[1], fields[2]))
                        continue
                except ValueError:
                    pass
            if fields[0] == 'L':
                where = locator(fields)
                if where:
                    lst.locators.add(where)
                    continue
                raise ValueError(f'{path}:{number}: not a shader list row')
            row = warm_row(fields[:-1])
            if row and len(fields) == len(row) + 1 and fields[-1] in ('t', 'm', 'tm'):
                lst.warm[row] = fields[-1]
                continue
            raise ValueError(f'{path}:{number}: not a shader list row')
        return lst

    def save(self, path):
        lines = [f'V\t{key}\t{deps}' for key, deps in sorted(self.variants)]
        lines += [f'L\t{k}\t{src}\t{c:x}\t{d:x}\t{n:x}' for k, src, c, d, n in sorted(self.locators)]
        lines += ['\t'.join(row + (flags,)) for row, flags in self.warm.items()]
        Path(path).write_text(header_for(path) + ''.join(line + '\n' for line in lines))

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
                    self.variants.add((index.stem, deps))

    def add_rows(self, path, flag):
        for line in Path(path).read_text().splitlines():
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            if fields[0] == 'V' and len(fields) == 3 and KEY.match(fields[1]):   # another copy of this list
                try:
                    deps = canonical(json.loads(fields[2]))
                except ValueError:
                    deps = None
                if deps is not None:
                    self.variants.add((fields[1], deps))
                continue
            if fields[0] == 'L':
                where = locator(fields)
                if where:
                    self.locators.add(where)
                continue
            row = warm_row(fields)
            if row:
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

    def counts(self):
        kinds = {k: sum(1 for row in self.warm if row[0] == k) for k in 'SCRB'}
        title = sum(1 for flags in self.warm.values() if 't' in flags)
        return (f'{len(self.variants)} variants over {len({k for k, _ in self.variants})} shaders, '
                f'{len(self.locators)} located; warm rows: '
                f'{kinds["S"]} S, {kinds["C"]} C, {kinds["R"]} R, {kinds["B"]} B ({title} for the title screen)')


NAME = 80   # nvn_pso_skey.h pso_skey: char vs[80], fs[80] lead the pipeline key, each a NUL-padded "<variant>_b"


def load_aliases(path):
    """<seed>.aliases.tsv (prepare_shader_seed.py write_aliases): variant -> the variant the seed carries in its place."""
    aliases = {}
    for line in Path(path).read_text().splitlines():
        old, new = line.split('\t')
        if not (VARIANT.match(old+'_b') and VARIANT.match(new+'_b')):
            raise ValueError(f'{path}: not an alias row: {line!r}')
        aliases[old] = new
    return aliases


def rename_row(line, aliases):
    """A warm row with the variants it names renamed through aliases (S / C: the variant; R / B: the two names in the
    pipeline key), anything else as it is."""
    fields = line.split('\t')
    def name(v):   # "<variant>_b"
        return aliases.get(v[:-2], v[:-2])+'_b' if v.endswith('_b') else v
    if fields[0] in ('S', 'C') and len(fields) >= 2:
        fields[1] = name(fields[1])
    elif fields[0] in ('R', 'B') and len(fields) >= 2 and PIPELINE.match(fields[1]) and len(fields[1]) >= 4*NAME:
        key = bytearray.fromhex(fields[1])
        for at in (0, NAME):
            old = bytes(key[at:at+NAME]).split(b'\0')[0].decode('ascii', 'replace')
            new = name(old)
            if new != old:
                if len(new) >= NAME:
                    raise ValueError(f'variant name {new} does not fit a pipeline key')
                key[at:at+NAME] = new.encode().ljust(NAME, b'\0')
        fields[1] = key.hex()
    return '\t'.join(fields)


def rename(text, aliases):
    """An exported warm-up list (or a seed's pso_manifest.tsv) with its variants renamed; a row renamed into one it
    already has is dropped."""
    out, seen = [], set()
    for line in text.splitlines():
        row = rename_row(line, aliases) if line and not line.startswith('#') else line
        if row and not row.startswith('#'):
            if row in seen:
                continue
            seen.add(row)
        out.append(row)
    return ''.join(line+'\n' for line in out)


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
        print(f'{len(lst.variants) + len(lst.warm) + len(lst.locators) - before} new; {lst.counts()}')
    elif argv[:1] == ['export'] and len(argv) == 4:
        lst = ShaderList.load(argv[1])
        Path(argv[2]).write_text(lst.export('m', source=list_name(argv[1])))
        Path(argv[3]).write_text(lst.export('t', source=list_name(argv[1])))
    elif argv[:1] == ['export'] and len(argv) == 3:
        Path(argv[2]).write_text(ShaderList.load(argv[1]).export('t', 'm', source=list_name(argv[1])))
    elif argv[:1] == ['rename'] and len(argv) >= 3:
        aliases = load_aliases(argv[1])
        for path in argv[2:]:
            text = Path(path).read_text()
            renamed = rename(text, aliases)
            if renamed != text:   # often a clone of the exported file: written anew, not through the shared blocks
                Path(path).unlink()
                Path(path).write_text(renamed)
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
        if variant_lines != sorted(variant_lines) or len(lines) != len(lst.variants) + len(lst.warm) + len(lst.locators):
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

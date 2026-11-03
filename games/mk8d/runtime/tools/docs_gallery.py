#!/usr/bin/env python3
# Writes the games gallery in README.md (between its gallery markers) from docs/images/gallery.tsv: one cell per row of the table, three to a row, in the table's order.
# The table is the source: a new game is a row in it (usually at the end), so two pull requests that each add a game change one line each instead of moving every cell after
# the first one; .gitattributes merges the table with the union driver. Columns: image (a file in docs/images), name (the caption and the image's alt text), note (the small
# line under it, in small print like the caption: the IPA's size, then " · " and a remark if any; omit when the size is unknown) and, optionally, the base game's title ID, which links to its game note.
# Run it after changing the table; --check only reports whether README.md is current (dm/tools/tests/test_docs.py runs it).
# Usage: docs_gallery.py [--check]
import html, pathlib, re, sys

REPO = pathlib.Path(__file__).resolve().parents[2]
TABLE = REPO / 'docs' / 'images' / 'gallery.tsv'
OUT = REPO / 'README.md'
BEGIN = '<!-- gallery: written by dm/tools/docs_gallery.py from docs/images/gallery.tsv; change the table, then run it -->'
END = '<!-- end of gallery -->'
PER_ROW = 3


def rows():
    out = []
    seen = {'image': {}, 'name': {}, 'title ID': {}}
    for n, line in enumerate(TABLE.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) not in (2, 3, 4): sys.exit(f'{TABLE.name}:{n}: 2 to 4 columns expected, {len(c)} found')
        image, name, note, tid = c + [''] * (4 - len(c))
        if not (REPO / 'docs' / 'images' / image).is_file(): sys.exit(f'{TABLE.name}:{n}: no image docs/images/{image}')
        if tid and not re.fullmatch(r'[0-9A-F]{16}', tid): sys.exit(f'{TABLE.name}:{n}: a title ID is 16 hex digits: {tid}')
        # A union merge can retain two different captions for the same game, so check its image and title ID too.
        for field, value in (('image', image), ('name', name), ('title ID', tid)):
            if not value: continue
            if value in seen[field]:
                sys.exit(f'{TABLE.name}:{n}: duplicate {field} {value!r} (first at line {seen[field][value]}): merge the rows by hand')
            seen[field][value] = n
        out.append((image, name, note, tid))
    return out


def cell(image, name, note, tid):
    # Attributes use double quotes; apostrophes in existing captions need no escaping.
    image, name, note = (html.escape(s, quote=False).replace('"', '&quot;') for s in (image, name, note))
    caption = f'<a href="dm/games/{tid}/README.md">{name}</a>' if tid else name
    detail = f'<br>{note}' if note else ''   # a game whose IPA size is still open has only its caption
    # All in small print: GitHub keeps its own line height in a README, so smaller text is what makes the captions compact.
    return f'<td align="center" valign="top" width="33%"><img src="docs/images/{image}" width="256" alt="{name}"><br><sub><b>{caption}</b>{detail}</sub></td>'


def gallery():
    r = rows()
    lines = ['<table>']
    for i in range(0, len(r), PER_ROW):
        row = [cell(*x) for x in r[i:i + PER_ROW]]
        lines += ['<tr>'] + row + ['<td></td>'] * (PER_ROW - len(row)) + ['</tr>']   # a short last row is filled with empty cells, as README had it
    return '\n'.join(lines + ['</table>'])


def readme():
    text = OUT.read_text(encoding='utf-8')
    if text.count(BEGIN) != 1 or text.count(END) != 1: sys.exit(f'README.md: the gallery markers ({BEGIN} … {END}) must appear once each')
    head, rest = text.split(BEGIN)
    if END not in rest: sys.exit('README.md: the gallery end marker must follow the begin marker')
    return head + BEGIN + '\n' + gallery() + '\n' + END + rest.split(END, 1)[1]


if __name__ == '__main__':
    text = readme()
    if '--check' in sys.argv[1:]:
        if OUT.read_text(encoding='utf-8') != text: sys.exit('README.md: the gallery is out of date: run dm/tools/docs_gallery.py')
    else:
        OUT.write_text(text, encoding='utf-8')
